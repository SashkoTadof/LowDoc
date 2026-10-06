#include "lowdoc/deflate.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <queue>

namespace lowdoc {

static const std::array<uint32_t, 256>& get_crc32_table() {
    static const auto table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                if (c & 1) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t DeflateEngine::crc32(std::span<const uint8_t> input, uint32_t seed) {
    const auto& table = get_crc32_table();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (uint8_t byte : input) {
        c = table[(c ^ byte) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t DeflateEngine::adler32(std::span<const uint8_t> input) {
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    const uint32_t base = 65521;
    for (uint8_t byte : input) {
        s1 = (s1 + byte) % base;
        s2 = (s2 + s1) % base;
    }
    return (s2 << 16) | s1;
}

class BitWriter {
public:
    void write_bits(uint32_t value, uint32_t count) {
        if (count == 0) return;
        bit_buffer_ |= (static_cast<uint64_t>(value & (count == 32 ? 0xFFFFFFFFu : ((1u << count) - 1))) << bit_count_);
        bit_count_ += count;
        while (bit_count_ >= 8) {
            output_.push_back(static_cast<uint8_t>(bit_buffer_ & 0xFF));
            bit_buffer_ >>= 8;
            bit_count_ -= 8;
        }
    }

    void flush() {
        if (bit_count_ > 0) {
            output_.push_back(static_cast<uint8_t>(bit_buffer_ & 0xFF));
            bit_buffer_ = 0;
            bit_count_ = 0;
        }
    }

    std::vector<uint8_t> take_data() {
        flush();
        return std::move(output_);
    }

    size_t byte_size() const {
        return output_.size() + (bit_count_ > 0 ? 1 : 0);
    }

private:
    std::vector<uint8_t> output_;
    uint64_t bit_buffer_ = 0;
    uint32_t bit_count_ = 0;
};

class BitReader {
public:
    BitReader(std::span<const uint8_t> data) : data_(data) {}

    bool read_bit(uint32_t& bit) {
        return read_bits(1, bit);
    }

    bool read_bits(uint32_t count, uint32_t& value) {
        while (bit_count_ < count) {
            if (byte_pos_ >= data_.size()) {
                return false;
            }
            bit_buffer_ |= (static_cast<uint64_t>(data_[byte_pos_++]) << bit_count_);
            bit_count_ += 8;
        }
        value = static_cast<uint32_t>(bit_buffer_ & (count == 32 ? 0xFFFFFFFFu : ((1ULL << count) - 1)));
        bit_buffer_ >>= count;
        bit_count_ -= count;
        return true;
    }

    void align_to_byte() {
        byte_pos_ -= (bit_count_ / 8);
        bit_buffer_ = 0;
        bit_count_ = 0;
    }

    bool read_bytes(uint8_t* dest, size_t len) {
        align_to_byte();
        if (byte_pos_ + len > data_.size()) return false;
        std::memcpy(dest, data_.data() + byte_pos_, len);
        byte_pos_ += len;
        return true;
    }

    size_t remaining_bytes() const {
        return byte_pos_ < data_.size() ? (data_.size() - byte_pos_) : 0;
    }

private:
    std::span<const uint8_t> data_;
    size_t byte_pos_ = 0;
    uint64_t bit_buffer_ = 0;
    uint32_t bit_count_ = 0;
};

struct HuffmanDecoder {
    struct Node {
        int left = -1;
        int right = -1;
        int symbol = -1;
    };
    std::vector<Node> nodes;

    void build_from_lengths(const uint8_t* lengths, size_t count) {
        nodes.clear();
        nodes.push_back({});

        int max_len = 0;
        for (size_t i = 0; i < count; ++i) {
            if (lengths[i] > max_len) max_len = lengths[i];
        }
        if (max_len == 0) return;

        std::vector<int> bl_count(max_len + 1, 0);
        for (size_t i = 0; i < count; ++i) {
            if (lengths[i] > 0) bl_count[lengths[i]]++;
        }

        std::vector<int> next_code(max_len + 1, 0);
        int code = 0;
        for (int bits = 1; bits <= max_len; ++bits) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }

        for (size_t i = 0; i < count; ++i) {
            int len = lengths[i];
            if (len == 0) continue;
            int c = next_code[len]++;
            int curr = 0;
            for (int b = len - 1; b >= 0; --b) {
                int bit = (c >> b) & 1;
                if (bit == 0) {
                    if (nodes[curr].left == -1) {
                        nodes[curr].left = static_cast<int>(nodes.size());
                        nodes.push_back({});
                    }
                    curr = nodes[curr].left;
                } else {
                    if (nodes[curr].right == -1) {
                        nodes[curr].right = static_cast<int>(nodes.size());
                        nodes.push_back({});
                    }
                    curr = nodes[curr].right;
                }
            }
            nodes[curr].symbol = static_cast<int>(i);
        }
    }

    bool decode_symbol(BitReader& reader, int& symbol) const {
        if (nodes.empty()) return false;
        int curr = 0;
        while (curr >= 0 && curr < static_cast<int>(nodes.size())) {
            if (nodes[curr].symbol != -1) {
                symbol = nodes[curr].symbol;
                return true;
            }
            uint32_t bit = 0;
            if (!reader.read_bit(bit)) return false;
            curr = (bit == 0) ? nodes[curr].left : nodes[curr].right;
        }
        return false;
    }
};

static const uint16_t LENGTH_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LENGTH_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const uint8_t CL_ORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static bool inflate_block_fixed(BitReader& reader, std::vector<uint8_t>& output, size_t max_out) {
    std::array<uint8_t, 288> lit_lens{};
    for (int i = 0; i <= 143; ++i) lit_lens[i] = 8;
    for (int i = 144; i <= 255; ++i) lit_lens[i] = 9;
    for (int i = 256; i <= 279; ++i) lit_lens[i] = 7;
    for (int i = 280; i <= 287; ++i) lit_lens[i] = 8;

    std::array<uint8_t, 32> dist_lens{};
    for (int i = 0; i < 32; ++i) dist_lens[i] = 5;

    HuffmanDecoder lit_dec, dist_dec;
    lit_dec.build_from_lengths(lit_lens.data(), lit_lens.size());
    dist_dec.build_from_lengths(dist_lens.data(), dist_lens.size());

    while (true) {
        int sym = 0;
        if (!lit_dec.decode_symbol(reader, sym)) return false;
        if (sym < 256) {
            output.push_back(static_cast<uint8_t>(sym));
            if (max_out && output.size() > max_out) return false;
        } else if (sym == 256) {
            break;
        } else {
            int len_idx = sym - 257;
            if (len_idx < 0 || len_idx >= 29) return false;
            uint32_t length = LENGTH_BASE[len_idx];
            uint32_t extra = 0;
            if (LENGTH_EXTRA[len_idx] > 0) {
                if (!reader.read_bits(LENGTH_EXTRA[len_idx], extra)) return false;
                length += extra;
            }

            int dist_sym = 0;
            if (!dist_dec.decode_symbol(reader, dist_sym)) return false;
            if (dist_sym < 0 || dist_sym >= 30) return false;
            uint32_t distance = DIST_BASE[dist_sym];
            if (DIST_EXTRA[dist_sym] > 0) {
                if (!reader.read_bits(DIST_EXTRA[dist_sym], extra)) return false;
                distance += extra;
            }

            if (distance > output.size()) return false;
            size_t start = output.size() - distance;
            for (uint32_t k = 0; k < length; ++k) {
                output.push_back(output[start + k]);
                if (max_out && output.size() > max_out) return false;
            }
        }
    }
    return true;
}

static bool inflate_block_dynamic(BitReader& reader, std::vector<uint8_t>& output, size_t max_out) {
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!reader.read_bits(5, hlit)) return false;
    if (!reader.read_bits(5, hdist)) return false;
    if (!reader.read_bits(4, hclen)) return false;

    hlit += 257;
    hdist += 1;
    hclen += 4;

    std::array<uint8_t, 19> cl_lengths{};
    for (uint32_t i = 0; i < hclen; ++i) {
        uint32_t len = 0;
        if (!reader.read_bits(3, len)) return false;
        cl_lengths[CL_ORDER[i]] = static_cast<uint8_t>(len);
    }

    HuffmanDecoder cl_dec;
    cl_dec.build_from_lengths(cl_lengths.data(), cl_lengths.size());

    size_t total_codes = hlit + hdist;
    std::vector<uint8_t> all_lens;
    all_lens.reserve(total_codes);

    while (all_lens.size() < total_codes) {
        int sym = 0;
        if (!cl_dec.decode_symbol(reader, sym)) return false;
        if (sym < 16) {
            all_lens.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 16) {
            if (all_lens.empty()) return false;
            uint8_t prev = all_lens.back();
            uint32_t rep = 0;
            if (!reader.read_bits(2, rep)) return false;
            rep += 3;
            for (uint32_t r = 0; r < rep; ++r) {
                all_lens.push_back(prev);
            }
        } else if (sym == 17) {
            uint32_t rep = 0;
            if (!reader.read_bits(3, rep)) return false;
            rep += 3;
            for (uint32_t r = 0; r < rep; ++r) {
                all_lens.push_back(0);
            }
        } else if (sym == 18) {
            uint32_t rep = 0;
            if (!reader.read_bits(7, rep)) return false;
            rep += 11;
            for (uint32_t r = 0; r < rep; ++r) {
                all_lens.push_back(0);
            }
        } else {
            return false;
        }
    }

    if (all_lens.size() < total_codes) return false;

    HuffmanDecoder lit_dec, dist_dec;
    lit_dec.build_from_lengths(all_lens.data(), hlit);
    dist_dec.build_from_lengths(all_lens.data() + hlit, hdist);

    while (true) {
        int sym = 0;
        if (!lit_dec.decode_symbol(reader, sym)) return false;
        if (sym < 256) {
            output.push_back(static_cast<uint8_t>(sym));
            if (max_out && output.size() > max_out) return false;
        } else if (sym == 256) {
            break;
        } else {
            int len_idx = sym - 257;
            if (len_idx < 0 || len_idx >= 29) return false;
            uint32_t length = LENGTH_BASE[len_idx];
            uint32_t extra = 0;
            if (LENGTH_EXTRA[len_idx] > 0) {
                if (!reader.read_bits(LENGTH_EXTRA[len_idx], extra)) return false;
                length += extra;
            }

            int dist_sym = 0;
            if (!dist_dec.decode_symbol(reader, dist_sym)) return false;
            if (dist_sym < 0 || dist_sym >= 30) return false;
            uint32_t distance = DIST_BASE[dist_sym];
            if (DIST_EXTRA[dist_sym] > 0) {
                if (!reader.read_bits(DIST_EXTRA[dist_sym], extra)) return false;
                distance += extra;
            }

            if (distance > output.size()) return false;
            size_t start = output.size() - distance;
            for (uint32_t k = 0; k < length; ++k) {
                output.push_back(output[start + k]);
                if (max_out && output.size() > max_out) return false;
            }
        }
    }
    return true;
}

bool DeflateEngine::decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size) {
    output.clear();
    BitReader reader(input);

    uint32_t bfinal = 0;
    while (!bfinal) {
        if (!reader.read_bit(bfinal)) return false;
        uint32_t btype = 0;
        if (!reader.read_bits(2, btype)) return false;

        if (btype == 0) {
            reader.align_to_byte();
            uint32_t len = 0, nlen = 0;
            if (!reader.read_bits(16, len)) return false;
            if (!reader.read_bits(16, nlen)) return false;
            if ((len ^ 0xFFFFu) != nlen) return false;
            size_t curr_pos = output.size();
            output.resize(curr_pos + len);
            if (!reader.read_bytes(output.data() + curr_pos, len)) return false;
        } else if (btype == 1) {
            if (!inflate_block_fixed(reader, output, max_output_size)) return false;
        } else if (btype == 2) {
            if (!inflate_block_dynamic(reader, output, max_output_size)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool DeflateEngine::zlib_decompress(std::span<const uint8_t> input, std::vector<uint8_t>& output, size_t max_output_size) {
    if (input.size() < 6) return false;
    uint8_t cmf = input[0];
    uint8_t flg = input[1];
    if ((cmf * 256 + flg) % 31 != 0) return false;
    if ((cmf & 0x0F) != 8) return false;
    if (flg & 0x20) return false;

    size_t payload_len = input.size() - 6;
    std::span<const uint8_t> raw_deflate(input.data() + 2, payload_len);
    if (!decompress(raw_deflate, output, max_output_size)) return false;

    uint32_t expected_adler = (static_cast<uint32_t>(input[input.size() - 4]) << 24) |
                             (static_cast<uint32_t>(input[input.size() - 3]) << 16) |
                             (static_cast<uint32_t>(input[input.size() - 2]) << 8) |
                             static_cast<uint32_t>(input[input.size() - 1]);
    return adler32(output) == expected_adler;
}

struct LzToken {
    uint16_t length = 0;
    uint16_t distance = 0;
    uint8_t literal = 0;
    bool is_match() const { return length > 0; }
};

static void compute_huffman_codes(const uint32_t* freqs, size_t count, int max_bits, std::vector<uint8_t>& lengths, std::vector<uint16_t>& codes) {
    lengths.assign(count, 0);
    codes.assign(count, 0);

    struct Node {
        uint32_t weight = 0;
        int symbol = -1;
        int left = -1;
        int right = -1;
    };

    auto cmp = [](const Node& a, const Node& b) {
        return a.weight > b.weight;
    };

    std::vector<Node> tree;
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);

    for (size_t i = 0; i < count; ++i) {
        if (freqs[i] > 0) {
            Node n;
            n.weight = freqs[i];
            n.symbol = static_cast<int>(i);
            pq.push(n);
        }
    }

    if (pq.empty()) {
        return;
    }
    if (pq.size() == 1) {
        int sym = pq.top().symbol;
        int other = (sym == 0) ? 1 : 0;
        lengths[sym] = 1;
        lengths[other] = 1;
        codes[sym] = 0;
        codes[other] = 1;
        return;
    }

    while (pq.size() > 1) {
        Node left = pq.top(); pq.pop();
        Node right = pq.top(); pq.pop();

        int left_idx = static_cast<int>(tree.size());
        tree.push_back(left);
        int right_idx = static_cast<int>(tree.size());
        tree.push_back(right);

        Node parent;
        parent.weight = left.weight + right.weight;
        parent.left = left_idx;
        parent.right = right_idx;
        pq.push(parent);
    }

    int root = static_cast<int>(tree.size());
    tree.push_back(pq.top());

    auto get_depths = [&](auto& self, int idx, int depth) -> void {
        if (tree[idx].symbol != -1) {
            lengths[tree[idx].symbol] = static_cast<uint8_t>(depth);
            return;
        }
        if (tree[idx].left != -1) self(self, tree[idx].left, depth + 1);
        if (tree[idx].right != -1) self(self, tree[idx].right, depth + 1);
    };
    get_depths(get_depths, root, 0);

    for (size_t i = 0; i < count; ++i) {
        if (lengths[i] > max_bits) lengths[i] = static_cast<uint8_t>(max_bits);
    }

    uint32_t kraft_sum = 0;
    for (size_t i = 0; i < count; ++i) {
        if (lengths[i] > 0) kraft_sum += (1u << (max_bits - lengths[i]));
    }

    while (kraft_sum > (1u << max_bits)) {
        for (int b = max_bits - 1; b >= 1; --b) {
            for (size_t i = 0; i < count; ++i) {
                if (lengths[i] == b) {
                    lengths[i]++;
                    kraft_sum -= (1u << (max_bits - (b + 1)));
                    if (kraft_sum <= (1u << max_bits)) break;
                }
            }
            if (kraft_sum <= (1u << max_bits)) break;
        }
    }

    while (kraft_sum < (1u << max_bits)) {
        bool changed = false;
        for (int b = max_bits; b >= 2; --b) {
            for (size_t i = 0; i < count; ++i) {
                if (lengths[i] == b) {
                    uint32_t delta = (1u << (max_bits - b));
                    if (kraft_sum + delta <= (1u << max_bits)) {
                        lengths[i]--;
                        kraft_sum += delta;
                        changed = true;
                        if (kraft_sum == (1u << max_bits)) break;
                    }
                }
            }
            if (kraft_sum == (1u << max_bits) || changed) break;
        }
        if (!changed) break;
    }

    std::vector<int> bl_count(max_bits + 1, 0);
    for (size_t i = 0; i < count; ++i) {
        if (lengths[i] > 0) bl_count[lengths[i]]++;
    }

    int code = 0;
    std::vector<int> next_code(max_bits + 1, 0);
    for (int bits = 1; bits <= max_bits; ++bits) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    for (size_t i = 0; i < count; ++i) {
        int len = lengths[i];
        if (len > 0) {
            uint16_t c = static_cast<uint16_t>(next_code[len]++);
            uint16_t rev = 0;
            for (int b = 0; b < len; ++b) {
                if ((c >> b) & 1) rev |= (1 << (len - 1 - b));
            }
            codes[i] = rev;
        }
    }
}

static void find_lz_matches(std::span<const uint8_t> input, const DeflateOptions& options, std::vector<LzToken>& tokens) {
    tokens.clear();
    const size_t n = input.size();
    if (n == 0) return;

    tokens.reserve(n / 2);
    const uint32_t window_size = 32768;
    const uint32_t hash_size = 65536;
    std::vector<int> head(hash_size, -1);
    std::vector<int> prev(window_size, -1);

    auto hash_bytes = [](uint8_t b0, uint8_t b1, uint8_t b2) -> uint32_t {
        return ((static_cast<uint32_t>(b0) << 8) ^ (static_cast<uint32_t>(b1) << 4) ^ b2) & 0xFFFF;
    };

    size_t pos = 0;
    while (pos < n) {
        if (pos + 3 > n) {
            tokens.push_back({0, 0, input[pos]});
            pos++;
            continue;
        }

        uint32_t h = hash_bytes(input[pos], input[pos + 1], input[pos + 2]);
        int match_pos = head[h];
        prev[pos & (window_size - 1)] = match_pos;
        head[h] = static_cast<int>(pos);

        uint32_t best_len = 0;
        uint32_t best_dist = 0;
        uint32_t chain = 0;

        auto get_match_len = [&](size_t s1, size_t s2, size_t limit) -> size_t {
            if (s1 + limit > n || s2 + limit > n) limit = std::min(n - s1, n - s2);
            size_t l = 0;
            while (l + 8 <= limit) {
                uint64_t v1, v2;
                std::memcpy(&v1, &input[s1 + l], sizeof(uint64_t));
                std::memcpy(&v2, &input[s2 + l], sizeof(uint64_t));
                if (v1 != v2) {
                    uint64_t diff = v1 ^ v2;
                    return l + (std::countr_zero(diff) >> 3);
                }
                l += 8;
            }
            while (l < limit && input[s1 + l] == input[s2 + l]) {
                l++;
            }
            return l;
        };

        while (match_pos != -1 && chain < options.max_chain && (pos - match_pos) < window_size) {
            size_t dist = pos - match_pos;
            size_t max_len = std::min<size_t>(258, n - pos);
            if (best_len >= 3) {
                if (input[match_pos + best_len] != input[pos + best_len] ||
                    input[match_pos + best_len - 1] != input[pos + best_len - 1] ||
                    input[match_pos] != input[pos]) {
                    match_pos = prev[match_pos & (window_size - 1)];
                    chain++;
                    continue;
                }
            }
            size_t len = get_match_len(match_pos, pos, max_len);
            if (len > best_len) {
                best_len = static_cast<uint32_t>(len);
                best_dist = static_cast<uint32_t>(dist);
                if (best_len >= options.nice_length) break;
            }
            match_pos = prev[match_pos & (window_size - 1)];
            chain++;
        }

        if (best_len >= 3) {
            if (options.lazy_matching && pos + 1 < n && best_len < 128) {
                uint32_t h2 = hash_bytes(input[pos + 1], input[pos + 2], (pos + 3 < n ? input[pos + 3] : 0));
                int match_pos2 = head[h2];
                uint32_t best_len2 = 0;
                uint32_t best_dist2 = 0;
                uint32_t chain2 = 0;
                while (match_pos2 != -1 && chain2 < (options.max_chain / 2) && (pos + 1 - match_pos2) < window_size) {
                    size_t dist2 = (pos + 1) - match_pos2;
                    size_t max_len2 = std::min<size_t>(258, n - (pos + 1));
                    if (best_len2 >= 3) {
                        if (input[match_pos2 + best_len2] != input[pos + 1 + best_len2] ||
                            input[match_pos2 + best_len2 - 1] != input[pos + 1 + best_len2 - 1] ||
                            input[match_pos2] != input[pos + 1]) {
                            match_pos2 = prev[match_pos2 & (window_size - 1)];
                            chain2++;
                            continue;
                        }
                    }
                    size_t len2 = get_match_len(match_pos2, pos + 1, max_len2);
                    if (len2 > best_len2) {
                        best_len2 = static_cast<uint32_t>(len2);
                        best_dist2 = static_cast<uint32_t>(dist2);
                    }
                    match_pos2 = prev[match_pos2 & (window_size - 1)];
                    chain2++;
                }
                if (best_len2 > best_len + 1) {
                    tokens.push_back({0, 0, input[pos]});
                    tokens.push_back({static_cast<uint16_t>(best_len2), static_cast<uint16_t>(best_dist2), 0});
                    for (size_t s = 1; s <= best_len2 && pos + s + 2 < n; ++s) {
                        uint32_t hs = hash_bytes(input[pos + s], input[pos + s + 1], input[pos + s + 2]);
                        prev[(pos + s) & (window_size - 1)] = head[hs];
                        head[hs] = static_cast<int>(pos + s);
                    }
                    pos += 1 + best_len2;
                    continue;
                }
            }

            tokens.push_back({static_cast<uint16_t>(best_len), static_cast<uint16_t>(best_dist), 0});
            for (size_t s = 1; s < best_len && pos + s + 2 < n; ++s) {
                uint32_t hs = hash_bytes(input[pos + s], input[pos + s + 1], input[pos + s + 2]);
                prev[(pos + s) & (window_size - 1)] = head[hs];
                head[hs] = static_cast<int>(pos + s);
            }
            pos += best_len;
        } else {
            tokens.push_back({0, 0, input[pos]});
            pos++;
        }
    }
}

static uint8_t get_len_code(uint16_t len) {
    for (uint8_t i = 0; i < 29; ++i) {
        if (len <= LENGTH_BASE[i] + ((1 << LENGTH_EXTRA[i]) - 1)) {
            return i;
        }
    }
    return 28;
}

static uint8_t get_dist_code(uint16_t dist) {
    for (uint8_t i = 0; i < 30; ++i) {
        if (dist <= DIST_BASE[i] + ((1 << DIST_EXTRA[i]) - 1)) {
            return i;
        }
    }
    return 29;
}

static void encode_tokens_dynamic(std::span<const LzToken> tokens, BitWriter& writer, bool bfinal = true) {
    std::array<uint32_t, 286> lit_freqs{};
    std::array<uint32_t, 30> dist_freqs{};

    for (const auto& tok : tokens) {
        if (!tok.is_match()) {
            lit_freqs[tok.literal]++;
        } else {
            uint8_t lc = get_len_code(tok.length);
            lit_freqs[257 + lc]++;
            uint8_t dc = get_dist_code(tok.distance);
            dist_freqs[dc]++;
        }
    }
    lit_freqs[256]++;

    std::vector<uint8_t> lit_lens;
    std::vector<uint16_t> lit_codes;
    compute_huffman_codes(lit_freqs.data(), 286, 15, lit_lens, lit_codes);

    std::vector<uint8_t> dist_lens;
    std::vector<uint16_t> dist_codes;
    compute_huffman_codes(dist_freqs.data(), 30, 15, dist_lens, dist_codes);

    int hlit = 286;
    while (hlit > 257 && lit_lens[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && dist_lens[hdist - 1] == 0) hdist--;

    std::vector<uint8_t> combined_lens;
    combined_lens.reserve(hlit + hdist);
    for (int i = 0; i < hlit; ++i) combined_lens.push_back(lit_lens[i]);
    for (int i = 0; i < hdist; ++i) combined_lens.push_back(dist_lens[i]);

    struct ClToken {
        uint8_t sym = 0;
        uint8_t extra = 0;
        uint8_t extra_bits = 0;
    };
    std::vector<ClToken> cl_tokens;
    std::array<uint32_t, 19> cl_freqs{};

    size_t idx = 0;
    while (idx < combined_lens.size()) {
        uint8_t cur = combined_lens[idx];
        size_t run = 1;
        while (idx + run < combined_lens.size() && combined_lens[idx + run] == cur) {
            run++;
        }
        size_t orig_run = run;

        if (cur == 0) {
            while (run >= 11) {
                size_t chunk = std::min<size_t>(run, 138);
                cl_tokens.push_back({18, static_cast<uint8_t>(chunk - 11), 7});
                cl_freqs[18]++;
                run -= chunk;
            }
            if (run >= 3) {
                cl_tokens.push_back({17, static_cast<uint8_t>(run - 3), 3});
                cl_freqs[17]++;
                run = 0;
            }
            while (run > 0) {
                cl_tokens.push_back({0, 0, 0});
                cl_freqs[0]++;
                run--;
            }
        } else {
            cl_tokens.push_back({cur, 0, 0});
            cl_freqs[cur]++;
            run--;
            while (run >= 3) {
                size_t chunk = std::min<size_t>(run, 6);
                cl_tokens.push_back({16, static_cast<uint8_t>(chunk - 3), 2});
                cl_freqs[16]++;
                run -= chunk;
            }
            while (run > 0) {
                cl_tokens.push_back({cur, 0, 0});
                cl_freqs[cur]++;
                run--;
            }
        }
        idx += orig_run;
    }

    std::vector<uint8_t> cl_code_lens;
    std::vector<uint16_t> cl_codes;
    compute_huffman_codes(cl_freqs.data(), 19, 7, cl_code_lens, cl_codes);

    int hclen = 19;
    while (hclen > 4 && cl_code_lens[CL_ORDER[hclen - 1]] == 0) hclen--;

    writer.write_bits(bfinal ? 1 : 0, 1);
    writer.write_bits(2, 2);

    writer.write_bits(hlit - 257, 5);
    writer.write_bits(hdist - 1, 5);
    writer.write_bits(hclen - 4, 4);

    for (int i = 0; i < hclen; ++i) {
        writer.write_bits(cl_code_lens[CL_ORDER[i]], 3);
    }

    for (const auto& ct : cl_tokens) {
        writer.write_bits(cl_codes[ct.sym], cl_code_lens[ct.sym]);
        if (ct.extra_bits > 0) {
            writer.write_bits(ct.extra, ct.extra_bits);
        }
    }

    for (const auto& tok : tokens) {
        if (!tok.is_match()) {
            writer.write_bits(lit_codes[tok.literal], lit_lens[tok.literal]);
        } else {
            uint8_t lc = get_len_code(tok.length);
            uint16_t sym = 257 + lc;
            writer.write_bits(lit_codes[sym], lit_lens[sym]);
            if (LENGTH_EXTRA[lc] > 0) {
                uint32_t extra = tok.length - LENGTH_BASE[lc];
                writer.write_bits(extra, LENGTH_EXTRA[lc]);
            }
            uint8_t dc = get_dist_code(tok.distance);
            writer.write_bits(dist_codes[dc], dist_lens[dc]);
            if (DIST_EXTRA[dc] > 0) {
                uint32_t extra = tok.distance - DIST_BASE[dc];
                writer.write_bits(extra, DIST_EXTRA[dc]);
            }
        }
    }
    writer.write_bits(lit_codes[256], lit_lens[256]);
}

struct FixedHuffmanTables {
    std::array<uint8_t, 288> lit_lens{};
    std::array<uint16_t, 288> lit_codes{};
    std::array<uint8_t, 32> dist_lens{};
    std::array<uint16_t, 32> dist_codes{};

    FixedHuffmanTables() {
        for (int i = 0; i <= 143; ++i) lit_lens[i] = 8;
        for (int i = 144; i <= 255; ++i) lit_lens[i] = 9;
        for (int i = 256; i <= 279; ++i) lit_lens[i] = 7;
        for (int i = 280; i <= 287; ++i) lit_lens[i] = 8;

        std::vector<int> bl_count(10, 0);
        for (size_t i = 0; i < 288; ++i) bl_count[lit_lens[i]]++;
        int code = 0;
        std::vector<int> next_code(10, 0);
        for (int bits = 1; bits <= 9; ++bits) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
        }
        for (size_t i = 0; i < 288; ++i) {
            int len = lit_lens[i];
            uint16_t c = static_cast<uint16_t>(next_code[len]++);
            uint16_t rev = 0;
            for (int b = 0; b < len; ++b) {
                if ((c >> b) & 1) rev |= (1 << (len - 1 - b));
            }
            lit_codes[i] = rev;
        }

        for (int i = 0; i < 32; ++i) dist_lens[i] = 5;
        for (size_t i = 0; i < 32; ++i) {
            uint16_t c = static_cast<uint16_t>(i);
            uint16_t rev = 0;
            for (int b = 0; b < 5; ++b) {
                if ((c >> b) & 1) rev |= (1 << (4 - b));
            }
            dist_codes[i] = rev;
        }
    }
};

static const FixedHuffmanTables& get_fixed_tables() {
    static const FixedHuffmanTables tables;
    return tables;
}

static void encode_tokens_fixed(std::span<const LzToken> tokens, BitWriter& writer, bool bfinal = true) {
    const auto& tbl = get_fixed_tables();
    writer.write_bits(bfinal ? 1 : 0, 1);
    writer.write_bits(1, 2);

    for (const auto& tok : tokens) {
        if (!tok.is_match()) {
            writer.write_bits(tbl.lit_codes[tok.literal], tbl.lit_lens[tok.literal]);
        } else {
            uint8_t lc = get_len_code(tok.length);
            uint16_t sym = 257 + lc;
            writer.write_bits(tbl.lit_codes[sym], tbl.lit_lens[sym]);
            if (LENGTH_EXTRA[lc] > 0) {
                uint32_t extra = tok.length - LENGTH_BASE[lc];
                writer.write_bits(extra, LENGTH_EXTRA[lc]);
            }
            uint8_t dc = get_dist_code(tok.distance);
            writer.write_bits(tbl.dist_codes[dc], tbl.dist_lens[dc]);
            if (DIST_EXTRA[dc] > 0) {
                uint32_t extra = tok.distance - DIST_BASE[dc];
                writer.write_bits(extra, DIST_EXTRA[dc]);
            }
        }
    }
    writer.write_bits(tbl.lit_codes[256], tbl.lit_lens[256]);
}

static size_t estimate_block_bits(std::span<const LzToken> tokens) {
    if (tokens.empty()) return 0;
    std::array<uint32_t, 286> lit_freqs{};
    std::array<uint32_t, 30> dist_freqs{};

    for (const auto& tok : tokens) {
        if (!tok.is_match()) {
            lit_freqs[tok.literal]++;
        } else {
            uint8_t lc = get_len_code(tok.length);
            lit_freqs[257 + lc]++;
            uint8_t dc = get_dist_code(tok.distance);
            dist_freqs[dc]++;
        }
    }
    lit_freqs[256]++;

    std::vector<uint8_t> lit_lens;
    std::vector<uint16_t> lit_codes;
    compute_huffman_codes(lit_freqs.data(), 286, 15, lit_lens, lit_codes);

    std::vector<uint8_t> dist_lens;
    std::vector<uint16_t> dist_codes;
    compute_huffman_codes(dist_freqs.data(), 30, 15, dist_lens, dist_codes);

    int hlit = 286;
    while (hlit > 257 && lit_lens[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && dist_lens[hdist - 1] == 0) hdist--;

    std::vector<uint8_t> combined_lens;
    combined_lens.reserve(hlit + hdist);
    for (int i = 0; i < hlit; ++i) combined_lens.push_back(lit_lens[i]);
    for (int i = 0; i < hdist; ++i) combined_lens.push_back(dist_lens[i]);

    struct ClToken {
        uint8_t sym = 0;
        uint8_t extra_bits = 0;
    };
    std::vector<ClToken> cl_tokens;
    std::array<uint32_t, 19> cl_freqs{};

    size_t idx = 0;
    while (idx < combined_lens.size()) {
        uint8_t cur = combined_lens[idx];
        size_t run = 1;
        while (idx + run < combined_lens.size() && combined_lens[idx + run] == cur) {
            run++;
        }
        size_t orig_run = run;

        if (cur == 0) {
            while (run >= 11) {
                size_t chunk = std::min<size_t>(run, 138);
                cl_tokens.push_back({18, 7});
                cl_freqs[18]++;
                run -= chunk;
            }
            if (run >= 3) {
                cl_tokens.push_back({17, 3});
                cl_freqs[17]++;
                run = 0;
            }
            while (run > 0) {
                cl_tokens.push_back({0, 0});
                cl_freqs[0]++;
                run--;
            }
        } else {
            cl_tokens.push_back({cur, 0});
            cl_freqs[cur]++;
            run--;
            while (run >= 3) {
                size_t chunk = std::min<size_t>(run, 6);
                cl_tokens.push_back({16, 2});
                cl_freqs[16]++;
                run -= chunk;
            }
            while (run > 0) {
                cl_tokens.push_back({cur, 0});
                cl_freqs[cur]++;
                run--;
            }
        }
        idx += orig_run;
    }

    std::vector<uint8_t> cl_code_lens;
    std::vector<uint16_t> cl_codes;
    compute_huffman_codes(cl_freqs.data(), 19, 7, cl_code_lens, cl_codes);

    int hclen = 19;
    while (hclen > 4 && cl_code_lens[CL_ORDER[hclen - 1]] == 0) hclen--;

    size_t total_bits = 3 + 5 + 5 + 4 + (hclen * 3);
    for (const auto& ct : cl_tokens) {
        total_bits += cl_code_lens[ct.sym] + ct.extra_bits;
    }
    for (size_t i = 0; i < 286; ++i) {
        if (lit_freqs[i] > 0) {
            total_bits += lit_freqs[i] * lit_lens[i];
            if (i >= 257 && i < 286) {
                total_bits += lit_freqs[i] * LENGTH_EXTRA[i - 257];
            }
        }
    }
    for (size_t i = 0; i < 30; ++i) {
        if (dist_freqs[i] > 0) {
            total_bits += dist_freqs[i] * dist_lens[i];
            total_bits += dist_freqs[i] * DIST_EXTRA[i];
        }
    }
    return total_bits;
}

static std::vector<uint8_t> encode_tokens_with_block_splitting(const std::vector<LzToken>& tokens) {
    if (tokens.size() < 1024) {
        BitWriter dyn_writer;
        encode_tokens_dynamic(tokens, dyn_writer, true);
        auto dyn_data = dyn_writer.take_data();

        BitWriter fix_writer;
        encode_tokens_fixed(tokens, fix_writer, true);
        auto fix_data = fix_writer.take_data();

        return (fix_data.size() < dyn_data.size()) ? fix_data : dyn_data;
    }

    std::vector<size_t> cuts = {0, tokens.size()};

    for (int pass = 0; pass < 2; ++pass) {
        std::vector<size_t> new_cuts;
        new_cuts.push_back(cuts[0]);
        for (size_t seg = 0; seg + 1 < cuts.size(); ++seg) {
            size_t seg_start = cuts[seg];
            size_t seg_end = cuts[seg + 1];
            size_t seg_len = seg_end - seg_start;
            if (seg_len >= 1024) {
                std::span<const LzToken> seg_span(tokens.data() + seg_start, seg_len);
                size_t base_cost = estimate_block_bits(seg_span);
                size_t best_split = 0;
                size_t best_cost = base_cost;
                size_t step = std::max<size_t>(256, seg_len / 16);

                for (size_t cand = seg_start + step; cand + step <= seg_end; cand += step) {
                    size_t left_len = cand - seg_start;
                    size_t right_len = seg_end - cand;
                    size_t cand_cost = estimate_block_bits(std::span<const LzToken>(tokens.data() + seg_start, left_len)) +
                                       estimate_block_bits(std::span<const LzToken>(tokens.data() + cand, right_len));
                    if (cand_cost + 128 < best_cost) {
                        best_cost = cand_cost;
                        best_split = cand;
                    }
                }
                if (best_split != 0) {
                    new_cuts.push_back(best_split);
                }
            }
            new_cuts.push_back(seg_end);
        }
        std::sort(new_cuts.begin(), new_cuts.end());
        new_cuts.erase(std::unique(new_cuts.begin(), new_cuts.end()), new_cuts.end());
        cuts = std::move(new_cuts);
    }

    BitWriter writer;
    for (size_t seg = 0; seg + 1 < cuts.size(); ++seg) {
        size_t s_start = cuts[seg];
        size_t s_len = cuts[seg + 1] - s_start;
        bool is_final = (seg + 2 == cuts.size());
        encode_tokens_dynamic(std::span<const LzToken>(tokens.data() + s_start, s_len), writer, is_final);
    }
    return writer.take_data();
}

static void find_lz_matches_optimal(std::span<const uint8_t> input, std::vector<LzToken>& tokens) {
    const size_t n = input.size();
    if (n == 0) return;

    const size_t window_size = 32768;
    const size_t hash_size = 65536;
    std::vector<int> head(hash_size, -1);
    std::vector<int> prev(window_size, -1);

    auto hash_bytes = [](uint8_t b0, uint8_t b1, uint8_t b2) -> uint32_t {
        return ((static_cast<uint32_t>(b0) << 10) ^ (static_cast<uint32_t>(b1) << 5) ^ static_cast<uint32_t>(b2)) & 0xFFFF;
    };

    struct MatchCandidate {
        uint16_t length = 0;
        uint16_t distance = 0;
    };

    auto get_match_len = [&](size_t s1, size_t s2, size_t limit) -> size_t {
        if (s1 + limit > n || s2 + limit > n) limit = std::min(n - s1, n - s2);
        size_t l = 0;
        while (l + 8 <= limit) {
            uint64_t v1, v2;
            std::memcpy(&v1, &input[s1 + l], sizeof(uint64_t));
            std::memcpy(&v2, &input[s2 + l], sizeof(uint64_t));
            if (v1 != v2) {
                uint64_t diff = v1 ^ v2;
                return l + (std::countr_zero(diff) >> 3);
            }
            l += 8;
        }
        while (l < limit && input[s1 + l] == input[s2 + l]) {
            l++;
        }
        return l;
    };

    std::vector<uint32_t> match_offsets(n + 1, 0);
    std::vector<MatchCandidate> match_pool;
    match_pool.reserve(n * 2);

    for (size_t pos = 0; pos < n; ++pos) {
        match_offsets[pos] = static_cast<uint32_t>(match_pool.size());
        if (pos + 2 < n) {
            uint32_t h = hash_bytes(input[pos], input[pos + 1], input[pos + 2]);
            int match_pos = head[h];
            uint32_t chain = 0;
            const uint32_t max_chain = 256;
            size_t longest_for_dist = 0;

            while (match_pos != -1 && chain < max_chain && (pos - match_pos) < window_size) {
                size_t dist = pos - match_pos;
                size_t max_len = std::min<size_t>(258, n - pos);
                if (longest_for_dist >= 3) {
                    if (input[match_pos + longest_for_dist] != input[pos + longest_for_dist] ||
                        input[match_pos + longest_for_dist - 1] != input[pos + longest_for_dist - 1] ||
                        input[match_pos] != input[pos]) {
                        match_pos = prev[match_pos & (window_size - 1)];
                        chain++;
                        continue;
                    }
                }
                size_t len = get_match_len(match_pos, pos, max_len);
                if (len >= 3 && len > longest_for_dist) {
                    longest_for_dist = len;
                    match_pool.push_back({static_cast<uint16_t>(len), static_cast<uint16_t>(dist)});
                    if (len == 258) break;
                }
                match_pos = prev[match_pos & (window_size - 1)];
                chain++;
            }
            prev[pos & (window_size - 1)] = head[h];
            head[h] = static_cast<int>(pos);
        }
    }
    match_offsets[n] = static_cast<uint32_t>(match_pool.size());

    std::array<float, 286> lit_costs{};
    std::array<float, 30> dist_costs{};
    for (size_t i = 0; i < 256; ++i) lit_costs[i] = 8.5f;
    lit_costs[256] = 8.0f;
    for (size_t i = 0; i < 29; ++i) lit_costs[257 + i] = 5.0f + LENGTH_EXTRA[i];
    for (size_t i = 0; i < 30; ++i) dist_costs[i] = 5.0f + DIST_EXTRA[i];

    auto match_cost = [&](uint16_t len, uint16_t dist) -> float {
        uint8_t lc = get_len_code(len);
        uint8_t dc = get_dist_code(dist);
        return lit_costs[257 + lc] + dist_costs[dc];
    };

    struct NodeEdge {
        uint16_t length = 0;
        uint16_t distance = 0;
        uint8_t literal = 0;
        size_t from = 0;
    };

    std::vector<float> min_cost(n + 1, 1e12f);
    std::vector<NodeEdge> best_edge(n + 1);

    int iterations = (n <= 65536) ? 2 : 1;
    for (int iter = 0; iter < iterations; ++iter) {
        std::fill(min_cost.begin(), min_cost.end(), 1e12f);
        min_cost[0] = 0.0f;

        for (size_t i = 0; i < n; ++i) {
            float cur_cost = min_cost[i];
            if (cur_cost >= 1e11f) continue;

            float lit_c = cur_cost + lit_costs[input[i]];
            if (lit_c < min_cost[i + 1]) {
                min_cost[i + 1] = lit_c;
                best_edge[i + 1] = {0, 0, input[i], i};
            }

            uint32_t m_start = match_offsets[i];
            uint32_t m_end = match_offsets[i + 1];
            for (uint32_t m_idx = m_start; m_idx < m_end; ++m_idx) {
                const auto& m = match_pool[m_idx];
                float mc = cur_cost + match_cost(m.length, m.distance);
                size_t target = i + m.length;
                if (mc < min_cost[target]) {
                    min_cost[target] = mc;
                    best_edge[target] = {m.length, m.distance, 0, i};
                }
            }
        }

        if (iter + 1 < iterations) {
            std::array<uint32_t, 286> l_freqs{};
            std::array<uint32_t, 30> d_freqs{};
            size_t curr = n;
            while (curr > 0) {
                const auto& edge = best_edge[curr];
                if (edge.length == 0) {
                    l_freqs[edge.literal]++;
                } else {
                    l_freqs[257 + get_len_code(edge.length)]++;
                    d_freqs[get_dist_code(edge.distance)]++;
                }
                curr = edge.from;
            }
            l_freqs[256]++;
            std::vector<uint8_t> l_lens, d_lens;
            std::vector<uint16_t> dummy_codes;
            compute_huffman_codes(l_freqs.data(), 286, 15, l_lens, dummy_codes);
            compute_huffman_codes(d_freqs.data(), 30, 15, d_lens, dummy_codes);

            for (size_t s = 0; s < 286; ++s) {
                float bl = (l_lens[s] > 0) ? static_cast<float>(l_lens[s]) : 15.0f;
                if (s >= 257) {
                    bl += LENGTH_EXTRA[s - 257];
                }
                lit_costs[s] = bl;
            }
            for (size_t s = 0; s < 30; ++s) {
                float bl = (d_lens[s] > 0) ? static_cast<float>(d_lens[s]) : 15.0f;
                bl += DIST_EXTRA[s];
                dist_costs[s] = bl;
            }
        }
    }

    tokens.clear();
    size_t curr = n;
    while (curr > 0) {
        const auto& edge = best_edge[curr];
        tokens.push_back({edge.length, edge.distance, edge.literal});
        curr = edge.from;
    }
    std::reverse(tokens.begin(), tokens.end());
}

std::vector<uint8_t> DeflateEngine::compress(std::span<const uint8_t> input, const DeflateOptions& options) {
    if (input.empty()) {
        return {0x03, 0x00};
    }

    std::vector<LzToken> tokens;
    find_lz_matches(input, options, tokens);

    if (options.level >= 9 && tokens.size() >= 1024) {
        auto split_res = encode_tokens_with_block_splitting(tokens);
        std::vector<uint8_t> test_decomp;
        if (!split_res.empty() && decompress(split_res, test_decomp) && test_decomp.size() == input.size() && std::memcmp(test_decomp.data(), input.data(), input.size()) == 0) {
            return split_res;
        }
    }

    BitWriter writer;
    encode_tokens_dynamic(tokens, writer);
    auto dyn_res = writer.take_data();

    if (tokens.size() < 1024) {
        BitWriter fix_writer;
        encode_tokens_fixed(tokens, fix_writer);
        auto fix_res = fix_writer.take_data();
        if (fix_res.size() < dyn_res.size()) {
            std::vector<uint8_t> test_d;
            if (decompress(fix_res, test_d) && test_d.size() == input.size() && std::memcmp(test_d.data(), input.data(), input.size()) == 0) {
                return fix_res;
            }
        }
    }

    std::vector<uint8_t> test_dyn;
    if (decompress(dyn_res, test_dyn) && test_dyn.size() == input.size() && std::memcmp(test_dyn.data(), input.data(), input.size()) == 0) {
        return dyn_res;
    }

    BitWriter fix_fallback;
    encode_tokens_fixed(tokens, fix_fallback);
    return fix_fallback.take_data();
}

std::vector<uint8_t> DeflateEngine::compress_best(std::span<const uint8_t> input) {
    if (input.empty()) {
        return {0x03, 0x00};
    }

    DeflateOptions opt1;
    opt1.level = 9;
    opt1.max_chain = 2048;
    opt1.nice_length = 258;
    opt1.lazy_matching = true;

    auto best_res = compress(input, opt1);

    if (input.size() <= 65536) {
        std::vector<LzToken> opt_tokens;
        find_lz_matches_optimal(input, opt_tokens);
        auto opt_res = encode_tokens_with_block_splitting(opt_tokens);

        std::vector<uint8_t> test_decomp;
        if (!opt_res.empty() && opt_res.size() < best_res.size() && decompress(opt_res, test_decomp) && test_decomp.size() == input.size() && std::memcmp(test_decomp.data(), input.data(), input.size()) == 0) {
            best_res = std::move(opt_res);
        }
    } else if (input.size() > 512) {
        DeflateOptions opt2;
        opt2.level = 12;
        opt2.max_chain = 4096;
        opt2.nice_length = 258;
        opt2.lazy_matching = true;
        std::vector<LzToken> opt2_tokens;
        find_lz_matches(input, opt2, opt2_tokens);
        auto res2 = encode_tokens_with_block_splitting(opt2_tokens);
        std::vector<uint8_t> test_decomp;
        if (!res2.empty() && res2.size() < best_res.size() && decompress(res2, test_decomp) && test_decomp.size() == input.size() && std::memcmp(test_decomp.data(), input.data(), input.size()) == 0) {
            best_res = std::move(res2);
        }
    }

    return best_res;
}

std::vector<uint8_t> DeflateEngine::zlib_compress(std::span<const uint8_t> input, const DeflateOptions& options) {
    auto compressed = compress(input, options);
    uint32_t adler = adler32(input);

    std::vector<uint8_t> out;
    out.reserve(compressed.size() + 6);
    out.push_back(0x78);
    out.push_back(0xDA);
    out.insert(out.end(), compressed.begin(), compressed.end());
    out.push_back(static_cast<uint8_t>((adler >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((adler >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((adler >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(adler & 0xFF));
    return out;
}

std::vector<uint8_t> DeflateEngine::zlib_compress_best(std::span<const uint8_t> input) {
    auto compressed = compress_best(input);
    uint32_t adler = adler32(input);

    std::vector<uint8_t> out;
    out.reserve(compressed.size() + 6);
    out.push_back(0x78);
    out.push_back(0xDA);
    out.insert(out.end(), compressed.begin(), compressed.end());
    out.push_back(static_cast<uint8_t>((adler >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((adler >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((adler >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(adler & 0xFF));
    return out;
}

}
