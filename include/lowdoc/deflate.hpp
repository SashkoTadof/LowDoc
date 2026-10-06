#pragma once

#include <cstdint>
#include <vector>
#include <span>

namespace lowdoc {

enum class DeflateStrategy {
    Auto,
    FixedHuffman,
    DynamicHuffman,
    StoreOnly
};

struct DeflateOptions {
    int level = 9;
    DeflateStrategy strategy = DeflateStrategy::Auto;
    uint32_t max_chain = 2048;
    uint32_t nice_length = 258;
    bool lazy_matching = true;
};

class DeflateEngine {
public:
    static std::vector<uint8_t> compress(std::span<const uint8_t> input, const DeflateOptions& options = {});
    static std::vector<uint8_t> compress_best(std::span<const uint8_t> input);
    static bool decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size = 0);

    static std::vector<uint8_t> zlib_compress(std::span<const uint8_t> input, const DeflateOptions& options = {});
    static std::vector<uint8_t> zlib_compress_best(std::span<const uint8_t> input);
    static bool zlib_decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size = 0);

    static uint32_t crc32(std::span<const uint8_t> input, uint32_t seed = 0);
    static uint32_t adler32(std::span<const uint8_t> input);
};

}
