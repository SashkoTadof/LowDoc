#include "lowdoc/font_subsetter.hpp"
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace lowdoc {

static uint16_t read_u16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

static int16_t read_s16(const uint8_t* p) {
    return static_cast<int16_t>(read_u16(p));
}

static uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           p[3];
}

static void write_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

static void write_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

static uint32_t calc_table_checksum(const uint8_t* data, size_t length) {
    uint32_t sum = 0;
    size_t n = (length + 3) / 4;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        for (size_t b = 0; b < 4; ++b) {
            size_t idx = i * 4 + b;
            uint8_t byte = (idx < length) ? data[idx] : 0;
            v = (v << 8) | byte;
        }
        sum += v;
    }
    return sum;
}

static std::vector<uint8_t> parse_guid_key(const std::string& guid_str) {
    std::string hex;
    for (char c : guid_str) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            hex.push_back(c);
        }
    }
    if (hex.size() != 32) return {};

    std::vector<uint8_t> bytes(16);
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };

    for (size_t i = 0; i < 16; ++i) {
        bytes[i] = static_cast<uint8_t>((hex_val(hex[i * 2]) << 4) | hex_val(hex[i * 2 + 1]));
    }

    std::reverse(bytes.begin(), bytes.end());
    return bytes;
}

bool FontSubsetter::deobfuscate_odttf(std::span<const uint8_t> odttf_data, const std::string& guid_str, std::vector<uint8_t>& output) {
    if (odttf_data.size() < 32) return false;
    auto key = parse_guid_key(guid_str);
    if (key.size() != 16) return false;

    output.assign(odttf_data.begin(), odttf_data.end());
    for (size_t i = 0; i < 16; ++i) {
        output[i] ^= key[i];
    }
    for (size_t i = 0; i < 16; ++i) {
        output[16 + i] ^= key[i];
    }
    return true;
}

bool FontSubsetter::obfuscate_odttf(std::span<const uint8_t> ttf_data, const std::string& guid_str, std::vector<uint8_t>& output) {
    return deobfuscate_odttf(ttf_data, guid_str, output);
}

struct TableRecord {
    uint32_t tag = 0;
    uint32_t checkSum = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
};

static uint32_t make_tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d));
}

bool FontSubsetter::subset_ttf(std::span<const uint8_t> ttf_data, const std::unordered_set<uint32_t>& used_codepoints, std::vector<uint8_t>& output) {
    if (ttf_data.size() < 12) return false;
    uint32_t sfnt_version = read_u32(ttf_data.data());
    if (sfnt_version != 0x00010000 && sfnt_version != 0x74727565) {
        return false;
    }

    uint16_t num_tables = read_u16(ttf_data.data() + 4);
    if (ttf_data.size() < 12 + num_tables * 16) return false;

    std::unordered_map<uint32_t, TableRecord> tables;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* tr = ttf_data.data() + 12 + i * 16;
        TableRecord rec;
        rec.tag = read_u32(tr);
        rec.checkSum = read_u32(tr + 4);
        rec.offset = read_u32(tr + 8);
        rec.length = read_u32(tr + 12);
        if (rec.offset + rec.length > ttf_data.size()) return false;
        tables[rec.tag] = rec;
    }

    uint32_t tag_head = make_tag('h', 'e', 'a', 'd');
    uint32_t tag_maxp = make_tag('m', 'a', 'x', 'p');
    uint32_t tag_loca = make_tag('l', 'o', 'c', 'a');
    uint32_t tag_glyf = make_tag('g', 'l', 'y', 'f');
    uint32_t tag_cmap = make_tag('c', 'm', 'a', 'p');

    if (!tables.contains(tag_head) || !tables.contains(tag_maxp) ||
        !tables.contains(tag_loca) || !tables.contains(tag_glyf) ||
        !tables.contains(tag_cmap)) {
        return false;
    }

    const auto& rec_head = tables[tag_head];
    const auto& rec_maxp = tables[tag_maxp];
    const auto& rec_loca = tables[tag_loca];
    const auto& rec_glyf = tables[tag_glyf];
    const auto& rec_cmap = tables[tag_cmap];

    if (rec_head.length < 54 || rec_maxp.length < 6) return false;
    int16_t index_to_loc_format = read_s16(ttf_data.data() + rec_head.offset + 50);
    uint16_t num_glyphs = read_u16(ttf_data.data() + rec_maxp.offset + 4);
    if (num_glyphs == 0) return false;

    size_t expected_loca_len = (index_to_loc_format == 0) ? (num_glyphs + 1) * 2 : (num_glyphs + 1) * 4;
    if (rec_loca.length < expected_loca_len) return false;

    std::vector<uint32_t> old_loca(num_glyphs + 1);
    const uint8_t* loca_ptr = ttf_data.data() + rec_loca.offset;
    for (uint32_t g = 0; g <= num_glyphs; ++g) {
        if (index_to_loc_format == 0) {
            old_loca[g] = static_cast<uint32_t>(read_u16(loca_ptr + g * 2)) * 2;
        } else {
            old_loca[g] = read_u32(loca_ptr + g * 4);
        }
        if (old_loca[g] > rec_glyf.length) return false;
    }

    std::unordered_set<uint16_t> needed_gids;
    needed_gids.insert(0);

    const uint8_t* cmap_ptr = ttf_data.data() + rec_cmap.offset;
    if (rec_cmap.length >= 4) {
        uint16_t cmap_tables = read_u16(cmap_ptr + 2);
        for (uint16_t t = 0; t < cmap_tables && 4 + static_cast<uint32_t>(t + 1) * 8 <= rec_cmap.length; ++t) {
            uint32_t sub_off = read_u32(cmap_ptr + 4 + t * 8 + 4);
            if (sub_off + 6 > rec_cmap.length) continue;
            const uint8_t* sub = cmap_ptr + sub_off;
            uint16_t format = read_u16(sub);

            if (format == 4 && sub_off + 14 <= rec_cmap.length) {
                uint16_t seg_count_x2 = read_u16(sub + 6);
                uint16_t seg_count = seg_count_x2 / 2;
                if (sub_off + 16 + seg_count * 8 <= rec_cmap.length) {
                    const uint8_t* end_codes = sub + 14;
                    const uint8_t* start_codes = sub + 16 + seg_count * 2;
                    const uint8_t* id_deltas = sub + 16 + seg_count * 4;
                    const uint8_t* id_range_offsets = sub + 16 + seg_count * 6;

                    for (uint32_t cp : used_codepoints) {
                        if (cp > 0xFFFF) continue;
                        for (uint16_t s = 0; s < seg_count; ++s) {
                            uint16_t start_c = read_u16(start_codes + s * 2);
                            uint16_t end_c = read_u16(end_codes + s * 2);
                            if (cp >= start_c && cp <= end_c) {
                                int16_t delta = read_s16(id_deltas + s * 2);
                                uint16_t roff = read_u16(id_range_offsets + s * 2);
                                uint16_t gid = 0;
                                if (roff == 0) {
                                    gid = static_cast<uint16_t>((cp + delta) & 0xFFFF);
                                } else {
                                    const uint8_t* gptr = id_range_offsets + s * 2 + roff + (cp - start_c) * 2;
                                    if (gptr + 2 <= cmap_ptr + rec_cmap.length) {
                                        gid = read_u16(gptr);
                                        if (gid != 0) {
                                            gid = static_cast<uint16_t>((gid + delta) & 0xFFFF);
                                        }
                                    }
                                }
                                if (gid < num_glyphs) {
                                    needed_gids.insert(gid);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    const uint8_t* glyf_ptr = ttf_data.data() + rec_glyf.offset;
    std::vector<uint16_t> queue(needed_gids.begin(), needed_gids.end());
    size_t qhead = 0;
    while (qhead < queue.size()) {
        uint16_t gid = queue[qhead++];
        if (gid >= num_glyphs) continue;
        uint32_t g_start = old_loca[gid];
        uint32_t g_end = old_loca[gid + 1];
        if (g_end <= g_start || g_end - g_start < 10) continue;

        int16_t num_contours = read_s16(glyf_ptr + g_start);
        if (num_contours < 0) {
            size_t comp_pos = g_start + 10;
            while (comp_pos + 4 <= g_end) {
                uint16_t flags = read_u16(glyf_ptr + comp_pos);
                uint16_t comp_gid = read_u16(glyf_ptr + comp_pos + 2);
                comp_pos += 4;
                if (comp_gid < num_glyphs && !needed_gids.contains(comp_gid)) {
                    needed_gids.insert(comp_gid);
                    queue.push_back(comp_gid);
                }

                if (flags & 0x0001) comp_pos += 4;
                else comp_pos += 2;

                if (flags & 0x0008) comp_pos += 2;
                else if (flags & 0x0040) comp_pos += 4;
                else if (flags & 0x0080) comp_pos += 8;

                if (!(flags & 0x0020)) break;
            }
        }
    }

    std::vector<uint8_t> new_glyf;
    new_glyf.reserve(rec_glyf.length / 2);
    std::vector<uint32_t> new_loca(num_glyphs + 1, 0);

    for (uint32_t g = 0; g < num_glyphs; ++g) {
        new_loca[g] = static_cast<uint32_t>(new_glyf.size());
        if (needed_gids.contains(static_cast<uint16_t>(g))) {
            uint32_t g_start = old_loca[g];
            uint32_t g_end = old_loca[g + 1];
            if (g_end > g_start) {
                new_glyf.insert(new_glyf.end(), glyf_ptr + g_start, glyf_ptr + g_end);
            }
        }
    }
    new_loca[num_glyphs] = static_cast<uint32_t>(new_glyf.size());

    if (new_glyf.size() >= rec_glyf.length) {
        return false;
    }

    std::vector<uint8_t> new_loca_bytes(expected_loca_len);
    for (uint32_t g = 0; g <= num_glyphs; ++g) {
        if (index_to_loc_format == 0) {
            write_u16(new_loca_bytes.data() + g * 2, static_cast<uint16_t>(new_loca[g] / 2));
        } else {
            write_u32(new_loca_bytes.data() + g * 4, new_loca[g]);
        }
    }

    std::vector<uint8_t> new_head(rec_head.length);
    std::memcpy(new_head.data(), ttf_data.data() + rec_head.offset, rec_head.length);
    write_u32(new_head.data() + 8, 0);

    std::vector<TableRecord> out_records;
    std::vector<std::vector<uint8_t>> out_data;

    auto is_drop_table = [](uint32_t tag) {
        return tag == make_tag('f', 'v', 'a', 'r') ||
               tag == make_tag('g', 'v', 'a', 'r') ||
               tag == make_tag('c', 'v', 'a', 'r') ||
               tag == make_tag('H', 'V', 'A', 'R') ||
               tag == make_tag('V', 'V', 'A', 'R') ||
               tag == make_tag('M', 'V', 'A', 'R') ||
               tag == make_tag('a', 'v', 'a', 'r') ||
               tag == make_tag('V', 'D', 'M', 'X') ||
               tag == make_tag('L', 'T', 'S', 'H') ||
               tag == make_tag('h', 'd', 'm', 'x') ||
               tag == make_tag('f', 'p', 'g', 'm') ||
               tag == make_tag('p', 'r', 'e', 'p') ||
               tag == make_tag('c', 'v', 't', ' ');
    };

    for (uint16_t i = 0; i < num_tables; ++i) {
        const uint8_t* tr = ttf_data.data() + 12 + i * 16;
        uint32_t tag = read_u32(tr);
        if (is_drop_table(tag)) continue;

        TableRecord r;
        r.tag = tag;
        uint32_t tag_post = make_tag('p', 'o', 's', 't');
        if (tag == tag_glyf) {
            r.length = static_cast<uint32_t>(new_glyf.size());
            r.checkSum = calc_table_checksum(new_glyf.data(), new_glyf.size());
            out_records.push_back(r);
            out_data.push_back(new_glyf);
        } else if (tag == tag_loca) {
            r.length = static_cast<uint32_t>(new_loca_bytes.size());
            r.checkSum = calc_table_checksum(new_loca_bytes.data(), new_loca_bytes.size());
            out_records.push_back(r);
            out_data.push_back(new_loca_bytes);
        } else if (tag == tag_head) {
            r.length = static_cast<uint32_t>(new_head.size());
            r.checkSum = calc_table_checksum(new_head.data(), new_head.size());
            out_records.push_back(r);
            out_data.push_back(new_head);
        } else if (tag == tag_post) {
            const auto& orig_rec = tables[tag];
            if (orig_rec.length > 32) {
                std::vector<uint8_t> new_post(32);
                std::memcpy(new_post.data(), ttf_data.data() + orig_rec.offset, 32);
                write_u32(new_post.data(), 0x00030000);
                r.length = 32;
                r.checkSum = calc_table_checksum(new_post.data(), 32);
                out_records.push_back(r);
                out_data.push_back(std::move(new_post));
            } else {
                r.length = orig_rec.length;
                r.checkSum = orig_rec.checkSum;
                out_records.push_back(r);
                std::vector<uint8_t> d(orig_rec.length);
                std::memcpy(d.data(), ttf_data.data() + orig_rec.offset, orig_rec.length);
                out_data.push_back(std::move(d));
            }
        } else {
            const auto& orig_rec = tables[tag];
            r.length = orig_rec.length;
            r.checkSum = orig_rec.checkSum;
            out_records.push_back(r);
            std::vector<uint8_t> d(orig_rec.length);
            std::memcpy(d.data(), ttf_data.data() + orig_rec.offset, orig_rec.length);
            out_data.push_back(std::move(d));
        }
    }

    uint16_t out_num_tables = static_cast<uint16_t>(out_records.size());
    size_t header_len = 12 + out_num_tables * 16;
    size_t cur_offset = header_len;
    for (size_t i = 0; i < out_records.size(); ++i) {
        cur_offset = (cur_offset + 3) & ~3;
        out_records[i].offset = static_cast<uint32_t>(cur_offset);
        cur_offset += out_records[i].length;
    }

    std::vector<uint8_t> built(cur_offset, 0);
    std::memcpy(built.data(), ttf_data.data(), 12);
    write_u16(built.data() + 4, out_num_tables);
    uint16_t entry_selector = 0;
    while ((1u << (entry_selector + 1)) <= out_num_tables) entry_selector++;
    uint16_t search_range = (1u << entry_selector) * 16;
    uint16_t range_shift = out_num_tables * 16 - search_range;
    write_u16(built.data() + 6, search_range);
    write_u16(built.data() + 8, entry_selector);
    write_u16(built.data() + 10, range_shift);

    for (size_t i = 0; i < out_records.size(); ++i) {
        uint8_t* tr = built.data() + 12 + i * 16;
        write_u32(tr, out_records[i].tag);
        write_u32(tr + 4, out_records[i].checkSum);
        write_u32(tr + 8, out_records[i].offset);
        write_u32(tr + 12, out_records[i].length);

        std::memcpy(built.data() + out_records[i].offset, out_data[i].data(), out_data[i].size());
    }

    uint32_t whole_sum = calc_table_checksum(built.data(), built.size());
    uint32_t check_sum_adj = 0xB1B0AFBA - whole_sum;

    for (size_t i = 0; i < out_records.size(); ++i) {
        if (out_records[i].tag == tag_head) {
            write_u32(built.data() + out_records[i].offset + 8, check_sum_adj);
            break;
        }
    }

    if (built.size() < ttf_data.size()) {
        output = std::move(built);
        return true;
    }

    return false;
}

bool FontSubsetter::optimize_cff(std::span<const uint8_t> cff_data, std::vector<uint8_t>& output) {
    if (cff_data.size() < 4) return false;
    if (cff_data[0] != 1 || cff_data[1] != 0) return false;
    uint8_t hdr_size = cff_data[2];
    if (hdr_size < 4 || hdr_size > cff_data.size()) return false;

    struct CffIndex {
        std::vector<std::vector<uint8_t>> items;
    };

    auto read_index = [](const uint8_t* base, size_t max_len, size_t& offset, CffIndex& idx) -> bool {
        if (offset + 2 > max_len) return false;
        uint16_t count = read_u16(base + offset);
        offset += 2;
        if (count == 0) return true;
        if (offset + 1 > max_len) return false;
        uint8_t off_size = base[offset++];
        if (off_size < 1 || off_size > 4) return false;
        if (offset + (count + 1) * off_size > max_len) return false;

        std::vector<size_t> offsets(count + 1);
        for (size_t i = 0; i <= count; ++i) {
            size_t v = 0;
            for (uint8_t b = 0; b < off_size; ++b) {
                v = (v << 8) | base[offset++];
            }
            offsets[i] = v;
        }
        size_t data_start = offset;
        size_t data_len = offsets[count] - 1;
        if (data_start + data_len > max_len) return false;

        idx.items.resize(count);
        for (size_t i = 0; i < count; ++i) {
            size_t start = offsets[i] - 1;
            size_t end = offsets[i + 1] - 1;
            if (end < start || data_start + end > max_len) return false;
            idx.items[i].assign(base + data_start + start, base + data_start + end);
        }
        offset = data_start + data_len;
        return true;
    };

    auto write_index = [](const CffIndex& idx, std::vector<uint8_t>& out) {
        uint16_t count = static_cast<uint16_t>(idx.items.size());
        if (count == 0) {
            out.push_back(0);
            out.push_back(0);
            return;
        }
        size_t total_data = 0;
        for (const auto& item : idx.items) total_data += item.size();

        uint8_t off_size = 1;
        if (total_data + 1 > 0xFFFFFF) off_size = 4;
        else if (total_data + 1 > 0xFFFF) off_size = 3;
        else if (total_data + 1 > 0xFF) off_size = 2;

        out.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(count & 0xFF));
        out.push_back(off_size);

        size_t cur = 1;
        auto append_offset = [&](size_t val) {
            for (int b = off_size - 1; b >= 0; --b) {
                out.push_back(static_cast<uint8_t>((val >> (b * 8)) & 0xFF));
            }
        };
        append_offset(cur);
        for (const auto& item : idx.items) {
            cur += item.size();
            append_offset(cur);
        }
        for (const auto& item : idx.items) {
            out.insert(out.end(), item.begin(), item.end());
        }
    };

    size_t offset = hdr_size;
    CffIndex name_idx, top_dict_idx, string_idx, global_subrs_idx;
    if (!read_index(cff_data.data(), cff_data.size(), offset, name_idx)) return false;
    if (!read_index(cff_data.data(), cff_data.size(), offset, top_dict_idx)) return false;
    if (!read_index(cff_data.data(), cff_data.size(), offset, string_idx)) return false;
    if (!read_index(cff_data.data(), cff_data.size(), offset, global_subrs_idx)) return false;

    if (top_dict_idx.items.empty()) return false;

    const auto& top_dict = top_dict_idx.items[0];
    int64_t charstrings_off = -1;

    std::vector<int64_t> operands;
    size_t dp = 0;
    while (dp < top_dict.size()) {
        uint8_t b0 = top_dict[dp++];
        if (b0 >= 32 && b0 <= 246) {
            operands.push_back(static_cast<int64_t>(b0) - 139);
        } else if (b0 >= 247 && b0 <= 250) {
            if (dp >= top_dict.size()) return false;
            uint8_t b1 = top_dict[dp++];
            operands.push_back((static_cast<int64_t>(b0) - 247) * 256 + b1 + 108);
        } else if (b0 >= 251 && b0 <= 254) {
            if (dp >= top_dict.size()) return false;
            uint8_t b1 = top_dict[dp++];
            operands.push_back(-(static_cast<int64_t>(b0) - 251) * 256 - b1 - 108);
        } else if (b0 == 28) {
            if (dp + 2 > top_dict.size()) return false;
            int16_t v = read_s16(top_dict.data() + dp);
            dp += 2;
            operands.push_back(v);
        } else if (b0 == 29) {
            if (dp + 4 > top_dict.size()) return false;
            int32_t v = static_cast<int32_t>(read_u32(top_dict.data() + dp));
            dp += 4;
            operands.push_back(v);
        } else if (b0 == 30) {
            while (dp < top_dict.size()) {
                uint8_t b = top_dict[dp++];
                if ((b & 0x0F) == 0x0F || ((b >> 4) & 0x0F) == 0x0F) break;
            }
            operands.push_back(0);
        } else if (b0 == 12) {
            if (dp >= top_dict.size()) return false;
            dp++;
            operands.clear();
        } else {
            if (b0 == 17 && !operands.empty()) {
                charstrings_off = operands.back();
            }
            operands.clear();
        }
    }

    if (charstrings_off <= 0 || static_cast<size_t>(charstrings_off) >= cff_data.size()) return false;

    size_t cs_off = static_cast<size_t>(charstrings_off);
    CffIndex charstrings_idx;
    if (!read_index(cff_data.data(), cff_data.size(), cs_off, charstrings_idx)) return false;

    std::unordered_map<std::string, int> pattern_counts;
    for (const auto& cs : charstrings_idx.items) {
        if (cs.size() < 12) continue;
        for (size_t len = 6; len <= 10; len += 2) {
            if (cs.size() < len) continue;
            for (size_t i = 0; i + len <= cs.size(); i += 2) {
                std::string pat(reinterpret_cast<const char*>(cs.data() + i), len);
                pattern_counts[pat]++;
            }
        }
    }

    std::string best_pat;
    int max_saved = 0;
    for (const auto& [pat, cnt] : pattern_counts) {
        if (cnt >= 3) {
            int pat_len = static_cast<int>(pat.size());
            int saved = cnt * (pat_len - 2) - pat_len - 1;
            if (saved > max_saved) {
                max_saved = saved;
                best_pat = pat;
            }
        }
    }

    if (max_saved <= 8 || best_pat.empty()) return false;

    std::vector<uint8_t> new_subr(best_pat.begin(), best_pat.end());
    new_subr.push_back(11);

    size_t subr_idx = global_subrs_idx.items.size();
    global_subrs_idx.items.push_back(std::move(new_subr));

    int n_subrs = static_cast<int>(global_subrs_idx.items.size());
    int bias = 107;
    if (n_subrs >= 33900) bias = 32768;
    else if (n_subrs >= 1240) bias = 1131;

    int biased_idx = static_cast<int>(subr_idx) - bias;
    std::vector<uint8_t> call_seq;
    if (biased_idx >= -107 && biased_idx <= 107) {
        call_seq.push_back(static_cast<uint8_t>(biased_idx + 139));
    } else if (biased_idx >= 108 && biased_idx <= 1131) {
        call_seq.push_back(static_cast<uint8_t>((biased_idx - 108) / 256 + 247));
        call_seq.push_back(static_cast<uint8_t>((biased_idx - 108) % 256));
    } else if (biased_idx >= -1131 && biased_idx <= -108) {
        call_seq.push_back(static_cast<uint8_t>((-biased_idx - 108) / 256 + 251));
        call_seq.push_back(static_cast<uint8_t>((-biased_idx - 108) % 256));
    } else {
        call_seq.push_back(28);
        call_seq.push_back(static_cast<uint8_t>((biased_idx >> 8) & 0xFF));
        call_seq.push_back(static_cast<uint8_t>(biased_idx & 0xFF));
    }
    call_seq.push_back(29);

    for (auto& cs : charstrings_idx.items) {
        if (cs.size() < best_pat.size()) continue;
        std::vector<uint8_t> new_cs;
        new_cs.reserve(cs.size());
        size_t p = 0;
        while (p < cs.size()) {
            if (p + best_pat.size() <= cs.size() &&
                std::memcmp(cs.data() + p, best_pat.data(), best_pat.size()) == 0) {
                new_cs.insert(new_cs.end(), call_seq.begin(), call_seq.end());
                p += best_pat.size();
            } else {
                new_cs.push_back(cs[p++]);
            }
        }
        cs = std::move(new_cs);
    }

    std::vector<uint8_t> new_cff;
    new_cff.insert(new_cff.end(), cff_data.begin(), cff_data.begin() + hdr_size);
    write_index(name_idx, new_cff);

    std::vector<uint8_t> new_cs_bytes;
    write_index(charstrings_idx, new_cs_bytes);

    std::vector<uint8_t> new_gsubrs_bytes;
    write_index(global_subrs_idx, new_gsubrs_bytes);

    write_index(top_dict_idx, new_cff);
    write_index(string_idx, new_cff);
    new_cff.insert(new_cff.end(), new_gsubrs_bytes.begin(), new_gsubrs_bytes.end());

    new_cff.insert(new_cff.end(), new_cs_bytes.begin(), new_cs_bytes.end());

    if (cs_off + 2 < cff_data.size()) {
        uint16_t orig_cs_count = read_u16(cff_data.data() + cs_off);
        uint8_t orig_off_size = cff_data[cs_off + 2];
        size_t orig_cs_end = cs_off + 3 + (orig_cs_count + 1) * orig_off_size;
        if (orig_cs_end < cff_data.size()) {
            new_cff.insert(new_cff.end(), cff_data.begin() + orig_cs_end, cff_data.end());
        }
    }

    if (new_cff.size() < cff_data.size()) {
        output = std::move(new_cff);
        return true;
    }

    return false;
}

}
