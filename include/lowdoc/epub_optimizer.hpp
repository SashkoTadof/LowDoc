#pragma once

#include "types.hpp"
#include "zip_archive.hpp"

namespace lowdoc {

class EpubOptimizer {
public:
    static bool optimize(ZipArchive& archive, OptimizationReport& report);
};

}
