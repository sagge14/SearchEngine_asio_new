#include "Auth/AuthClientStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
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
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());
    return value;
}

std::string normalizeFlashSerial(std::string value)
{
    value = trimCopy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

struct TokenFields
{
    std::string client_id;
    std::string client_name;
    std::string flash_serial;
    std::string signature_meta;
};

TokenFields loadTokenFields(const std::string& token_path)
{
    std::ifstream input(token_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open token file: " + token_path);
    }

    nlohmann::json document;
    try {
        input >> document;
    } catch (const std::exception& ex) {
        throw std::runtime_error(
            std::string("token JSON parse failed: ") + ex.what());
    }

    if (!document.is_object()) {
        throw std::runtime_error("token payload must be a JSON object");
    }
    if (document.value("format", std::string()) != "searchclient-auth-token") {
        throw std::runtime_error(
            "token format must be searchclient-auth-token");
    }
    if (document.value("format_version", 0) != 1) {
        throw std::runtime_error("token format_version must be 1");
    }

    TokenFields fields;
    fields.client_id = trimCopy(document.value("client_id", std::string()));
    fields.client_name = trimCopy(document.value("client_name", std::string()));
    fields.flash_serial = normalizeFlashSerial(
        document.value("flash_serial", std::string()));

    if (fields.client_id.empty() || fields.client_name.empty() ||
        fields.flash_serial.empty())
    {
        throw std::runtime_error(
            "token requires non-empty client_id, client_name, flash_serial");
    }

    if (document.contains("signature")) {
        if (!document.at("signature").is_object()) {
            throw std::runtime_error("token signature must be a JSON object");
        }
        const auto alg = document.at("signature").value("alg", std::string());
        if (!alg.empty() && alg != "none") {
            throw std::runtime_error(
                "token signature.alg must be empty or \"none\" for stage 1");
        }
        fields.signature_meta = document.at("signature").dump();
    }

    return fields;
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
            } else if (arg == "--flash") {
                flash_serial = requireArg(args, i, "--flash");
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
            if (client_id.empty() || client_name.empty() || flash_serial.empty()) {
                throw std::runtime_error(
                    command + " requires --id --name --flash");
            }
            const bool row_enabled = have_enabled ? enabled : true;
            store.upsertClient(
                client_id,
                client_name,
                normalizeFlashSerial(flash_serial),
                row_enabled);
            std::cout << command << " ok: " << client_id << '\n';
            return 0;
        }

        if (command == "add-from-token") {
            if (token_path.empty()) {
                throw std::runtime_error("add-from-token requires --token");
            }
            const auto fields = loadTokenFields(token_path);
            const bool row_enabled = have_enabled ? enabled : true;
            store.upsertClient(
                fields.client_id,
                fields.client_name,
                fields.flash_serial,
                row_enabled,
                fields.signature_meta);
            std::cout << "add-from-token ok: " << fields.client_id << '\t'
                      << fields.client_name << '\t' << fields.flash_serial
                      << '\t' << (row_enabled ? "enabled" : "disabled") << '\n';
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
