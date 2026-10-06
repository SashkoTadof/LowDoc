#pragma once

#include "types.hpp"
#include <span>
#include <string>

namespace lowdoc {

class Validator {
public:
    static bool validate(DocumentFormat format, std::span<const uint8_t> data, std::span<const uint8_t> original_data);
    static bool validate_zip(std::span<const uint8_t> data);
    static bool validate_ooxml(std::span<const uint8_t> data);
    static bool validate_odf(std::span<const uint8_t> data);
    static bool validate_epub(std::span<const uint8_t> data);
    static bool validate_pdf(std::span<const uint8_t> data, std::span<const uint8_t> original_data);
    static bool validate_rtf(std::span<const uint8_t> data);
    static bool validate_image(DocumentFormat format, std::span<const uint8_t> data);
};

}
