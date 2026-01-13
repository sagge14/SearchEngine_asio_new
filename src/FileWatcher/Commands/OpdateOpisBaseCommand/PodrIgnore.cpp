#include "PodrIgnore.h"

#include <fstream>
#include <filesystem>
#include <iostream>

using std::string;
namespace fs = std::filesystem;

/* ---------- helpers ---------- */

static inline string trim(string s)
{
    const auto not_space = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

/* ---------- private ---------- */

void PodrIgnore::save_defaults()
{
    std::ofstream out(file_path_, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot create ignore-file: " + file_path_);

    out << "ИСХ\n" << "ТРАНЗИТ\n";
    ignore_ = {"ИСХ", "ТРАНЗИТ"};
}

void PodrIgnore::load()
{
    ignore_.clear();

    std::ifstream in(file_path_, std::ios::binary);
    if (!in) {
        save_defaults();
        return;
    }

    string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty())
            ignore_.insert(line);
    }
}

/* ---------- public ---------- */

PodrIgnore::PodrIgnore(string file_path)
        : file_path_(std::move(file_path))
{
    // Если путь относительный – «превратить» его в абсолютный
    if (!fs::path(file_path_).is_absolute())
        file_path_ = (fs::current_path() / file_path_).string();

    try {
        load();
    }
    catch (const std::exception& e) {
        std::cerr << "PodrIgnore: " << e.what() << '\n';
        throw;
    }
}

bool PodrIgnore::itsIgnore(const string& podr) const noexcept
{
    return ignore_.contains(podr);
}

void PodrIgnore::reload()
{
    load();
}

int PodrIgnore::size() {
    return ignore_.size();
}
