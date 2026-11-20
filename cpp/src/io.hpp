// io.hpp - load an ITCH stream from a binary file or the simulator's .hex file
#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

inline std::vector<uint8_t> load_stream(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> buf;
    if (path.size() > 4 && path.substr(path.size() - 4) == ".hex") {   // one hex byte per line
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) buf.push_back(uint8_t(std::stoul(line, nullptr, 16)));
    } else {
        f.seekg(0, std::ios::end);
        buf.resize(size_t(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
    }
    return buf;
}
