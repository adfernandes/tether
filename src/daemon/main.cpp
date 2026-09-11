#include "notification.hpp"
#include <csignal>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sys/timerfd.h>
#include <tether/audio.hpp>
#include <tether/bluetooth/airpods.hpp>
#include <tether/bluetooth/config.hpp>
#include <tether/bluetooth/connection.hpp>
#include <tether/bluetooth/contacts.hpp>
#include <tether/bluetooth/monitor.hpp>
#include <tether/bluetooth/pairing.hpp>
#include <tether/core.hpp>
#include <tether/crypto.hpp>
#include <tether/discovery.hpp>
#include <tether/event_loop.hpp>
#include <tether/file_transfer.hpp>
#include <tether/i18n.hpp>
#include <tether/log.hpp>
#include <tether/mpris.hpp>
#include <tether/net.hpp>
#include <tether/otp.hpp>
#include <tether/secret_store.hpp>
#include <tether/wayland.hpp>
#include <unistd.h>

tether::EpollEventLoop* g_loop = nullptr;

void signal_handler(int) {
    if (g_loop) {
        debug::log(INFO, "\nStopping tetherd...");
        g_loop->stop();
        g_loop = nullptr;
    }
}

// Append stderr/stdout to the state-dir log unless attached to a terminal.
static void redirect_output_to_log() {
    if (isatty(STDERR_FILENO))
        return;
    try {
        const std::string log_path = tether::get_state_dir() + "/tetherd.log";
        if (freopen(log_path.c_str(), "a", stderr) == nullptr)
            return;
        if (freopen(log_path.c_str(), "a", stdout) == nullptr) {
        }
    } catch (const std::exception&) {
        // Losing the log is survivable; failing to start is not.
    }
}

// How long to let the phone finish with the buds before reclaiming them.
constexpr int HANDOFF_SETTLE_SECONDS = 2;
constexpr int HANDOFF_RECLAIM_ATTEMPTS = 3;
// The phone goes quiet mid-call, so a peer that let go is checked again after this before the buds are taken back.
constexpr int HANDOFF_GUARD_SECONDS = 3;
// How long to wait for the buds' sink to come back before resuming anything.
constexpr int SINK_RETURN_TIMEOUT_MS = 8000;
// HACK: After a rebuilt Bluetooth sink reappears, time for the audio server to move streams back onto it.
constexpr int SINK_SETTLE_SECONDS = 1;

int main(int argc, char** argv) {
    redirect_output_to_log();
    tether::init_locale();
    debug::log(INFO, "tetherd version {}", tether::get_version());

    try {
        tether::ensure_single_instance();
    } catch (const std::exception& e) {
        debug::log(ERR, "Initialization error: {}", e.what());
        return 1;
    }

    if (!tether::Crypto::instance().init()) {
        debug::log(ERR, "Fatal: Failed to initialize OpenSSL mTLS engine.");
        return 1;
    }

    // Capture signals gracefully
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE to prevent crash on closed connections

    tether::EpollEventLoop loop;
    g_loop = &loop;

    tether::TcpServer tcp_srv(loop, 5134);
    tether::UnixServer unix_srv(loop, tcp_srv);
    if (!unix_srv.start()) {
        debug::log(ERR, "Failed to start Unix server");
        return 1;
    }

    if (!tcp_srv.start()) {
        debug::log(ERR, "Failed to start TCP server");
        return 1;
    }

    // Advertise this daemon on the local network via mDNS
    tether::Discovery discovery;
    {
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname) - 1);
        std::string my_fp = tether::Crypto::instance().get_my_fingerprint();

        discovery.set_state_callback([](bool available) { tether::set_mdns_available(available); });
        discovery.publish(hostname, 5134, my_fp);

        discovery.start_continuous_browse([&loop, &tcp_srv](const std::vector<tether::DiscoveredDevice>& devices) {
            // Reestablish sessions with peers we already trust.
            const std::string my_fp = tether::Crypto::instance().get_my_fingerprint();
            for (const auto& dev : devices) {
                if (dev.addresses.empty())
                    continue;
                if (!tether::should_dial_peer(
                        my_fp, dev.fingerprint, tether::Crypto::instance().is_host_known(dev.fingerprint)))
                    continue;

                // on avahi's poll thread
                const auto& addr = dev.addresses.front();
                loop.post([&tcp_srv, host = addr.address, port = addr.port, name = dev.name, fp = dev.fingerprint]() {
                    tcp_srv.connect_peer(host, port, name, fp);
                });
            }

            nlohmann::json payload;
            payload["command"] = "discovery_result";
            payload["devices"] = nlohmann::json::array();
            for (const auto& dev : devices) {
                nlohmann::json d;
                d["name"] = dev.name;
                d["fingerprint"] = dev.fingerprint;
                d["addresses"] = nlohmann::json::array();
                for (const auto& addr : dev.addresses) {
                    nlohmann::json a;
                    a["address"] = addr.address;
                    a["port"] = addr.port;
                    d["addresses"].push_back(a);
                }
                payload["devices"].push_back(d);
            }
            tether::broadcast_local_event(payload.dump());
        });

        tcp_srv.set_peers_changed_callback([&discovery] { discovery.refresh(); });
    }

    tether::WaylandContext wayland_srv(loop);
    tether::g_wayland = &wayland_srv;
    if (wayland_srv.init()) {
        wayland_srv.set_clipboard_callback([](const std::string& text) {
            nlohmann::json j;
            j["command"] = "clipboard_updated";
            j["content"] = text;
            // replace bad UTF-8 instead of throwing; a clipboard
            // app can still mislabel binary as text/plain. Don't abort the daemon.
            tether::broadcast_message(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        });
    }

    tether::FileReceiveManager file_mgr;
    tether::DesktopNotifier notifier;
    const bool notifier_ready = notifier.init();
    if (!notifier_ready) {
        debug::log(ERR, "Warning: desktop notifications unavailable");
    } else {
        file_mgr.set_on_complete([&notifier](const std::filesystem::path& path, size_t bytes_written) {
            notifier.notify_file_arrived(path);
            tether::record_received_file(path, bytes_written);
        });
    }
    tether::g_file_manager = &file_mgr;

    notifier.set_copy_handler([&loop](const std::string& code) {
        // libnotify dispatches actions on its own thread; the clipboard belongs to the loop.
        loop.post([code] {
            if (tether::g_wayland)
                tether::g_wayland->copy_to_clipboard(code);
        });
    });

    if (notifier_ready) {
        int mdns_warn_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (mdns_warn_fd >= 0) {
            itimerspec spec{};
            spec.it_value.tv_sec = 15;
            timerfd_settime(mdns_warn_fd, 0, &spec, nullptr);
            loop.addFd(mdns_warn_fd, [&loop, &notifier](int fd) {
                uint64_t ticks = 0;
                ssize_t ignored = read(fd, &ticks, sizeof(ticks));
                (void)ignored;
                loop.removeFd(fd);
                close(fd);
                if (tether::mdns_available())
                    return;
                debug::log(ERR, "mDNS: still unavailable after 15s; notifying the user");
                notifier.notify({_("Tether"),
                                 _("This PC can't be discovered"),
                                 _("avahi-daemon isn't running, so Tether can't advertise itself on the "
                                   "network. Start it with: sudo systemctl enable --now avahi-daemon"),
                                 {"network-wireless-offline", "network-offline", "dialog-warning"},
                                 false,
                                 ""});
            });
        }
    }

    // BlueZ runs on its own GLib thread and wakes the loop through an eventfd
    // whenever the adapter or device set changes.
    tether::bluetooth::BluezMonitor bluez;
    tether::bluetooth::ConnectionManager connections(
        bluez,
        [](const nlohmann::json& status) { tether::broadcast_local_event(status.dump()); },
        [&notifier](const tether::bluetooth::Message& message, bool backfill) {
            nlohmann::json event = tether::bluetooth::to_json(message);
            event["command"] = "bt_message";
            tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));

            std::string otp;
            if (!backfill && !message.outgoing) {
                otp = tether::otp_extract(message.body);
                tether::otp_publish(otp);
            }

            // ANCS deliberately shows no popup for Messages, because MAP is the
            // copy whose read state stays in sync with the phone. This is that popup.
            if (backfill || message.outgoing || message.read)
                return;

            std::string who;
            {
                std::lock_guard<std::mutex> lock(tether::bluetooth::message_store_mutex());
                who = tether::bluetooth::contact_store().name_for(message.thread_key);
            }
            if (who.empty())
                who = message.peer_name.empty() ? message.peer_address : message.peer_name;
            tether::bluetooth::ancs::Notification as_notification;
            as_notification.app_id = tether::bluetooth::ancs::APP_ID_MESSAGES;
            // The reply action is only offered for a thread that has a route back.
            tether::bluetooth::Recipient recipient;
            std::string reply_error;
            const bool repliable =
                tether::bluetooth::recipient_from_thread_key(message.thread_key, recipient, reply_error);
            notifier.notify({_("Messages"),
                             who.empty() ? "iPhone" : who,
                             message.body,
                             tether::bluetooth::ancs::icon_candidates(as_notification),
                             false,
                             repliable ? message.thread_key : std::string{},
                             otp});
        });

    auto bt_config = tether::bluetooth::load_config();

    tether::secret::set_retention(bt_config.retention);
    tether::set_desktop_popups_enabled(bt_config.desktop_popups_enabled);

    // Pick the controller before the first capability, a second adapter never comes up bound to the wrong one.
    bluez.set_preferred_adapter(bt_config.adapter);

    // Handing the buds to the iPhone for a call and taking them back after.
    struct Handoff {
        mutable std::mutex mutex;
        std::string released;
        bool busy = false;
        bool own_output = false;

        bool holding_buds() const {
            std::lock_guard<std::mutex> lock(mutex);
            return !released.empty();
        }
    };
    auto handoff = std::make_shared<Handoff>();

    // Set when the controller presents itself as Apple hardware, which is what makes
    // the buds offer ownership to this machine instead of only to the phone.
    std::atomic<bool> native_handoff{false};

    // Apple's own handoff. The buds say when the phone has taken them, and ownership
    // moves over AAP with the link left up.
    const auto run_ownership = [handoff](const tether::bluetooth::AirPodsState& state) {
        const auto config = tether::bluetooth::load_config();

        std::string target;
        bool released_now = false;
        {
            std::lock_guard<std::mutex> lock(handoff->mutex);
            if (handoff->busy)
                return;
            released_now = !handoff->released.empty();
            target = released_now ? handoff->released : state.address;
        }
        if (target.empty() || !tether::bluetooth::g_airpods)
            return;

        const auto action = tether::bluetooth::ownership_action(
            state.peer_active, released_now, config.airpods_handoff && config.airpods_enabled);

        if (action == tether::bluetooth::HandoffAction::Release) {
            if (tether::g_media)
                tether::g_media->pause();
            const bool ours = tether::audio::default_sink() == tether::audio::bluez_sink(target);
            {
                std::lock_guard<std::mutex> lock(handoff->mutex);
                handoff->released = target;
                handoff->own_output = ours;
            }
            tether::bluetooth::g_airpods->set_ownership(false);
            debug::log(INFO, "airpods: gave the buds to the iPhone");
            return;
        }
        if (action != tether::bluetooth::HandoffAction::Reclaim)
            return;

        {
            std::lock_guard<std::mutex> lock(handoff->mutex);
            handoff->busy = true;
        }
        std::thread([handoff] {
            bool reclaimed = false;
            for (int attempt = 0; attempt < HANDOFF_RECLAIM_ATTEMPTS && !reclaimed; ++attempt) {
                std::this_thread::sleep_for(std::chrono::seconds(HANDOFF_GUARD_SECONDS));
                if (!tether::bluetooth::g_airpods)
                    break;
                // The firmware reports no audio source periodically during a call.
                if (tether::bluetooth::g_airpods->state().peer_active)
                    continue;
                tether::bluetooth::g_airpods->set_ownership(true);
                reclaimed = true;
            }
            if (reclaimed) {
                bool ours = false;
                std::string address;
                {
                    std::lock_guard<std::mutex> lock(handoff->mutex);
                    ours = handoff->own_output;
                    address = handoff->released;
                }
                if (ours)
                    tether::audio::restore_default_sink(address, SINK_RETURN_TIMEOUT_MS);
                if (tether::g_media)
                    tether::g_media->resume();
            }
            debug::log(reclaimed ? INFO : WARN,
                       reclaimed ? "airpods: took the buds back from the iPhone"
                                 : "airpods: the iPhone still has the buds; leaving them to it");
            std::lock_guard<std::mutex> lock(handoff->mutex);
            if (reclaimed)
                handoff->released.clear();
            handoff->busy = false;
        }).detach();
    };

    // AirPods battery arrives on its own L2CAP channel, not through BlueZ.
    tether::MediaControl media;
    tether::bluetooth::EarState last_ear;
    bool last_peer_call = false;
    bool paused_for_call = false;
    tether::bluetooth::AirPodsWatcher airpods(
        [&media, &last_ear, &last_peer_call, &paused_for_call, handoff, &native_handoff, &run_ownership](
            const tether::bluetooth::AirPodsState& state) {
            const auto config = tether::bluetooth::load_config();
            const auto mode = config.airpods_enabled ? config.airpods_pause : tether::bluetooth::PauseMode::Never;
            switch (tether::bluetooth::ear_media_action(last_ear, state.ear, mode, media.holding())) {
            case tether::bluetooth::MediaAction::Pause:
                if (const size_t paused = media.pause())
                    debug::log(INFO, "airpods: paused {} player(s), a bud came out", paused);
                break;
            case tether::bluetooth::MediaAction::Resume:
                if (const size_t resumed = media.resume())
                    debug::log(INFO, "airpods: resumed {} player(s), the buds went back in", resumed);
                break;
            case tether::bluetooth::MediaAction::None:
                break;
            }

            bool resume_after_call = false;
            switch (tether::bluetooth::call_media_action(
                last_peer_call, state.peer_call, config.airpods_enabled && config.airpods_handoff, paused_for_call)) {
            case tether::bluetooth::MediaAction::Pause:
                if (const size_t paused = media.pause()) {
                    paused_for_call = true;
                    debug::log(INFO, "airpods: paused {} player(s), the iPhone has a call", paused);
                }
                break;
            case tether::bluetooth::MediaAction::Resume:
                paused_for_call = false;
                // Resuming without the buds would move the audio to the speakers.
                resume_after_call = !state.address.empty();
                break;
            case tether::bluetooth::MediaAction::None:
                break;
            }

            // HACK: The phone tore down this machine's transport when it took the buds, and PipeWire does
            // not reopen it, so a sink left suspended with a stream on it is rebuilt. Players stay
            // paused meanwhile: with the card off their streams fall back to the speakers.
            if (last_peer_call && !state.peer_call && !state.address.empty()) {
                std::thread([address = state.address, resume = resume_after_call] {
                    const bool stuck = tether::audio::default_sink() == tether::audio::bluez_sink(address) &&
                                       tether::audio::bluez_sink_stuck(address);
                    if (stuck) {
                        debug::log(INFO, "airpods: the Bluetooth sink stayed suspended after the call; rebuilding it");
                        if (tether::g_media)
                            tether::g_media->pause();
                        if (!tether::audio::restart_bluez_card(address) ||
                            !tether::audio::restore_default_sink(address, SINK_RETURN_TIMEOUT_MS)) {
                            debug::log(WARN, "airpods: could not rebuild the Bluetooth sink for {}", address);
                            if (tether::g_media)
                                tether::g_media->forget();
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(SINK_SETTLE_SECONDS));
                    }
                    if ((resume || stuck) && tether::g_media) {
                        if (const size_t resumed = tether::g_media->resume())
                            debug::log(INFO, "airpods: resumed {} player(s), the call ended", resumed);
                    }
                }).detach();
            }

            if (state.address.empty() && !handoff->holding_buds()) {
                media.forget();
                paused_for_call = false;
            }

            if (native_handoff.load())
                run_ownership(state);

            last_ear = state.ear;
            last_peer_call = state.peer_call;

            tether::broadcast_local_event(tether::bluetooth::to_json(state).dump());
        });
    const auto run_handoff = [&bluez, &media, handoff, &native_handoff](const nlohmann::json& calls) {
        // Ownership handoff runs off the buds' own peer state, not the call list.
        if (native_handoff.load())
            return;

        const auto config = tether::bluetooth::load_config();
        const bool active = tether::bluetooth::call_wants_audio(calls);

        std::string target;
        bool released_now = false;
        {
            std::lock_guard<std::mutex> lock(handoff->mutex);
            if (handoff->busy)
                return;
            released_now = !handoff->released.empty();
            target = handoff->released;
        }

        // The snapshot has to outlive the pointer into it.
        const auto snapshot = bluez.snapshot();
        const auto* device = tether::bluetooth::find_airpods(snapshot);
        if (!released_now && device)
            target = device->address;

        const auto action = tether::bluetooth::handoff_action(
            active, device != nullptr, released_now, config.airpods_handoff && config.airpods_enabled);
        if (action == tether::bluetooth::HandoffAction::None || target.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(handoff->mutex);
            handoff->busy = true;
        }

        if (action == tether::bluetooth::HandoffAction::Release) {
            media.pause();
            const bool ours = tether::audio::default_sink() == tether::audio::bluez_sink(target);
            {
                std::lock_guard<std::mutex> lock(handoff->mutex);
                handoff->released = target;
                handoff->own_output = ours;
            }
            std::string err;
            const bool ok = tether::bluetooth::disconnect_device(bluez, target, err);
            std::lock_guard<std::mutex> lock(handoff->mutex);
            handoff->busy = false;
            if (ok) {
                debug::log(INFO, "airpods: handed the buds to the iPhone for a call");
            } else {
                handoff->released.clear();
                debug::log(WARN, "airpods: could not release the buds: {}", err);
            }
            return;
        }

        std::thread([handoff, target] {
            // The phone does not drop them the instant the call ends.
            std::this_thread::sleep_for(std::chrono::seconds(HANDOFF_SETTLE_SECONDS));
            bool ok = false;
            for (int attempt = 0; attempt < HANDOFF_RECLAIM_ATTEMPTS && !ok; ++attempt) {
                std::string err;
                ok = tether::bluetooth::g_bluez &&
                     tether::bluetooth::connect_device(*tether::bluetooth::g_bluez, target, err);
                if (!ok) {
                    debug::log(DEBUG, "airpods: reclaim attempt {} failed: {}", attempt + 1, err);
                    std::this_thread::sleep_for(std::chrono::seconds(HANDOFF_SETTLE_SECONDS));
                }
            }
            bool ours = false;
            {
                std::lock_guard<std::mutex> lock(handoff->mutex);
                ours = handoff->own_output;
            }
            // The buds are back on the link, but their sink takes a moment longer.
            if (ok && ours)
                tether::audio::restore_default_sink(target, SINK_RETURN_TIMEOUT_MS);
            if (tether::g_media) {
                if (ok)
                    tether::g_media->resume();
                else
                    tether::g_media->forget();
            }
            debug::log(ok ? INFO : WARN,
                       ok ? "airpods: took the buds back after the call"
                          : "airpods: the buds did not come back; leaving them to the phone");
            std::lock_guard<std::mutex> lock(handoff->mutex);
            handoff->released.clear();
            handoff->busy = false;
        }).detach();
    };

    const auto follow_airpods = [&bluez, &airpods, &native_handoff]() {
        const auto snapshot = bluez.snapshot();
        const auto* device = tether::bluetooth::find_airpods(snapshot);
        // The buds report this machine back in their own peer list.
        const auto* adapter = tether::bluetooth::preferred_adapter(snapshot, bluez.preferred_adapter_id());
        native_handoff.store(adapter != nullptr && adapter->presents_as_apple());
        airpods.set_device(device ? device->address : std::string{},
                           device ? device->name : std::string{},
                           adapter ? adapter->address : std::string{});
    };
    airpods.set_enabled(bt_config.airpods_enabled);

    if (bluez.start()) {
        tether::bluetooth::g_bluez = &bluez;
        tether::bluetooth::g_airpods = &airpods;
        tether::g_media = &media;
        loop.addFd(bluez.event_fd(), [&bluez, &follow_airpods](int) {
            bluez.drain();
            tether::broadcast_local_event(tether::build_bt_devices().dump());
            tether::broadcast_local_event(tether::build_bt_status().dump());
            follow_airpods();
        });
        follow_airpods();
        auto cap = bluez.capability();
        debug::log(INFO, "Bluetooth: {} mode", tether::bluetooth::to_string(cap.mode));
        for (const auto& reason : cap.reasons)
            debug::log(INFO, "Bluetooth: {}", reason);

        // Supervise the selected iPhone's bearers and OBEX sessions. ANCS is only
        // attempted when the controller can carry it and pairing produced a bond
        // that covers LE.
        tether::bluetooth::g_bt_connections = &connections;

        tether::bluetooth::set_group_replies_enabled(bt_config.group_messages_enabled &&
                                                     bt_config.ancs_content_enabled && bt_config.ancs_enabled);
        tether::bluetooth::reload_group_rosters();

        {
            connections.set_notification_handlers(
                [&notifier](const tether::bluetooth::ancs::Notification& notification) {
                    // Messages notifications are the only side channel that says
                    // which conversation a MAP message belongs to.
                    if (!notification.pre_existing && notification.app_id == tether::bluetooth::ancs::APP_ID_MESSAGES) {
                        tether::bluetooth::observe_message_notification(notification.title,
                                                                        notification.subtitle,
                                                                        notification.body,
                                                                        static_cast<int64_t>(std::time(nullptr)));
                    }

                    // A code in the backlog is stale.
                    std::string otp;
                    if (!notification.pre_existing)
                        otp = tether::otp_extract(notification.title + "\n" + notification.subtitle + "\n" +
                                                  notification.body);
                    tether::otp_publish(otp);

                    nlohmann::json event = tether::bluetooth::ancs::to_json(notification);
                    event["command"] = "bt_notification";
                    tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));

                    // Messages already arrive over MAP with read state that stays
                    // in sync, so a popup here would be the second copy.
                    if (!tether::bluetooth::ancs::should_show_desktop_popup(notification))
                        return;

                    const std::string title = notification.title.empty() ? notification.app_name : notification.title;
                    // Some apps put the whole notification in the subtitle and leave the message empty.
                    const std::string body = notification.body.empty() ? notification.subtitle : notification.body;
                    notifier.notify({notification.app_name,
                                     title.empty() ? "iPhone" : title,
                                     body,
                                     tether::bluetooth::ancs::icon_candidates(notification),
                                     notification.silent,
                                     "",
                                     otp,
                                     notification.app_id});
                },
                [](uint32_t uid) {
                    nlohmann::json event;
                    event["command"] = "bt_notification_removed";
                    event["uid"] = uid;
                    tether::broadcast_local_event(event.dump());
                });
        }

        connections.set_call_handler([&run_handoff](const nlohmann::json& calls) {
            run_handoff(calls);

            nlohmann::json event;
            event["command"] = "bt_calls";
            event["calls"] = calls;
            tether::broadcast_local_event(event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        });

        connections.start(tether::bluetooth::supervised_address(bt_config), bt_config.ancs_enabled);
    } else {
        debug::log(INFO, "Bluetooth unavailable; messages and notifications are disabled");
    }

    debug::log(INFO, "tetherd is running. Press Ctrl-C to stop.");
    loop.run();

    // discovery destructor calls unpublish() automatically
    debug::log(INFO, "tetherd shutdown complete.");

    // explicitly null globals to be safe during final stack unwinding
    tether::bluetooth::g_bt_connections = nullptr;
    connections.stop();
    tether::bluetooth::g_bluez = nullptr;
    bluez.stop();
    tether::g_wayland = nullptr;
    tether::g_file_manager = nullptr;
    g_loop = nullptr;

    return 0;
}
