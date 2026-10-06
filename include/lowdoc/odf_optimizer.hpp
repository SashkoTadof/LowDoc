#pragma once

#include "types.hpp"
#include "zip_archive.hpp"

namespace lowdoc {

class OdfOptimizer {
public:
    static bool optimize(ZipArchive& archive, DocumentFormat format, OptimizationReport& report);
};

}
