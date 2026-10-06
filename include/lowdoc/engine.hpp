#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <span>
#include <filesystem>

namespace lowdoc {

class OptimizationEngine {
public:
    static OptimizationReport optimize_file(const std::filesystem::path& input_path, const OptimizationOptions& options = {});
    static std::pair<std::vector<uint8_t>, OptimizationReport> optimize_buffer(
        std::span<const uint8_t> input_data,
        const std::string& filename_hint = "",
        const OptimizationOptions& options = {});

    static std::filesystem::path determine_output_path(const std::filesystem::path& input_path, const std::string& custom_output = "");
};

}
