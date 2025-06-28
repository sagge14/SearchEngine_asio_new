//
// Created by user on 06.08.2023.
//

#pragma once
#include <iostream>
#include <set>


struct Resolution
{
    enum class EXC:size_t
    {
        ISP = 0, SO_ISP
    };
    std::string n_sht,id_r,text,chk,file_name,dir_sht,dead_line;
    std::set<std::string> isp, so_isp;
    std::string getLastN_RES();
    int addResolution();
    int saveResolutions();

    Resolution() = default;
};