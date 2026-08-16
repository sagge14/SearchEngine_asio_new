#include "Auth/AuthClientStore.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void printUsage()
{
    std::cerr
        << "AuthDbTool --db <path> <command> [options]\n"
        << "Commands:\n"
        << "  add --id <id> --name <name> --flash <serial> [--disabled]\n"
        << "  update --id <id> --name <name> --flash <serial> [--disabled|--enabled]\n"
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
        std::string flash_serial;
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
            } else if (arg == "--flash") {
                flash_serial = requireArg(args, i, "--flash");
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
            if (client_id.empty() || client_name.empty() || flash_serial.empty()) {
                throw std::runtime_error(
                    command + " requires --id --name --flash");
            }
            const bool row_enabled = have_enabled ? enabled : true;
            store.upsertClient(
                client_id, client_name, flash_serial, row_enabled);
            std::cout << command << " ok: " << client_id << '\n';
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
            std::cout << row->client_id << '\t' << row->client_name << '\t'
                      << row->flash_serial << '\t'
                      << (row->enabled ? "enabled" : "disabled") << '\n';
            return 0;
        }

        if (command == "list") {
            const auto rows = store.listClients();
            for (const auto& row : rows) {
                std::cout << row.client_id << '\t' << row.client_name << '\t'
                          << row.flash_serial << '\t'
                          << (row.enabled ? "enabled" : "disabled") << '\n';
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
