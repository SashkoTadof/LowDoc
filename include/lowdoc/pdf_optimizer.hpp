#pragma once

#include "types.hpp"
#include <cstdint>
#include <vector>
#include <span>

namespace lowdoc {

class PdfOptimizer {
public:
    static bool is_signed(std::span<const uint8_t> data);
    static bool is_encrypted(std::span<const uint8_t> data);
    static bool optimize(std::span<const uint8_t> input, std::vector<uint8_t>& output, OptimizationReport& report);
};

}
