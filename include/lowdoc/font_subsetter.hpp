#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <unordered_set>
#include <string>

namespace lowdoc {

class FontSubsetter {
public:
    static bool subset_ttf(std::span<const uint8_t> ttf_data, const std::unordered_set<uint32_t>& used_codepoints, std::vector<uint8_t>& output);
    static bool deobfuscate_odttf(std::span<const uint8_t> odttf_data, const std::string& guid_str, std::vector<uint8_t>& output);
    static bool obfuscate_odttf(std::span<const uint8_t> ttf_data, const std::string& guid_str, std::vector<uint8_t>& output);
    static bool optimize_cff(std::span<const uint8_t> cff_data, std::vector<uint8_t>& output);
};

}
