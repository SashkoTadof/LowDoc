#pragma once

#include "types.hpp"
#include <span>

namespace lowdoc {

class FormatDetector {
public:
    static DocumentFormat detect(std::span<const uint8_t> data, const std::string& filename_hint = "");
    static bool is_ooxml(DocumentFormat fmt);
    static bool is_odf(DocumentFormat fmt);
    static bool is_zip_container(DocumentFormat fmt);
    static bool is_image(DocumentFormat fmt);
    static std::string default_extension(DocumentFormat fmt);
};

}
