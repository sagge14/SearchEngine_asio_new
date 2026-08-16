#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace auth
{
    struct AuthClientRecord
    {
        std::string client_id;
        std::string client_name;
        std::string flash_serial;
        bool enabled{true};
        std::string signature_meta;
        std::int64_t created_at{0};
        std::int64_t updated_at{0};
    };

    class AuthClientStore
    {
    public:
        AuthClientStore() = default;
        AuthClientStore(const AuthClientStore&) = delete;
        AuthClientStore& operator=(const AuthClientStore&) = delete;
        ~AuthClientStore();

        void open(const std::filesystem::path& db_path);
        void close() noexcept;
        [[nodiscard]] bool isOpen() const noexcept;

        void upsertClient(
            const std::string& client_id,
            const std::string& client_name,
            const std::string& flash_serial,
            bool enabled = true,
            const std::string& signature_meta = {});

        [[nodiscard]] std::optional<AuthClientRecord> getClient(
            const std::string& client_id) const;

        [[nodiscard]] std::vector<AuthClientRecord> listClients() const;

        void setEnabled(const std::string& client_id, bool enabled);
        void removeClient(const std::string& client_id);

        [[nodiscard]] std::optional<AuthClientRecord> findEnabledMatch(
            const std::string& client_id,
            const std::string& client_name,
            const std::string& flash_serial) const;

    private:
        void ensureSchema();
        void execOrThrow(const char* sql) const;
        [[nodiscard]] static std::int64_t nowUnix();

        mutable std::mutex mutex_;
        sqlite3* db_{nullptr};
        std::filesystem::path path_;
    };
}
