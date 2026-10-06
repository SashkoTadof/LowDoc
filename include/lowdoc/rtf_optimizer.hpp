#pragma once

#include "types.hpp"
#include <string_view>
#include <string>
#include <vector>

namespace lowdoc {

class RtfOptimizer {
public:
    static bool is_valid(std::string_view rtf);
    static bool optimize(std::string_view input, std::string& output, OptimizationReport& report);
};

}
