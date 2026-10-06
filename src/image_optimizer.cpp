#include "lowdoc/image_optimizer.hpp"
#include "lowdoc/deflate.hpp"
#include "lowdoc/xml_optimizer.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <unordered_map>

namespace lowdoc {

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

static bool try_optimize_png_payload(
    uint32_t width, uint32_t height, uint8_t bit_depth, uint8_t color_type, uint8_t interlace,
    std::span<const uint8_t> raw_pixels,
    std::span<const uint8_t> in_plte,
    uint8_t& out_color_type,
    std::vector<uint8_t>& out_plte,
    std::vector<uint8_t>& out_recompressed_idat) {

    if (interlace != 0 || bit_depth != 8 || width == 0 || height == 0 || width > 16384 || height > 16384) {
        return false;
    }

    size_t bpp = 0;
    if (color_type == 6) bpp = 4;
    else if (color_type == 2) bpp = 3;
    else if (color_type == 0 || color_type == 3) bpp = 1;
    else return false;

    size_t stride = width * bpp;
    if (raw_pixels.size() != height * (1 + stride)) {
        return false;
    }

    std::vector<uint8_t> recon(height * stride);
    for (size_t y = 0; y < height; ++y) {
        size_t row_in = y * (1 + stride);
        uint8_t ftype = raw_pixels[row_in];
        const uint8_t* in_line = &raw_pixels[row_in + 1];
        uint8_t* out_line = &recon[y * stride];
        const uint8_t* prev_line = (y > 0) ? &recon[(y - 1) * stride] : nullptr;

        for (size_t x = 0; x < stride; ++x) {
            uint8_t filt = in_line[x];
            int a = (x >= bpp) ? out_line[x - bpp] : 0;
            int b = prev_line ? prev_line[x] : 0;
            int c = (prev_line && x >= bpp) ? prev_line[x - bpp] : 0;
            uint8_t val = 0;
            if (ftype == 0) val = filt;
            else if (ftype == 1) val = static_cast<uint8_t>(filt + a);
            else if (ftype == 2) val = static_cast<uint8_t>(filt + b);
            else if (ftype == 3) val = static_cast<uint8_t>(filt + (a + b) / 2);
            else if (ftype == 4) val = static_cast<uint8_t>(filt + paeth(a, b, c));
            else return false;
            out_line[x] = val;
        }
    }

    uint8_t target_color_type = color_type;
    size_t target_bpp = bpp;
    std::vector<uint8_t> target_pixels;
    out_plte.clear();

    if (color_type == 6) {
        bool all_opaque = true;
        for (size_t p = 0; p < height * width; ++p) {
            if (recon[p * 4 + 3] != 255) {
                all_opaque = false;
                break;
            }
        }

        if (all_opaque) {
            std::vector<uint8_t> rgb(height * width * 3);
            for (size_t p = 0; p < height * width; ++p) {
                rgb[p * 3] = recon[p * 4];
                rgb[p * 3 + 1] = recon[p * 4 + 1];
                rgb[p * 3 + 2] = recon[p * 4 + 2];
            }

            bool all_gray = true;
            for (size_t p = 0; p < height * width; ++p) {
                if (rgb[p * 3] != rgb[p * 3 + 1] || rgb[p * 3] != rgb[p * 3 + 2]) {
                    all_gray = false;
                    break;
                }
            }

            if (all_gray) {
                target_color_type = 0;
                target_bpp = 1;
                target_pixels.resize(height * width);
                for (size_t p = 0; p < height * width; ++p) {
                    target_pixels[p] = rgb[p * 3];
                }
            } else {
                std::unordered_map<uint32_t, uint8_t> palette_map;
                std::vector<uint8_t> plte_data;
                bool exceeds_palette = false;
                for (size_t p = 0; p < height * width; ++p) {
                    uint32_t rgb_val = (static_cast<uint32_t>(rgb[p * 3]) << 16) |
                                       (static_cast<uint32_t>(rgb[p * 3 + 1]) << 8) |
                                       static_cast<uint32_t>(rgb[p * 3 + 2]);
                    if (palette_map.find(rgb_val) == palette_map.end()) {
                        if (palette_map.size() >= 256) {
                            exceeds_palette = true;
                            break;
                        }
                        palette_map[rgb_val] = static_cast<uint8_t>(palette_map.size());
                        plte_data.push_back(rgb[p * 3]);
                        plte_data.push_back(rgb[p * 3 + 1]);
                        plte_data.push_back(rgb[p * 3 + 2]);
                    }
                }

                if (!exceeds_palette && !plte_data.empty()) {
                    target_color_type = 3;
                    target_bpp = 1;
                    out_plte = std::move(plte_data);
                    target_pixels.resize(height * width);
                    for (size_t p = 0; p < height * width; ++p) {
                        uint32_t rgb_val = (static_cast<uint32_t>(rgb[p * 3]) << 16) |
                                           (static_cast<uint32_t>(rgb[p * 3 + 1]) << 8) |
                                           static_cast<uint32_t>(rgb[p * 3 + 2]);
                        target_pixels[p] = palette_map[rgb_val];
                    }
                } else {
                    target_color_type = 2;
                    target_bpp = 3;
                    target_pixels = std::move(rgb);
                }
            }
        } else {
            target_pixels = std::move(recon);
        }
    } else if (color_type == 2) {
        bool all_gray = true;
        for (size_t p = 0; p < height * width; ++p) {
            if (recon[p * 3] != recon[p * 3 + 1] || recon[p * 3] != recon[p * 3 + 2]) {
                all_gray = false;
                break;
            }
        }

        if (all_gray) {
            target_color_type = 0;
            target_bpp = 1;
            target_pixels.resize(height * width);
            for (size_t p = 0; p < height * width; ++p) {
                target_pixels[p] = recon[p * 3];
            }
        } else {
            std::unordered_map<uint32_t, uint8_t> palette_map;
            std::vector<uint8_t> plte_data;
            bool exceeds_palette = false;
            for (size_t p = 0; p < height * width; ++p) {
                uint32_t rgb_val = (static_cast<uint32_t>(recon[p * 3]) << 16) |
                                   (static_cast<uint32_t>(recon[p * 3 + 1]) << 8) |
                                   static_cast<uint32_t>(recon[p * 3 + 2]);
                if (palette_map.find(rgb_val) == palette_map.end()) {
                    if (palette_map.size() >= 256) {
                        exceeds_palette = true;
                        break;
                    }
                    palette_map[rgb_val] = static_cast<uint8_t>(palette_map.size());
                    plte_data.push_back(recon[p * 3]);
                    plte_data.push_back(recon[p * 3 + 1]);
                    plte_data.push_back(recon[p * 3 + 2]);
                }
            }

            if (!exceeds_palette && !plte_data.empty()) {
                target_color_type = 3;
                target_bpp = 1;
                out_plte = std::move(plte_data);
                target_pixels.resize(height * width);
                for (size_t p = 0; p < height * width; ++p) {
                    uint32_t rgb_val = (static_cast<uint32_t>(recon[p * 3]) << 16) |
                                       (static_cast<uint32_t>(recon[p * 3 + 1]) << 8) |
                                       static_cast<uint32_t>(recon[p * 3 + 2]);
                    target_pixels[p] = palette_map[rgb_val];
                }
            } else {
                target_pixels = std::move(recon);
            }
        }
    } else if (color_type == 3 && !in_plte.empty()) {
        target_color_type = 3;
        target_bpp = 1;
        out_plte.assign(in_plte.begin(), in_plte.end());
        target_pixels = std::move(recon);
    } else {
        target_pixels = std::move(recon);
    }

    if (target_color_type == 3 && !out_plte.empty()) {
        size_t num_colors = out_plte.size() / 3;
        std::vector<uint32_t> freq(num_colors, 0);
        for (uint8_t px : target_pixels) {
            if (px < num_colors) freq[px]++;
        }

        std::vector<uint8_t> old_to_new(num_colors);
        std::vector<uint8_t> new_to_old(num_colors);
        for (size_t i = 0; i < num_colors; ++i) new_to_old[i] = static_cast<uint8_t>(i);

        std::stable_sort(new_to_old.begin(), new_to_old.end(), [&](uint8_t a, uint8_t b) {
            return freq[a] > freq[b];
        });

        for (size_t i = 0; i < num_colors; ++i) {
            old_to_new[new_to_old[i]] = static_cast<uint8_t>(i);
        }

        std::vector<uint8_t> sorted_plte(out_plte.size());
        for (size_t i = 0; i < num_colors; ++i) {
            uint8_t old_idx = new_to_old[i];
            sorted_plte[i * 3] = out_plte[old_idx * 3];
            sorted_plte[i * 3 + 1] = out_plte[old_idx * 3 + 1];
            sorted_plte[i * 3 + 2] = out_plte[old_idx * 3 + 2];
        }
        out_plte = std::move(sorted_plte);

        for (uint8_t& px : target_pixels) {
            if (px < num_colors) px = old_to_new[px];
        }
    }

    size_t target_stride = width * target_bpp;
    std::vector<uint8_t> refiltered(height * (1 + target_stride));
    std::vector<uint8_t> cand_buf(5 * target_stride);
    uint8_t* cand_lines[5] = {
        cand_buf.data(),
        cand_buf.data() + target_stride,
        cand_buf.data() + 2 * target_stride,
        cand_buf.data() + 3 * target_stride,
        cand_buf.data() + 4 * target_stride
    };

    for (size_t y = 0; y < height; ++y) {
        const uint8_t* curr = &target_pixels[y * target_stride];
        const uint8_t* prev = (y > 0) ? &target_pixels[(y - 1) * target_stride] : nullptr;

        uint32_t cand_scores[5] = {0, 0, 0, 0, 0};

        for (size_t x = 0; x < target_stride; ++x) {
            int raw = curr[x];
            int a = (x >= target_bpp) ? curr[x - target_bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= target_bpp) ? prev[x - target_bpp] : 0;

            cand_lines[0][x] = static_cast<uint8_t>(raw);
            cand_lines[1][x] = static_cast<uint8_t>(raw - a);
            cand_lines[2][x] = static_cast<uint8_t>(raw - b);
            cand_lines[3][x] = static_cast<uint8_t>(raw - (a + b) / 2);
            cand_lines[4][x] = static_cast<uint8_t>(raw - paeth(a, b, c));

            cand_scores[0] += std::abs(static_cast<int8_t>(cand_lines[0][x]));
            cand_scores[1] += std::abs(static_cast<int8_t>(cand_lines[1][x]));
            cand_scores[2] += std::abs(static_cast<int8_t>(cand_lines[2][x]));
            cand_scores[3] += std::abs(static_cast<int8_t>(cand_lines[3][x]));
            cand_scores[4] += std::abs(static_cast<int8_t>(cand_lines[4][x]));
        }

        uint8_t best_f = 0;
        uint32_t best_s = cand_scores[0];
        for (uint8_t f = 1; f < 5; ++f) {
            if (cand_scores[f] < best_s) {
                best_s = cand_scores[f];
                best_f = f;
            }
        }

        refiltered[y * (1 + target_stride)] = best_f;
        std::memcpy(&refiltered[y * (1 + target_stride) + 1], cand_lines[best_f], target_stride);
    }

    out_recompressed_idat = DeflateEngine::zlib_compress_best(refiltered);
    out_color_type = target_color_type;
    return !out_recompressed_idat.empty();
}

bool ImageOptimizer::optimize_png(std::span<const uint8_t> input, std::vector<uint8_t>& output) {
    const uint8_t png_sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (input.size() < 8 || std::memcmp(input.data(), png_sig, 8) != 0) {
        return false;
    }

    struct PngChunk {
        std::array<char, 4> tag;
        std::vector<uint8_t> data;
    };

    std::vector<PngChunk> chunks;
    std::vector<uint8_t> all_idat;

    size_t pos = 8;
    while (pos + 12 <= input.size()) {
        uint32_t len = (static_cast<uint32_t>(input[pos]) << 24) |
                       (static_cast<uint32_t>(input[pos + 1]) << 16) |
                       (static_cast<uint32_t>(input[pos + 2]) << 8) |
                       static_cast<uint32_t>(input[pos + 3]);
        pos += 4;

        if (pos + 4 + len + 4 > input.size()) return false;

        std::array<char, 4> tag;
        std::memcpy(tag.data(), input.data() + pos, 4);
        pos += 4;

        std::span<const uint8_t> chunk_data(input.data() + pos, len);
        pos += len;
        pos += 4;

        if (std::memcmp(tag.data(), "IDAT", 4) == 0) {
            all_idat.insert(all_idat.end(), chunk_data.begin(), chunk_data.end());
        } else if (std::memcmp(tag.data(), "IHDR", 4) == 0 ||
                   std::memcmp(tag.data(), "PLTE", 4) == 0 ||
                   std::memcmp(tag.data(), "tRNS", 4) == 0 ||
                   std::memcmp(tag.data(), "iCCP", 4) == 0 ||
                   std::memcmp(tag.data(), "IEND", 4) == 0) {
            PngChunk chunk;
            chunk.tag = tag;
            chunk.data.assign(chunk_data.begin(), chunk_data.end());
            chunks.push_back(std::move(chunk));
        }
    }

    if (all_idat.empty()) return false;

    std::vector<uint8_t> raw_pixels;
    if (!DeflateEngine::zlib_decompress(all_idat, raw_pixels)) {
        return false;
    }

    auto write_chunk_to = [](std::vector<uint8_t>& buf, const char tname[4], std::span<const uint8_t> cdata) {
        uint32_t len = static_cast<uint32_t>(cdata.size());
        buf.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(len & 0xFF));

        size_t crc_start = buf.size();
        buf.insert(buf.end(), tname, tname + 4);
        buf.insert(buf.end(), cdata.begin(), cdata.end());

        uint32_t c = DeflateEngine::crc32(std::span<const uint8_t>(buf.data() + crc_start, cdata.size() + 4));
        buf.push_back(static_cast<uint8_t>((c >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((c >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(c & 0xFF));
    };

    auto build_png = [&](std::span<const uint8_t> idat_bytes, const std::vector<PngChunk>& chs) {
        std::vector<uint8_t> res;
        res.reserve(input.size());
        res.insert(res.end(), png_sig, png_sig + 8);
        bool idat_done = false;
        for (const auto& ch : chs) {
            if (std::memcmp(ch.tag.data(), "IEND", 4) == 0 && !idat_done) {
                write_chunk_to(res, "IDAT", idat_bytes);
                idat_done = true;
            }
            write_chunk_to(res, ch.tag.data(), ch.data);
        }
        if (!idat_done) {
            write_chunk_to(res, "IDAT", idat_bytes);
            write_chunk_to(res, "IEND", std::span<const uint8_t>{});
        }
        return res;
    };

    auto recompressed_idat = DeflateEngine::zlib_compress_best(raw_pixels);
    std::span<const uint8_t> best_idat = (recompressed_idat.size() < all_idat.size()) ?
        std::span<const uint8_t>(recompressed_idat) : std::span<const uint8_t>(all_idat);

    std::vector<uint8_t> best_result = build_png(best_idat, chunks);

    PngChunk* ihdr_chunk = nullptr;
    for (auto& ch : chunks) {
        if (std::memcmp(ch.tag.data(), "IHDR", 4) == 0 && ch.data.size() == 13) {
            ihdr_chunk = &ch;
            break;
        }
    }

    if (ihdr_chunk) {
        uint32_t w = (static_cast<uint32_t>(ihdr_chunk->data[0]) << 24) |
                     (static_cast<uint32_t>(ihdr_chunk->data[1]) << 16) |
                     (static_cast<uint32_t>(ihdr_chunk->data[2]) << 8) |
                     static_cast<uint32_t>(ihdr_chunk->data[3]);
        uint32_t h = (static_cast<uint32_t>(ihdr_chunk->data[4]) << 24) |
                     (static_cast<uint32_t>(ihdr_chunk->data[5]) << 16) |
                     (static_cast<uint32_t>(ihdr_chunk->data[6]) << 8) |
                     static_cast<uint32_t>(ihdr_chunk->data[7]);
        uint8_t depth = ihdr_chunk->data[8];
        uint8_t ctype = ihdr_chunk->data[9];
        uint8_t inter = ihdr_chunk->data[12];

        uint8_t new_ctype = ctype;
        std::vector<uint8_t> new_plte;
        std::vector<uint8_t> new_idat;

        std::span<const uint8_t> in_plte;
        for (const auto& ch : chunks) {
            if (std::memcmp(ch.tag.data(), "PLTE", 4) == 0) {
                in_plte = ch.data;
                break;
            }
        }

        if (try_optimize_png_payload(w, h, depth, ctype, inter, raw_pixels, in_plte, new_ctype, new_plte, new_idat)) {
            std::vector<PngChunk> alt_chunks;
            for (const auto& ch : chunks) {
                if (std::memcmp(ch.tag.data(), "PLTE", 4) == 0) continue;
                if (std::memcmp(ch.tag.data(), "IHDR", 4) == 0) {
                    PngChunk alt_ihdr = ch;
                    alt_ihdr.data[9] = new_ctype;
                    alt_chunks.push_back(std::move(alt_ihdr));
                    if (new_ctype == 3 && !new_plte.empty()) {
                        PngChunk plte_chunk;
                        std::memcpy(plte_chunk.tag.data(), "PLTE", 4);
                        plte_chunk.data = new_plte;
                        alt_chunks.push_back(std::move(plte_chunk));
                    }
                } else {
                    alt_chunks.push_back(ch);
                }
            }

            auto alt_png = build_png(new_idat, alt_chunks);
            if (!alt_png.empty() && alt_png.size() < best_result.size()) {
                best_result = std::move(alt_png);
            }
        }
    }

    if (best_result.size() < input.size()) {
        output = std::move(best_result);
        return true;
    }

    return false;
}

bool ImageOptimizer::optimize_jpeg(std::span<const uint8_t> input, std::vector<uint8_t>& output) {
    if (input.size() < 4 || input[0] != 0xFF || input[1] != 0xD8) {
        return false;
    }

    std::vector<uint8_t> result;
    result.reserve(input.size());
    result.push_back(0xFF);
    result.push_back(0xD8);

    size_t pos = 2;
    while (pos < input.size()) {
        if (input[pos] != 0xFF) {
            result.push_back(input[pos++]);
            continue;
        }

        while (pos < input.size() && input[pos] == 0xFF) {
            pos++;
        }
        if (pos >= input.size()) break;

        uint8_t marker = input[pos++];
        if (marker == 0xD9) {
            result.push_back(0xFF);
            result.push_back(0xD9);
            break;
        }
        if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7)) {
            result.push_back(0xFF);
            result.push_back(marker);
            continue;
        }

        if (pos + 2 > input.size()) return false;
        uint16_t length = (static_cast<uint16_t>(input[pos]) << 8) | input[pos + 1];
        if (pos + length > input.size() || length < 2) return false;

        bool keep_marker = true;
        if (marker == 0xFE) {
            keep_marker = false;
        } else if (marker == 0xE0) {
            if (length >= 16 && pos + 16 <= input.size() && std::memcmp(input.data() + pos + 2, "JFIF\0", 5) == 0) {
                if (length > 16 || input[pos + 14] > 0 || input[pos + 15] > 0) {
                    result.push_back(0xFF);
                    result.push_back(0xE0);
                    result.push_back(0x00);
                    result.push_back(0x10);
                    result.insert(result.end(), input.data() + pos + 2, input.data() + pos + 14);
                    result.push_back(0x00);
                    result.push_back(0x00);
                    keep_marker = false;
                }
            } else if (length >= 7 && pos + 7 <= input.size() && std::memcmp(input.data() + pos + 2, "JFXX\0", 5) == 0) {
                keep_marker = false;
            }
        } else if (marker == 0xE1) {
            keep_marker = false;
        } else if (marker == 0xE2) {
            bool is_icc = false;
            if (length >= 14 && pos + 14 <= input.size()) {
                const char* icc_sig = "ICC_PROFILE";
                if (std::memcmp(input.data() + pos + 2, icc_sig, 11) == 0) {
                    is_icc = true;
                }
            }
            if (!is_icc) keep_marker = false;
        } else if (marker >= 0xE3 && marker <= 0xEF) {
            keep_marker = false;
        }

        if (keep_marker) {
            result.push_back(0xFF);
            result.push_back(marker);
            result.insert(result.end(), input.data() + pos, input.data() + pos + length);
        }

        pos += length;

        if (marker == 0xDA) {
            while (pos < input.size()) {
                if (input[pos] == 0xFF) {
                    if (pos + 1 < input.size() && input[pos + 1] != 0x00 && !(input[pos + 1] >= 0xD0 && input[pos + 1] <= 0xD7)) {
                        break;
                    }
                }
                result.push_back(input[pos++]);
            }
        }
    }

    if (result.size() < input.size()) {
        output = std::move(result);
        return true;
    }

    return false;
}

bool ImageOptimizer::optimize_svg(std::span<const uint8_t> input, std::vector<uint8_t>& output) {
    std::string_view sv(reinterpret_cast<const char*>(input.data()), input.size());
    std::string minified = XmlOptimizer::minify(sv);
    if (minified.size() < input.size() && XmlOptimizer::is_well_formed(minified)) {
        output.assign(minified.begin(), minified.end());
        return true;
    }
    return false;
}

bool ImageOptimizer::optimize_image(std::span<const uint8_t> input, const std::string& extension, std::vector<uint8_t>& output) {
    std::string ext = extension;
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == ".png") {
        return optimize_png(input, output);
    }
    if (ext == ".jpg" || ext == ".jpeg") {
        return optimize_jpeg(input, output);
    }
    if (ext == ".svg") {
        return optimize_svg(input, output);
    }
    return false;
}

}
