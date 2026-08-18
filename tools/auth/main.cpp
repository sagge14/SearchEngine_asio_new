#include "Auth/AuthClientStore.h"
#include "Auth/DeviceIdentity.h"
#include "TokenLoader.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void printUsage()
{
    std::cerr
        << "AuthDbTool --db <path> <command> [options]\n"
        << "Commands:\n"
        << "  add --id <id> --name <name> --device-type usb|computer "
           "--device-id <id> [--disabled]\n"
        << "  update --id <id> --name <name> --device-type usb|computer "
           "--device-id <id> [--disabled|--enabled]\n"
        << "  add-from-token --token <path> [--disabled]\n"
        << "  enable --id <id>\n"
        << "  disable --id <id>\n"
        << "  remove --id <id>\n"
        << "  list\n"
        << "  get --id <id>\n";
}

std::string requireArg(
    const std::vector<std::string>& args,
    std::size_t& index,
    const char* flag)
{
    if (index + 1 >= args.size()) {
        throw std::runtime_error(std::string(flag) + " requires a value");
    }
    return args[++index];
}

std::string trimCopy(std::string value)
{
    return auth::TrimCopy(std::move(value));
}

std::string normalizeDeviceId(
    const std::string& device_type,
    std::string device_id)
{
    if (device_type == auth::kDeviceTypeUsb) {
        return auth::NormalizeUsbDeviceId(std::move(device_id));
    }
    if (device_type == auth::kDeviceTypeComputer) {
        auto uuid = auth::NormalizeComputerUuid(std::move(device_id));
        if (!uuid) {
            throw std::runtime_error(
                "computer device_id must be a usable SMBIOS UUID");
        }
        return *uuid;
    }
    throw std::runtime_error("device_type must be usb or computer");
}

void printClientRow(const auth::AuthClientRecord& row)
{
    std::cout << row.client_id << '\t' << row.client_name << '\t'
              << row.device_type << '\t' << row.device_id << '\t'
              << (row.enabled ? "enabled" : "disabled") << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }

        if (args.empty() || args[0] == "--help" || args[0] == "-h") {
            printUsage();
            return args.empty() ? 1 : 0;
        }

        std::string db_path = "auth_clients.sqlite";
        std::string command;
        std::string client_id;
        std::string client_name;
        std::string device_type;
        std::string device_id;
        std::string token_path;
        bool have_enabled = false;
        bool enabled = true;

        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (arg == "--db") {
                db_path = requireArg(args, i, "--db");
            } else if (arg == "--id") {
                client_id = requireArg(args, i, "--id");
            } else if (arg == "--name") {
                client_name = requireArg(args, i, "--name");
            } else if (arg == "--device-type") {
                device_type = requireArg(args, i, "--device-type");
            } else if (arg == "--device-id") {
                device_id = requireArg(args, i, "--device-id");
            } else if (arg == "--token") {
                token_path = requireArg(args, i, "--token");
            } else if (arg == "--disabled") {
                have_enabled = true;
                enabled = false;
            } else if (arg == "--enabled") {
                have_enabled = true;
                enabled = true;
            } else if (arg == "--help" || arg == "-h") {
                printUsage();
                return 0;
            } else if (arg.starts_with("-")) {
                throw std::runtime_error("unknown option: " + arg);
            } else if (command.empty()) {
                command = arg;
            } else {
                throw std::runtime_error("unexpected argument: " + arg);
            }
        }

        if (command.empty()) {
            printUsage();
            return 1;
        }

        auth::AuthClientStore store;
        store.open(db_path);

        if (command == "add" || command == "update") {
            device_type = trimCopy(device_type);
            if (client_id.empty() || client_name.empty() ||
                device_type.empty() || device_id.empty())
            {
                throw std::runtime_error(
                    command +
                    " requires --id --name --device-type --device-id");
            }
            const auto normalized_id = normalizeDeviceId(device_type, device_id);
            const bool row_enabled = have_enabled ? enabled : true;
            store.upsertClient(
                client_id,
                client_name,
                device_type,
                normalized_id,
                row_enabled);
            std::cout << command << " ok: " << client_id << '\n';
            return 0;
        }

        if (command == "add-from-token") {
            if (token_path.empty()) {
                throw std::runtime_error("add-from-token requires --token");
            }
            const auto fields = auth_db::loadTokenFields(token_path);
            const bool row_enabled = have_enabled ? enabled : true;
            store.upsertClient(
                fields.client_id,
                fields.client_name,
                fields.device_type,
                fields.device_id,
                row_enabled,
                fields.signature_meta);
            std::cout << "add-from-token ok: " << fields.client_id << '\t'
                      << fields.client_name << '\t' << fields.device_type
                      << '\t' << fields.device_id << '\t'
                      << (row_enabled ? "enabled" : "disabled") << '\n';
            return 0;
        }

        if (command == "enable" || command == "disable") {
            if (client_id.empty()) {
                throw std::runtime_error(command + " requires --id");
            }
            store.setEnabled(client_id, command == "enable");
            std::cout << command << " ok: " << client_id << '\n';
            return 0;
        }

        if (command == "remove") {
            if (client_id.empty()) {
                throw std::runtime_error("remove requires --id");
            }
            store.removeClient(client_id);
            std::cout << "remove ok: " << client_id << '\n';
            return 0;
        }

        if (command == "get") {
            if (client_id.empty()) {
                throw std::runtime_error("get requires --id");
            }
            const auto row = store.getClient(client_id);
            if (!row) {
                std::cerr << "not found: " << client_id << '\n';
                return 2;
            }
            printClientRow(*row);
            return 0;
        }

        if (command == "list") {
            const auto rows = store.listClients();
            for (const auto& row : rows) {
                printClientRow(row);
            }
            std::cout << rows.size() << " client(s)\n";
            return 0;
        }

        throw std::runtime_error("unknown command: " + command);
    } catch (const std::exception& ex) {
        std::cerr << "AuthDbTool error: " << ex.what() << '\n';
        return 1;
    }
}
