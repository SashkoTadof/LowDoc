#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <string>

namespace lowdoc {

class ImageOptimizer {
public:
    static bool optimize_png(std::span<const uint8_t> input, std::vector<uint8_t>& output);
    static bool optimize_jpeg(std::span<const uint8_t> input, std::vector<uint8_t>& output);
    static bool optimize_svg(std::span<const uint8_t> input, std::vector<uint8_t>& output);
    static bool optimize_image(std::span<const uint8_t> input, const std::string& extension, std::vector<uint8_t>& output);
};

}
