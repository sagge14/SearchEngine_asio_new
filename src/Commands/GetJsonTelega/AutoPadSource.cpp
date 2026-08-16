#include "AutoPadSource.h"

#include "nlohmann/json.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
    nlohmann::json telegaToJson(const Telega& value)
    {
        return nlohmann::json{
            {"num", value.num},
            {"type", value.type},
            {"from_to", value.from_to},
            {"isp", value.isp},
            {"podp_num", value.podp_num},
            {"date", value.date},
            {"date_podp", value.date_podp},
            {"dir", value.dir},
            {"rel", value.rel},
            {"tel_num", value.tel_num},
            {"kr", value.kr},
            {"pril_name", value.pril_name},
            {"pril_count", value.pril_count},
            {"blank", value.blank},
            {"gde_sht", value.gde_sht},
            {"last_mesto", value.last_mesto},
            {"deleted", value.deleted},
        };
    }
}

command_execution::CommandResult buildTelegiJsonFromSearchHits(
    const listAnswer& results)
{
    std::set<std::pair<std::string, Telega::TYPE>> set_num_type;
    std::vector<Telega> telegi;
    telegi.reserve(results.size());

    bool prmLoaded = false;
    bool prdLoaded = false;

    for (const auto& item : results)
    {
        auto fs_path = std::filesystem::path{item.path};
        auto num = Telega::getNumFromFileName(fs_path);
        if (num.empty())
            continue;

        auto type = Telega::getTypeFromDir(fs_path);
        if (!Telega::isSourceConfigured(type))
            continue;

        try {
            if (type == Telega::TYPE::VHOD && !prmLoaded) {
                Telega::ensureBasesLoaded(Telega::TYPE::VHOD);
                prmLoaded = true;
            }
            else if (type == Telega::TYPE::ISHOD && !prdLoaded) {
                Telega::ensureBasesLoaded(Telega::TYPE::ISHOD);
                prdLoaded = true;
            }
        }
        catch (const Telega::SourceError& error) {
            return mapAutoPadSourceError(error);
        }

        if (!set_num_type.insert({num, type}).second)
            continue;

        try {
            telegi.emplace_back(fs_path, item.relevance, item.deleted);
        }
        catch (const Telega::SourceError& error) {
            return mapAutoPadSourceError(error);
        }
        catch (const std::exception& error) {
            return command_execution::CommandResult::failure(
                command_execution::ErrorCode::DataSourceUnavailable,
                std::string(Telega::sourceLabel(type)) +
                    " data source is unavailable: " + error.what());
        }
    }

    nlohmann::json jsonTelegi = nlohmann::json::array();
    for (const auto& telega : telegi)
        jsonTelegi.push_back(telegaToJson(telega));
    const std::string jsonString = jsonTelegi.dump();
    return command_execution::CommandResult::success(
        {jsonString.begin(), jsonString.end()});
}
