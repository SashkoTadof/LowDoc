#include "lowdoc/html_optimizer.hpp"
#include "lowdoc/image_optimizer.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace lowdoc {

static bool base64_decode(std::string_view in, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(in.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else if (std::isspace(static_cast<unsigned char>(c))) continue;
        else return false;

        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

static std::string base64_encode(std::span<const uint8_t> in) {
    static const char B64_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    uint32_t buf = 0;
    int bits = 0;
    for (uint8_t b : in) {
        buf = (buf << 8) | b;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(B64_CHARS[(buf >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        buf <<= (6 - bits);
        out.push_back(B64_CHARS[buf & 0x3F]);
        while (out.size() % 4 != 0) {
            out.push_back('=');
        }
    }
    return out;
}

static std::string strip_html_comments(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    const size_t n = html.size();
    const char c_open[] = {'<', '!', '-', '-', '\0'};
    const char c_close[] = {'-', '-', '>', '\0'};

    while (i < n) {
        if (html.substr(i).starts_with(c_open)) {
            size_t end = html.find(c_close, i + 4);
            if (end != std::string_view::npos) {
                i = end + 3;
                continue;
            }
        }
        out.push_back(html[i++]);
    }
    return out;
}

bool HtmlOptimizer::optimize(std::string_view input, std::string& output, OptimizationReport& report) {
    std::string str = strip_html_comments(input);
    size_t orig_sz = input.size();

    std::string out;
    out.reserve(str.size());
    size_t i = 0;
    const size_t n = str.size();

    int64_t image_savings = 0;

    while (i < n) {
        std::string_view rem = std::string_view(str).substr(i);
        if (rem.starts_with("data:image/")) {
            size_t sub_start = i + 11;
            size_t semicolon = str.find(";base64,", sub_start);
            if (semicolon != std::string_view::npos && semicolon < sub_start + 16) {
                std::string_view img_sub = std::string_view(str).substr(sub_start, semicolon - sub_start);
                size_t data_start = semicolon + 8;
                size_t data_end = data_start;
                while (data_end < n && (std::isalnum(static_cast<unsigned char>(str[data_end])) ||
                                        str[data_end] == '+' || str[data_end] == '/' || str[data_end] == '=' ||
                                        std::isspace(static_cast<unsigned char>(str[data_end])))) {
                    data_end++;
                }

                std::string_view b64_str = std::string_view(str).substr(data_start, data_end - data_start);
                std::vector<uint8_t> bin_data;
                if (base64_decode(b64_str, bin_data) && !bin_data.empty()) {
                    std::vector<uint8_t> opt_bin;
                    bool opt_ok = false;
                    if (img_sub == "png") {
                        opt_ok = ImageOptimizer::optimize_png(bin_data, opt_bin);
                    } else if (img_sub == "jpeg" || img_sub == "jpg") {
                        opt_ok = ImageOptimizer::optimize_jpeg(bin_data, opt_bin);
                    }

                    if (opt_ok && opt_bin.size() < bin_data.size()) {
                        std::string new_b64 = base64_encode(opt_bin);
                        if (new_b64.size() < b64_str.size()) {
                            image_savings += static_cast<int64_t>(b64_str.size() - new_b64.size());
                            out.append(str.substr(i, data_start - i));
                            out.append(new_b64);
                            i = data_end;
                            continue;
                        }
                    }
                }
            }
        }
        out.push_back(str[i++]);
    }

    if (image_savings > 0) {
        report.savings_by_pass["Image Optimization"] += image_savings;
    }

    if (out.size() < orig_sz) {
        report.savings_by_pass["HTML Minification"] += static_cast<int64_t>(orig_sz - out.size() - image_savings);
        output = std::move(out);
        return true;
    }

    return false;
}

}
