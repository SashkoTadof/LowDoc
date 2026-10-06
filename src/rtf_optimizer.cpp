#include "lowdoc/rtf_optimizer.hpp"
#include "lowdoc/image_optimizer.hpp"
#include <regex>
#include <stack>
#include <cctype>
#include <unordered_set>

namespace lowdoc {

bool RtfOptimizer::is_valid(std::string_view rtf) {
    if (!rtf.starts_with("{\\rtf")) return false;

    int depth = 0;
    bool escaped = false;

    for (char c : rtf) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth < 0) return false;
        }
    }

    return depth == 0;
}

static std::string remove_rtf_group(std::string_view rtf, std::string_view keyword) {
    std::string out;
    out.reserve(rtf.size());
    size_t i = 0;
    const size_t n = rtf.size();

    while (i < n) {
        if (rtf[i] == '{' && rtf.substr(i + 1).starts_with(keyword)) {
            int depth = 1;
            size_t j = i + 1;
            bool escaped = false;
            while (j < n && depth > 0) {
                if (escaped) {
                    escaped = false;
                    j++;
                    continue;
                }
                if (rtf[j] == '\\') {
                    escaped = true;
                    j++;
                    continue;
                }
                if (rtf[j] == '{') depth++;
                else if (rtf[j] == '}') depth--;
                j++;
            }
            if (depth == 0) {
                i = j;
                if (i < n && (rtf[i] == '\r' || rtf[i] == '\n')) {
                    i++;
                }
                continue;
            }
        }
        out.push_back(rtf[i]);
        i++;
    }
    return out;
}

static std::string optimize_rtf_pictures(std::string_view rtf) {
    std::string out;
    out.reserve(rtf.size());
    size_t i = 0;
    const size_t n = rtf.size();

    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    while (i < n) {
        if (rtf.substr(i).starts_with("\\pict")) {
            size_t blip_pos = rtf.find("\\pngblip", i);
            bool is_png = true;
            if (blip_pos == std::string_view::npos || blip_pos > i + 1000) {
                blip_pos = rtf.find("\\jpegblip", i);
                is_png = false;
            }

            if (blip_pos != std::string_view::npos && blip_pos < i + 1000) {
                size_t blip_tag_len = is_png ? 8 : 9;
                size_t hex_start = blip_pos + blip_tag_len;
                while (hex_start < n && std::isspace(static_cast<unsigned char>(rtf[hex_start]))) {
                    hex_start++;
                }

                int depth = 1;
                size_t hex_end = hex_start;
                while (hex_end < n && depth > 0) {
                    if (rtf[hex_end] == '{') depth++;
                    else if (rtf[hex_end] == '}') {
                        depth--;
                        if (depth == 0) break;
                    }
                    hex_end++;
                }

                if (depth == 0) {
                    std::vector<uint8_t> bin_data;
                    bin_data.reserve((hex_end - hex_start) / 2);
                    int high_nibble = -1;
                    for (size_t k = hex_start; k < hex_end; ++k) {
                        int v = hex_val(rtf[k]);
                        if (v >= 0) {
                            if (high_nibble == -1) {
                                high_nibble = v;
                            } else {
                                bin_data.push_back(static_cast<uint8_t>((high_nibble << 4) | v));
                                high_nibble = -1;
                            }
                        }
                    }

                    std::vector<uint8_t> opt_bin;
                    bool opt_ok = false;
                    if (is_png) {
                        opt_ok = ImageOptimizer::optimize_png(bin_data, opt_bin);
                    } else {
                        opt_ok = ImageOptimizer::optimize_jpeg(bin_data, opt_bin);
                    }

                    const std::vector<uint8_t>& target_bin = (opt_ok && opt_bin.size() < bin_data.size()) ? opt_bin : bin_data;

                    out.append(rtf.substr(i, hex_start - i));
                    out.push_back('\n');

                    static const char HEX_CHARS[] = "0123456789abcdef";
                    size_t col = 0;
                    for (uint8_t b : target_bin) {
                        out.push_back(HEX_CHARS[(b >> 4) & 0xF]);
                        out.push_back(HEX_CHARS[b & 0xF]);
                        col += 2;
                        if (col >= 128) {
                            out.push_back('\n');
                            col = 0;
                        }
                    }
                    if (col > 0) out.push_back('\n');

                    out.push_back('}');
                    i = hex_end + 1;
                    continue;
                }
            }
        }
        out.push_back(rtf[i++]);
    }
    return out;
}

static std::string optimize_rtf_fonttbl(std::string_view rtf) {
    size_t fonttbl_start = rtf.find("{\\fonttbl");
    if (fonttbl_start == std::string_view::npos) return std::string(rtf);

    int depth = 0;
    size_t fonttbl_end = std::string_view::npos;
    bool escaped = false;
    for (size_t i = fonttbl_start; i < rtf.size(); ++i) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (rtf[i] == '\\') {
            escaped = true;
            continue;
        }
        if (rtf[i] == '{') depth++;
        else if (rtf[i] == '}') {
            depth--;
            if (depth == 0) {
                fonttbl_end = i;
                break;
            }
        }
    }

    if (fonttbl_end == std::string_view::npos) return std::string(rtf);

    std::string_view ftbl = rtf.substr(fonttbl_start, fonttbl_end - fonttbl_start + 1);
    std::string body;
    body.reserve(rtf.size() - ftbl.size());
    body.append(rtf.substr(0, fonttbl_start));
    body.append(rtf.substr(fonttbl_end + 1));

    std::unordered_set<int> used_fonts;
    std::regex font_ref_regex(R"rx(\\(?:a?f)(\d+)\b)rx");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), font_ref_regex); it != std::sregex_iterator(); ++it) {
        used_fonts.insert(std::stoi((*it)[1].str()));
    }

    std::string new_ftbl = "{\\fonttbl";
    std::regex font_entry_regex(R"rx(\{\\f(\d+)\b[^{}]*\})rx");
    std::string ftbl_str(ftbl);
    for (auto it = std::sregex_iterator(ftbl_str.begin(), ftbl_str.end(), font_entry_regex); it != std::sregex_iterator(); ++it) {
        int fid = std::stoi((*it)[1].str());
        if (used_fonts.contains(fid)) {
            new_ftbl.append(it->str());
        }
    }
    new_ftbl.push_back('}');

    if (new_ftbl.size() >= ftbl.size()) return std::string(rtf);

    std::string res;
    res.reserve(rtf.size() - (ftbl.size() - new_ftbl.size()));
    res.append(rtf.substr(0, fonttbl_start));
    res.append(new_ftbl);
    res.append(rtf.substr(fonttbl_end + 1));
    return res;
}

static std::string normalize_rtf_state(std::string str) {
    const std::vector<std::pair<std::regex, std::string>> state_patterns = {
        { std::regex(R"rx((\\[a-z]+-?\d*)\s*\1\b)rx"), "$1" },
        { std::regex(R"rx(\\langfe(\d+)(?=[^\r\n{}\\]*\\langfe\1\b))rx"), "" },
        { std::regex(R"rx(\\ql(?=[^\r\n{}\\]*\\qj\b))rx"), "" },
        { std::regex(R"rx(\\sb0(?=[^\r\n{}\\]*\\sb0\b))rx"), "" },
        { std::regex(R"rx(\\sa0(?=[^\r\n{}\\]*\\sa0\b))rx"), "" },
        { std::regex(R"rx(\{\s*\})rx"), "" }
    };

    for (const auto& [rgx, rep] : state_patterns) {
        str = std::regex_replace(str, rgx, rep);
    }
    return str;
}

bool RtfOptimizer::optimize(std::string_view input, std::string& output, OptimizationReport& report) {
    if (!is_valid(input)) return false;

    std::string str(input);
    size_t orig_sz = str.size();

    str = remove_rtf_group(str, "\\*\\generator");
    str = remove_rtf_group(str, "\\info");
    str = optimize_rtf_pictures(str);
    str = optimize_rtf_fonttbl(str);
    str = normalize_rtf_state(std::move(str));

    std::regex trailing_ws_regex(R"([\r\n]+)");
    str = std::regex_replace(str, trailing_ws_regex, "\n");

    if (str.size() < orig_sz && is_valid(str)) {
        report.savings_by_pass["RTF Optimization"] += static_cast<int64_t>(orig_sz - str.size());
        output = std::move(str);
        return true;
    }

    return false;
}

}
