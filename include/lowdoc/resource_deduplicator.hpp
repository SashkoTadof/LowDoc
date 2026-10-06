#pragma once

#include "lowdoc/types.hpp"
#include "lowdoc/zip_archive.hpp"
#include <cstdint>
#include <string>

namespace lowdoc {

class ResourceDeduplicator {
public:
    static bool deduplicate_archive_media(ZipArchive& archive, DocumentFormat format, int64_t& saved_bytes);
};

}
