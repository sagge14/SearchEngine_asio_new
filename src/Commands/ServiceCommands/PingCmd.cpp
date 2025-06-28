//
// Created by Sg on 05.06.2024.
//

#include "PingCmd.h"

std::vector<uint8_t> PingCmd::execute(const std::vector<uint8_t>& _data)
{
    const std::vector<uint8_t> pingMessage = { 'P', 'I', 'N', 'G' };
    const std::vector<uint8_t> pongMessage = { 'P', 'O', 'N', 'G' };

    if(_data == pingMessage)
        return pongMessage;

    else return {0};
}
