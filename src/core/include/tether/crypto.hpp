#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <vector>

namespace tether {

    class Crypto {
    public:
        static Crypto& instance();

        bool init();
        SSL_CTX* get_server_context();
        SSL_CTX* get_client_context();

        std::string get_my_fingerprint();
        bool is_host_known(const std::string& fingerprint);
        void add_known_host(const std::string& name, const std::string& fingerprint);
        bool remove_known_host(const std::string& fingerprint);
        std::string get_known_hosts_dump() const;

        static std::string get_peer_fingerprint(SSL* ssl);
        static std::string generate_fingerprint(X509* cert);

        void reset_for_tests();

    private:
        Crypto() = default;
        ~Crypto();

        bool ensure_certificates();

        // All three assume the caller holds hosts_mutex_.
        void load_known_hosts();
        void save_known_hosts();
        void refresh_known_hosts_if_stale();

        std::string cert_path_;
        std::string key_path_;
        std::string hosts_path_;

        // fingerprint -> device name. Cached in memory because is_host_known() runs on
        // every TLS handshake and used to re-read and re-parse known_hosts.json each
        // time. Revalidated by mtime, since `tether --accept` writes the file from a
        // separate process and the daemon must notice.
        mutable std::mutex hosts_mutex_;
        std::map<std::string, std::string> known_hosts_;
        std::filesystem::file_time_type hosts_mtime_{};
        bool hosts_loaded_ = false;

        SSL_CTX* server_ctx_ = nullptr;
        SSL_CTX* client_ctx_ = nullptr;

        std::string my_fingerprint_;
    };

} // namespace tether
