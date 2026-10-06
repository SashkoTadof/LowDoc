#pragma once

#include "lowdoc/types.hpp"
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace lowdoc {

class HtmlOptimizer {
public:
    static bool optimize(std::string_view input, std::string& output, OptimizationReport& report);
};

}
