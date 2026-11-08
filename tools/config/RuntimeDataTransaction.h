#pragma once

#include <string>
#include <vector>

namespace runtime_data_transaction {

int applyCommand(const std::vector<std::wstring>& args);
int rollbackCommand(const std::vector<std::wstring>& args);
int commitCommand(const std::vector<std::wstring>& args);

int settingsApplyCommand(const std::vector<std::wstring>& args);
int settingsRollbackCommand(const std::vector<std::wstring>& args);
int settingsCommitCommand(const std::vector<std::wstring>& args);

} // namespace runtime_data_transaction
