#pragma once
#include "IFileEventCommand.h"
#include "OpdateOpisBaseCommand/RecordProcessor.h"
#include "scheduler/PeriodicTaskManager.h"
#include <memory>
#include <algorithm>
#include <filesystem>
#include "MyUtils/Encoding.h"

template<typename TaskID>
class UpdateOpisBaseCommand : public IFileEventCommand {
public:
    UpdateOpisBaseCommand(PeriodicTaskManager<TaskID>& ptm,
                          std::vector<std::string>      ext )
            : periodicTaskManager_{ptm}, allowed_exts_{std::move(ext)} {}

    /*-------------------------------------------------------------*/
    void execute(const std::wstring& path) override {
        auto fs_path = std::filesystem::path(path);
     //   std::wcout << L"[opis_command] Execute started for: " << path << std::endl;

        // ── фильтры ──────────────────────────────
        if (containsOUT(fs_path)) {
            std::wcout << L"[opis_command] Skipped (contains OUT): " << path << std::endl;
            return;
        }
        if (!std::filesystem::is_regular_file(fs_path)) {
            std::wcout << L"[opis_command] Skipped (not a regular file): " << path << std::endl;
            return;
        }
        if (!isExtensionAllowed(fs_path)) {
            std::wcout << L"[opis_command] Skipped (extension not allowed): " << path << std::endl;
            return;
        }

        // ── создаём RecordProcessor ───────────────
        Telega::TYPE type    = Telega::getTypeFromDir(fs_path);
        int          num     = std::atoi(Telega::getNumFromFileName(path).c_str());
        bool need_kr_update  = (type == Telega::TYPE::VHOD);

        std::wcout << L"[opis_command] RecordProcessor created: type=" << static_cast<int>(type)
                   << L", num=" << num
                   << L", need_kr_update=" << std::boolalpha << need_kr_update << std::endl;

        auto safe_creat_RP = [type, num, need_kr_update]() {

            std::wstringstream ss;

            try {
                auto rp = std::make_shared<RecordProcessor>(type, num, need_kr_update);
                return std::move(rp);
            }
            catch (const std::exception& e) {
                ss << L"Exception in delayed action for num=" << num << L": " << e.what() << std::endl;
            }
            catch (...) {
                ss << L"Unknown exception in delayed action for num=" << num << std::endl;
            }

            std::wstring ws = ss.str();
            std::string utf8msg = encoding::wstring_to_utf8(ws);
            throw std::runtime_error(utf8msg);
        };


        //      auto rp = std::make_shared<RecordProcessor>(type, num, need_kr_update);
        auto rp = safe_creat_RP();
      //  std::wcout << L"[opis_command] RecordProcessor created =" << num << std::endl;
        // Проверка пустоты и ошибок в конструкторе
        if (!rp) {
            std::wcout << L"[opis_command] ERROR: RecordProcessor not created for num=" << num << std::endl;
            return;
        }

        // ── единая отложенная задача ──────────────
        DelayedEvent evt;
        evt.execute_after = std::chrono::steady_clock::now() + std::chrono::seconds(7);

        evt.action = [rp]() {
           // std::wcout << L"[opis_command] Delayed action triggered for num=" << rp->getNum() << std::endl;

            try {
                if (rp->needUpdate()) {
                    rp->updateKrShtInShtJurnal();
                    //      std::wcout << L"[opis_command] KrSht updated in journal for num=" << rp->getNum() << std::endl;
                }
                else
                {
                     std::wcout << L"[opis_command] No update needed for num=" << rp->getNum() << std::endl;
                    //return;
                }


                PodrIgnore pi{"ignore.txt"};
                if (pi.itsIgnore(rp->getPodrazd())) {
                    std::wcout << L"[opis_command] Podrazd ignored for num=" << rp->getNum() << std::endl;
                    return;
                }

                if (rp->setAndCheckLists()) {
                 //   std::wcout << L"[opis_command] Running RecordProcessor for num=" << rp->getNum() << std::endl;
                    try {
                        rp->run();
               //         std::wcout << L"[opis_command] RecordProcessor finished for num=" << rp->getNum() << std::endl;
                    } catch (const std::exception& e) {
                        std::wcerr << L"[opis_command] Exception in run() for num=" << rp->getNum() << L": " << e.what() << std::endl;
                    } catch (...) {
                        std::wcerr << L"[opis_command] Unknown exception in run() for num=" << rp->getNum() << std::endl;
                    }
                } else {
                    std::wcout << L"[opis_command] setAndCheckLists failed for num=" << rp->getNum() << std::endl;
                }
            } catch (const std::exception& e) {
                std::wcerr << L"[opis_command] Exception in delayed action for num=" << rp->getNum() << L": " << e.what() << std::endl;
            } catch (...) {
                std::wcerr << L"[opis_command] Unknown exception in delayed action for num=" << rp->getNum() << std::endl;
            }
        };

        periodicTaskManager_.addDelayedEvent(evt);
     //   std::wcout << L"[opis_command] Delayed event added for num=" << num << std::endl;
    }


private:
    /*------------- утилиты -------------------------------------*/
    bool containsOUT(const std::wstring& p) const {
        std::wstring up = p;
        std::transform(up.begin(), up.end(), up.begin(), ::towupper);
        return up.find(L"OUT") != std::wstring::npos;
    }

    bool isExtensionAllowed(const std::filesystem::path& fp) const {
        std::string ext = fp.extension().string();
        if (!ext.empty() && ext[0] == '.') ext.erase(0,1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        for (auto a : allowed_exts_) {
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            if (ext == a) return true;
        }
        return ext.empty() &&                                           // пустое?
               std::find(allowed_exts_.begin(), allowed_exts_.end(), "") != allowed_exts_.end();
    }

    /*------------- данные --------------------------------------*/
    PeriodicTaskManager<TaskID>& periodicTaskManager_;
    std::vector<std::string>      allowed_exts_;
};
