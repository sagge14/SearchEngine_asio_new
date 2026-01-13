//
// Created by Sg on 19.09.2025.
//

#ifndef SEARCHENGINE_COMMANDCONTEXT_H
#define SEARCHENGINE_COMMANDCONTEXT_H

#include <vector>
#include <string>
#include <filesystem>


struct CommandContext {

   // COMMAND command;
    bool error;
    std::vector<unsigned char> input;   // то, что пришло
    std::vector<unsigned char> output;  // то, что вернём (или пусто, если, например, отдаём файл)

    std::string user_name;
    std::string request;
    std::string request_type;

 //   PersonalRequest logRecord; // для логирования
    std::filesystem::path file; // если это "file" команда
    size_t fileSize = 0;       // размер, если есть файл

    void setError() {
        error = true;
        output.clear();
    }
};



#endif //SEARCHENGINE_COMMANDCONTEXT_H
