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
    inline constexpr int kAuthClientsSchemaUserVersion = 2;

    struct AuthClientRecord
    {
        std::string client_id;
        std::string client_name;
        std::string device_type;
        std::string device_id;
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
            const std::string& device_type,
            const std::string& device_id,
            bool enabled = true,
            const std::string& signature_meta = {},
            bool reject_identity_conflict = false,
            bool preserve_enabled = false);

        [[nodiscard]] std::optional<AuthClientRecord> getClient(
            const std::string& client_id) const;

        [[nodiscard]] std::vector<AuthClientRecord> listClients() const;

        void beginTransaction();
        void commitTransaction();
        void rollbackTransaction() noexcept;

        void setEnabled(const std::string& client_id, bool enabled);
        void removeClient(const std::string& client_id);

        [[nodiscard]] std::optional<AuthClientRecord> findEnabledMatch(
            const std::string& client_id,
            const std::string& client_name,
            const std::string& device_type,
            const std::string& device_id) const;

    private:
        void ensureSchema();
        void createSchemaV2();
        void rejectIncompatibleSchema() const;
        void execOrThrow(const char* sql) const;
        [[nodiscard]] int readUserVersion() const;
        [[nodiscard]] bool clientsTableExists() const;
        [[nodiscard]] std::vector<std::string> readClientColumns() const;
        [[nodiscard]] static std::int64_t nowUnix();

        mutable std::mutex mutex_;
        sqlite3* db_{nullptr};
        std::filesystem::path path_;
    };
}
