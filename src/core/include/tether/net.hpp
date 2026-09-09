#pragma once

#include "tether/event_loop.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <set>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace tether {

    // Check if tetherd is already running. Exits if another instance holds the lock.
    void ensure_single_instance();
    std::string get_runtime_dir();
    // $XDG_STATE_HOME/tether (or ~/.local/state/tether), created if absent.
    std::string get_state_dir();

    class TcpServer;
    class UnixServer;

    // Simple registry for active clients to support broadcasting
    void register_client_fd(int fd);
    void register_client_ssl(int fd, SSL* ssl);
    void unregister_client_fd(int fd);
    void broadcast_message(const std::string& msg, int exclude_fd = -1);
    void register_local_subscriber(int fd);
    void unregister_local_subscriber(int fd);
    void broadcast_local_event(const std::string& msg, int exclude_fd = -1);

    // Store an OTP and push it to local subscribers (browser extension, GTK).
    // sender_domain is the registrable domain of the email sender, "" when the
    // code did not come from email.
    void otp_publish(const std::string& code, const std::string& sender_domain = "", int exclude_fd = -1);
    void record_received_file(const std::filesystem::path& path, size_t bytes_written);
    size_t broadcast_tcp_message(const std::string& msg, int exclude_fd = -1);

    // Bluetooth state, read from the BluezMonitor snapshot.
    nlohmann::json build_bt_status();
    nlohmann::json build_bt_devices();
    nlohmann::json build_bt_airpods();

    // How many contacts a bt_list_contacts answers with when the caller does not say.
    inline constexpr size_t BT_CONTACTS_DEFAULT_LIMIT = 100;
    nlohmann::json build_bt_contacts(const std::string& query, size_t limit = BT_CONTACTS_DEFAULT_LIMIT);

    // Whether mDNS is usable (avahi-daemon is running). False until the
    // Avahi client reports otherwise. Setting it pushes an "mdns_status" event
    // to local subscribers.
    // Whether the daemon shows desktop popups at all. Off silences every one:
    // mirrored iPhone alerts, incoming messages, and arriving files.
    void set_desktop_popups_enabled(bool enabled);
    bool desktop_popups_enabled();

    void set_mdns_available(bool available);
    bool mdns_available();
    nlohmann::json build_mdns_status();

    // Whether a host firewall is running.
    bool ufw_enabled(std::string_view ufw_conf);
    bool firewall_active();

    // Whether this node should dial a peer it just saw over mDNS.
    bool should_dial_peer(const std::string& my_fingerprint, const std::string& peer_fingerprint, bool peer_is_known);

    class UnixServer {
    public:
        UnixServer(EpollEventLoop& loop, TcpServer& tcp_server);
        ~UnixServer();

        bool start();
        void stop();

    private:
        void handle_accept(int fd);
        void handle_client(int client_fd);

        EpollEventLoop& loop_;
        TcpServer& tcp_server_;
        int server_fd_ = -1;
        std::string socket_path_;
        std::map<int, std::string> client_buffers_;
    };

    class TcpServer {
    public:
        // bind_port = 0 binds to any available port
        TcpServer(EpollEventLoop& loop, int bind_port = 5134);
        ~TcpServer();

        bool start();
        void stop();

        // Trusts a fingerprint and promotes every matching live TLS session.
        // Returns true when at least one connected client was promoted.
        bool accept_device(const std::string& fingerprint, const std::string& fallback_name = "Paired Device");

        // Dial a peer and add the session into this server's client tables
        bool connect_peer(const std::string& host,
                          int port,
                          const std::string& peer_name = "",
                          const std::string& expected_fingerprint = "");

        // Drops a pairing: unpins the fingerprint and closes any live session with it.
        // Returns true when the fingerprint was pinned.
        bool forget_device(const std::string& fingerprint);

        // Invoked when the set of trusted peers gains a member with no live session.
        void set_peers_changed_callback(std::function<void()> callback);

    private:
        struct ConnectedClientInfo {
            std::string address;
            std::string fingerprint;
            std::string device_name;
            bool paired = false;
        };

        void handle_accept(int fd);
        void handle_client(int client_fd);
        void adopt_peer(int fd,
                        const std::string& host,
                        const std::string& peer_name,
                        const std::string& expected_fingerprint);
        void spawn_pair_dialog(int client_fd, SSL* ssl, const std::string& fingerprint, const std::string& device_name);
        // Marks a session trusted and wires it into the broadcast registry.
        void promote_session(int client_fd, const std::string& fingerprint, const std::string& device_name);
        // Frees the SSL session, forgets every per-client table, and closes the fd.
        void drop_client(int fd);

        EpollEventLoop& loop_;
        int server_fd_ = -1;
        int bind_port_;
        std::map<int, std::string> client_buffers_;
        std::map<int, SSL*> active_ssl_;
        std::map<int, bool> ssl_handshake_complete_;
        std::map<int, bool> client_paired_;
        std::map<int, ConnectedClientInfo> client_info_;
        // fds we dialled
        std::map<int, bool> client_initiated_;

        std::set<std::string> dialing_;

        std::function<void()> peers_changed_;

        // Track spawned dialog child processes
        struct PendingPairDialog {
            pid_t pid;
            int client_fd;
            std::string fingerprint;
            std::string device_name;
            // The device was accepted elsewhere and the dialog killed, so its
            // non-zero exit is not a refusal.
            bool superseded = false;
        };
        // keyed by the pipe read-fd we monitor in epoll
        std::map<int, PendingPairDialog> pending_dialogs_;
    };

} // namespace tether
