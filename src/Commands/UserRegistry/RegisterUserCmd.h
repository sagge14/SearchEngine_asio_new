#ifndef REGISTERUSERCOMMAND_H
#define REGISTERUSERCOMMAND_H

#include <vector>
#include <stdexcept>
#include "UserRegistry.h"
#include "Commands/Command.h"

class RegisterUserCmd : public Command {

public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& username_bytes) override;

};

#endif // REGISTERUSERCOMMAND_H
