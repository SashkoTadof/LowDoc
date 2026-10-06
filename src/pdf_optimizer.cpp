#include "lowdoc/pdf_optimizer.hpp"
#include "lowdoc/deflate.hpp"
#include "lowdoc/validator.hpp"
#include "lowdoc/font_subsetter.hpp"
#include "lowdoc/zip_archive.hpp"
#include "lowdoc/format_detector.hpp"
#include "lowdoc/ooxml_optimizer.hpp"
#include "lowdoc/odf_optimizer.hpp"
#include <string_view>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <algorithm>
#include <regex>

namespace lowdoc {

bool PdfOptimizer::is_signed(std::span<const uint8_t> data) {
    std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
    if (sv.find("/ByteRange") != std::string_view::npos) {
        if (sv.find("/Sig") != std::string_view::npos || sv.find("/DocChecksum") != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool PdfOptimizer::is_encrypted(std::span<const uint8_t> data) {
    std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
    return sv.find("/Encrypt") != std::string_view::npos;
}

struct PdfRawObject {
    int id = 0;
    int gen = 0;
    std::string header_dict;
    std::vector<uint8_t> stream_data;
    bool has_stream = false;
    std::string body;
};

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

static bool parse_pdf_objects(std::string_view pdf_str, std::unordered_map<int, PdfRawObject>& objects, std::string& trailer_dict) {
    const size_t n = pdf_str.size();

    std::regex obj_regex(R"((\d+)\s+(\d+)\s+obj)");
    auto words_begin = std::cregex_iterator(pdf_str.data(), pdf_str.data() + n, obj_regex);
    auto words_end = std::cregex_iterator();

    std::vector<std::pair<size_t, std::pair<int, int>>> obj_positions;
    for (auto it = words_begin; it != words_end; ++it) {
        size_t m_pos = it->position();
        int id = std::stoi((*it)[1].str());
        int gen = std::stoi((*it)[2].str());
        obj_positions.push_back({m_pos, {id, gen}});
    }

    for (size_t i = 0; i < obj_positions.size(); ++i) {
        size_t start = obj_positions[i].first;
        int id = obj_positions[i].second.first;
        int gen = obj_positions[i].second.second;

        size_t next_start = (i + 1 < obj_positions.size()) ? obj_positions[i + 1].first : n;
        size_t endobj_pos = pdf_str.find("endobj", start);
        if (endobj_pos == std::string_view::npos || endobj_pos > next_start) {
            endobj_pos = next_start;
        }

        size_t obj_kw_end = pdf_str.find("obj", start) + 3;
        std::string_view obj_body = pdf_str.substr(obj_kw_end, endobj_pos - obj_kw_end);

        PdfRawObject obj;
        obj.id = id;
        obj.gen = gen;

        size_t stream_pos = obj_body.find("stream");
        if (stream_pos != std::string_view::npos) {
            obj.has_stream = true;
            obj.header_dict = std::string(obj_body.substr(0, stream_pos));

            size_t stream_start = stream_pos + 6;
            if (stream_start < obj_body.size() && obj_body[stream_start] == '\r') stream_start++;
            if (stream_start < obj_body.size() && obj_body[stream_start] == '\n') stream_start++;

            size_t stream_end = obj_body.rfind("endstream");
            if (stream_end != std::string_view::npos && stream_end >= stream_start) {
                if (stream_end > stream_start && obj_body[stream_end - 1] == '\n') stream_end--;
                if (stream_end > stream_start && obj_body[stream_end - 1] == '\r') stream_end--;
                std::string_view raw_stream = obj_body.substr(stream_start, stream_end - stream_start);
                obj.stream_data.assign(raw_stream.begin(), raw_stream.end());
            }
        } else {
            obj.body = std::string(obj_body);
        }

        objects[id] = std::move(obj);
    }

    size_t trailer_pos = pdf_str.rfind("trailer");
    if (trailer_pos != std::string_view::npos) {
        size_t start_dict = pdf_str.find("<<", trailer_pos);
        size_t end_dict = pdf_str.find(">>", start_dict);
        if (start_dict != std::string_view::npos && end_dict != std::string_view::npos) {
            trailer_dict = std::string(pdf_str.substr(start_dict, end_dict + 2 - start_dict));
        }
    }

    return !objects.empty();
}

static void find_reachable_objects(int root_id, const std::unordered_map<int, PdfRawObject>& objects, std::unordered_set<int>& reachable) {
    if (reachable.find(root_id) != reachable.end()) return;
    auto it = objects.find(root_id);
    if (it == objects.end()) return;

    reachable.insert(root_id);

    std::string text_to_scan = it->second.has_stream ? it->second.header_dict : it->second.body;
    std::regex ref_regex(R"((\d+)\s+(\d+)\s+R)");
    auto words_begin = std::sregex_iterator(text_to_scan.begin(), text_to_scan.end(), ref_regex);
    auto words_end = std::sregex_iterator();

    for (auto rit = words_begin; rit != words_end; ++rit) {
        int ref_id = std::stoi((*rit)[1].str());
        find_reachable_objects(ref_id, objects, reachable);
    }
}

static bool inline_simple_indirect_objects(
    int root_id,
    std::unordered_map<int, PdfRawObject>& objects,
    std::unordered_set<int>& reachable,
    int64_t& inlined_bytes) {

    inlined_bytes = 0;
    std::unordered_map<int, int> ref_counts;
    std::unordered_map<int, int> single_referrer;

    std::regex ref_regex(R"((\d+)\s+(\d+)\s+R)");
    for (int pid : reachable) {
        auto it = objects.find(pid);
        if (it == objects.end()) continue;
        const std::string& text = it->second.has_stream ? it->second.header_dict : it->second.body;
        auto words_begin = std::sregex_iterator(text.begin(), text.end(), ref_regex);
        auto words_end = std::sregex_iterator();
        for (auto rit = words_begin; rit != words_end; ++rit) {
            int ref_id = std::stoi((*rit)[1].str());
            ref_counts[ref_id]++;
            single_referrer[ref_id] = pid;
        }
    }

    std::vector<int> candidates_to_inline;
    for (int id : reachable) {
        if (id == root_id) continue;
        if (ref_counts[id] != 1) continue;
        auto it = objects.find(id);
        if (it == objects.end()) continue;
        const auto& obj = it->second;
        if (obj.has_stream) continue;

        std::string_view b = obj.body;
        while (!b.empty() && std::isspace(static_cast<unsigned char>(b.front()))) b.remove_prefix(1);
        while (!b.empty() && std::isspace(static_cast<unsigned char>(b.back()))) b.remove_suffix(1);

        if (b.size() > 128) continue;
        if (b.find("/Type /Page") != std::string_view::npos || b.find("/Type/Page") != std::string_view::npos ||
            b.find("/Type /Pages") != std::string_view::npos || b.find("/Type/Pages") != std::string_view::npos ||
            b.find("/Type /Catalog") != std::string_view::npos) {
            continue;
        }
        if (b.find(" R") != std::string_view::npos) continue;

        candidates_to_inline.push_back(id);
    }

    bool any_inlined = false;
    for (int kid : candidates_to_inline) {
        int pid = single_referrer[kid];
        if (pid == kid) continue;
        auto parent_it = objects.find(pid);
        auto child_it = objects.find(kid);
        if (parent_it == objects.end() || child_it == objects.end()) continue;

        std::string child_body = child_it->second.body;
        while (!child_body.empty() && std::isspace(static_cast<unsigned char>(child_body.front()))) child_body.erase(child_body.begin());
        while (!child_body.empty() && std::isspace(static_cast<unsigned char>(child_body.back()))) child_body.pop_back();

        std::string pat = std::to_string(kid) + " 0 R";
        auto& p_text = parent_it->second.has_stream ? parent_it->second.header_dict : parent_it->second.body;
        size_t p_pos = p_text.find(pat);
        if (p_pos != std::string_view::npos) {
            p_text.replace(p_pos, pat.size(), child_body);
            reachable.erase(kid);
            objects.erase(kid);
            inlined_bytes += 30;
            any_inlined = true;
        }
    }

    return any_inlined;
}

static bool optimize_vector_content(std::string& s) {
    size_t orig_sz = s.size();

    const std::vector<std::pair<std::regex, std::string>> replacements = {
        { std::regex(R"rx(\bq\s+Q\b)rx"), "" },
        { std::regex(R"rx(\b1(?:\.0+)?\s+0(?:\.0+)?\s+0(?:\.0+)?\s+1(?:\.0+)?\s+0(?:\.0+)?\s+0(?:\.0+)?\s+cm\b)rx"), "" },
        { std::regex(R"rx(\bBT\s*ET\b)rx"), "" },
        { std::regex(R"rx((\b\d+(?:\.\d+)?\s+\d+(?:\.\d+)?\s+\d+(?:\.\d+)?\s+rg)\s+\1\b)rx"), "$1" },
        { std::regex(R"rx((\b\d+(?:\.\d+)?\s+\d+(?:\.\d+)?\s+\d+(?:\.\d+)?\s+RG)\s+\1\b)rx"), "$1" },
        { std::regex(R"rx((\b\d+(?:\.\d+)?\s+g)\s+\1\b)rx"), "$1" },
        { std::regex(R"rx((\b\d+(?:\.\d+)?\s+G)\s+\1\b)rx"), "$1" },
        { std::regex(R"rx((\b\d+(?:\.\d+)?\s+w)\s+\1\b)rx"), "$1" },
        { std::regex(R"rx(\(([^\r\n()]*)\)\s*Tj\s*\(([^\r\n()]*)\)\s*Tj)rx"), "($1$2) Tj" }
    };

    for (const auto& [rgx, rep] : replacements) {
        s = std::regex_replace(s, rgx, rep);
    }

    return s.size() < orig_sz;
}

bool PdfOptimizer::optimize(std::span<const uint8_t> input, std::vector<uint8_t>& output, OptimizationReport& report) {
    if (is_signed(input)) {
        report.message = "Signed PDF preserved";
        return false;
    }

    if (is_encrypted(input)) {
        report.message = "Encrypted PDF preserved";
        return false;
    }

    std::string_view pdf_str(reinterpret_cast<const char*>(input.data()), input.size());
    std::unordered_map<int, PdfRawObject> objects;
    std::string trailer_dict;

    if (!parse_pdf_objects(pdf_str, objects, trailer_dict)) {
        return false;
    }

    int root_id = -1;
    std::regex root_regex(R"(/Root\s+(\d+)\s+\d+\s+R)");
    std::smatch m;
    if (std::regex_search(trailer_dict, m, root_regex)) {
        root_id = std::stoi(m[1].str());
    }

    if (root_id == -1) {
        for (const auto& [id, obj] : objects) {
            if (obj.body.find("/Type /Catalog") != std::string_view::npos || obj.body.find("/Type/Catalog") != std::string_view::npos ||
                obj.header_dict.find("/Type /Catalog") != std::string_view::npos || obj.header_dict.find("/Type/Catalog") != std::string_view::npos) {
                root_id = id;
                break;
            }
        }
    }

    if (root_id == -1) return false;

    std::unordered_set<int> reachable;
    find_reachable_objects(root_id, objects, reachable);

    int64_t stream_savings = 0;
    for (auto& [id, obj] : objects) {
        if (!obj.has_stream || obj.stream_data.empty()) continue;

        if (obj.header_dict.find("/FlateDecode") != std::string::npos) {
            std::vector<uint8_t> uncompressed;
            if (DeflateEngine::zlib_decompress(obj.stream_data, uncompressed)) {
                bool is_cff = (obj.header_dict.find("/Type1C") != std::string::npos ||
                               obj.header_dict.find("/CIDFontType0C") != std::string::npos);
                if (is_cff) {
                    std::vector<uint8_t> opt_cff;
                    if (FontSubsetter::optimize_cff(uncompressed, opt_cff) && opt_cff.size() < uncompressed.size()) {
                        uncompressed = std::move(opt_cff);
                    }
                }

                bool is_zip = (uncompressed.size() >= 30 && uncompressed[0] == 'P' && uncompressed[1] == 'K' && uncompressed[2] == 0x03 && uncompressed[3] == 0x04);
                if (is_zip) {
                    ZipArchive sub_archive;
                    if (ZipArchive::read(uncompressed, sub_archive)) {
                        DocumentFormat sub_format = DocumentFormat::Unknown;
                        for (const auto& ename : sub_archive.entry_names()) {
                            if (ename.starts_with("xl/")) { sub_format = DocumentFormat::Xlsx; break; }
                            if (ename.starts_with("word/")) { sub_format = DocumentFormat::Docx; break; }
                            if (ename.starts_with("ppt/")) { sub_format = DocumentFormat::Pptx; break; }
                            if (ename == "mimetype") {
                                const auto* me = sub_archive.get_entry("mimetype");
                                if (me) {
                                    std::string_view msv(reinterpret_cast<const char*>(me->data.data()), me->data.size());
                                    if (msv.find("opendocument.text") != std::string_view::npos) sub_format = DocumentFormat::Odt;
                                    else if (msv.find("opendocument.spreadsheet") != std::string_view::npos) sub_format = DocumentFormat::Ods;
                                    else if (msv.find("opendocument.presentation") != std::string_view::npos) sub_format = DocumentFormat::Odp;
                                }
                            }
                        }
                        if (sub_format != DocumentFormat::Unknown) {
                            OptimizationReport sub_rep;
                            bool sub_ok = false;
                            if (FormatDetector::is_ooxml(sub_format)) {
                                sub_ok = OoxmlOptimizer::optimize(sub_archive, sub_format, sub_rep);
                            } else if (FormatDetector::is_odf(sub_format)) {
                                sub_ok = OdfOptimizer::optimize(sub_archive, sub_format, sub_rep);
                            }
                            if (sub_ok) {
                                std::vector<uint8_t> opt_sub;
                                if (sub_archive.write(opt_sub, true) && opt_sub.size() < uncompressed.size()) {
                                    uncompressed = std::move(opt_sub);
                                    if (obj.header_dict.find("/DL ") != std::string::npos || obj.header_dict.find("/DL/") != std::string::npos) {
                                        std::regex dl_regex(R"(/DL\s+\d+)");
                                        obj.header_dict = std::regex_replace(obj.header_dict, dl_regex, "/DL " + std::to_string(uncompressed.size()));
                                    }
                                    if (obj.header_dict.find("/Size ") != std::string::npos || obj.header_dict.find("/Size/") != std::string::npos) {
                                        std::regex sz_regex(R"(/Size\s+\d+)");
                                        obj.header_dict = std::regex_replace(obj.header_dict, sz_regex, "/Size " + std::to_string(uncompressed.size()));
                                    }
                                }
                            }
                        }
                    }
                }

                bool is_image = (obj.header_dict.find("/Subtype /Image") != std::string::npos ||
                                 obj.header_dict.find("/Subtype/Image") != std::string::npos);

                if (!is_cff && !is_image && !is_zip && obj.header_dict.find("/Font") == std::string::npos && obj.header_dict.find("/EmbeddedFile") == std::string::npos) {
                    std::string stream_str(reinterpret_cast<const char*>(uncompressed.data()), uncompressed.size());
                    if (optimize_vector_content(stream_str)) {
                        uncompressed.assign(stream_str.begin(), stream_str.end());
                    }
                }

                bool applied_predictor = false;
                std::vector<uint8_t> best_comp = DeflateEngine::zlib_compress_best(uncompressed);

                if (is_image && obj.header_dict.find("/DecodeParms") == std::string::npos) {
                    int width = 0, height = 0, bpc = 8, colors = 0;
                    std::smatch sm;
                    if (std::regex_search(obj.header_dict, sm, std::regex(R"(/Width\s+(\d+))"))) {
                        width = std::stoi(sm[1].str());
                    }
                    if (std::regex_search(obj.header_dict, sm, std::regex(R"(/Height\s+(\d+))"))) {
                        height = std::stoi(sm[1].str());
                    }
                    if (std::regex_search(obj.header_dict, sm, std::regex(R"(/BitsPerComponent\s+(\d+))"))) {
                        bpc = std::stoi(sm[1].str());
                    }
                    if (obj.header_dict.find("/DeviceRGB") != std::string::npos) {
                        colors = 3;
                    } else if (obj.header_dict.find("/DeviceGray") != std::string::npos) {
                        colors = 1;
                    } else if (obj.header_dict.find("/DeviceCMYK") != std::string::npos) {
                        colors = 4;
                    }

                    if (width > 0 && height > 0 && bpc == 8 && colors > 0) {
                        size_t stride = static_cast<size_t>(width) * colors;
                        if (uncompressed.size() == static_cast<size_t>(height) * stride) {
                            std::vector<uint8_t> predicted(static_cast<size_t>(height) * (1 + stride));
                            size_t bpp = colors;

                            for (size_t y = 0; y < static_cast<size_t>(height); ++y) {
                                const uint8_t* curr = &uncompressed[y * stride];
                                const uint8_t* prev = (y > 0) ? &uncompressed[(y - 1) * stride] : nullptr;

                                uint8_t cand_lines[5][2048];
                                uint32_t cand_scores[5] = {0, 0, 0, 0, 0};

                                if (stride <= sizeof(cand_lines[0])) {
                                    for (size_t x = 0; x < stride; ++x) {
                                        int raw = curr[x];
                                        int a = (x >= bpp) ? curr[x - bpp] : 0;
                                        int b = prev ? prev[x] : 0;
                                        int c = (prev && x >= bpp) ? prev[x - bpp] : 0;

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

                                    predicted[y * (1 + stride)] = best_f;
                                    std::memcpy(&predicted[y * (1 + stride) + 1], cand_lines[best_f], stride);
                                } else {
                                    std::vector<std::vector<uint8_t>> cand(5, std::vector<uint8_t>(stride));
                                    for (size_t x = 0; x < stride; ++x) {
                                        int raw = curr[x];
                                        int a = (x >= bpp) ? curr[x - bpp] : 0;
                                        int b = prev ? prev[x] : 0;
                                        int c = (prev && x >= bpp) ? prev[x - bpp] : 0;

                                        cand[0][x] = static_cast<uint8_t>(raw);
                                        cand[1][x] = static_cast<uint8_t>(raw - a);
                                        cand[2][x] = static_cast<uint8_t>(raw - b);
                                        cand[3][x] = static_cast<uint8_t>(raw - (a + b) / 2);
                                        cand[4][x] = static_cast<uint8_t>(raw - paeth(a, b, c));

                                        cand_scores[0] += std::abs(static_cast<int8_t>(cand[0][x]));
                                        cand_scores[1] += std::abs(static_cast<int8_t>(cand[1][x]));
                                        cand_scores[2] += std::abs(static_cast<int8_t>(cand[2][x]));
                                        cand_scores[3] += std::abs(static_cast<int8_t>(cand[3][x]));
                                        cand_scores[4] += std::abs(static_cast<int8_t>(cand[4][x]));
                                    }

                                    uint8_t best_f = 0;
                                    uint32_t best_s = cand_scores[0];
                                    for (uint8_t f = 1; f < 5; ++f) {
                                        if (cand_scores[f] < best_s) {
                                            best_s = cand_scores[f];
                                            best_f = f;
                                        }
                                    }

                                    predicted[y * (1 + stride)] = best_f;
                                    std::memcpy(&predicted[y * (1 + stride) + 1], cand[best_f].data(), stride);
                                }
                            }

                            auto pred_comp = DeflateEngine::zlib_compress_best(predicted);
                            if (pred_comp.size() < best_comp.size()) {
                                best_comp = std::move(pred_comp);
                                applied_predictor = true;
                            }
                        }
                    }
                }

                if (best_comp.size() < obj.stream_data.size()) {
                    stream_savings += static_cast<int64_t>(obj.stream_data.size() - best_comp.size());
                    obj.stream_data = std::move(best_comp);

                    if (applied_predictor) {
                        int width = 0, colors = 0;
                        std::smatch sm;
                        if (std::regex_search(obj.header_dict, sm, std::regex(R"(/Width\s+(\d+))"))) {
                            width = std::stoi(sm[1].str());
                        }
                        if (obj.header_dict.find("/DeviceRGB") != std::string::npos) colors = 3;
                        else if (obj.header_dict.find("/DeviceGray") != std::string::npos) colors = 1;
                        else if (obj.header_dict.find("/DeviceCMYK") != std::string::npos) colors = 4;

                        std::string decode_parms = "/DecodeParms <</Predictor 15 /Columns " + std::to_string(width) +
                                                   " /Colors " + std::to_string(colors) + " /BitsPerComponent 8>> ";
                        size_t filter_pos = obj.header_dict.find("/FlateDecode");
                        if (filter_pos != std::string::npos) {
                            obj.header_dict.insert(filter_pos + 12, " " + decode_parms);
                        }
                    }

                    std::regex len_regex(R"(/Length\s+(\d+\s+\d+\s+R|\d+))");
                    std::string new_len = "/Length " + std::to_string(obj.stream_data.size());
                    obj.header_dict = std::regex_replace(obj.header_dict, len_regex, new_len);
                }
            }
        } else if (obj.header_dict.find("/Filter") == std::string::npos && obj.stream_data.size() > 64) {
            auto comp = DeflateEngine::zlib_compress_best(obj.stream_data);
            if (comp.size() + 25 < obj.stream_data.size()) {
                stream_savings += static_cast<int64_t>(obj.stream_data.size() - comp.size());
                obj.stream_data = std::move(comp);
                std::regex len_regex(R"(/Length\s+(\d+\s+\d+\s+R|\d+))");
                std::string new_len = "/Filter /FlateDecode /Length " + std::to_string(obj.stream_data.size());
                obj.header_dict = std::regex_replace(obj.header_dict, len_regex, new_len);
            }
        }
    }

    if (stream_savings > 0) {
        report.savings_by_pass["Stream Recompression"] += stream_savings;
    }

    int64_t inlined_savings = 0;
    if (inline_simple_indirect_objects(root_id, objects, reachable, inlined_savings)) {
        report.savings_by_pass["Object Inlining"] += inlined_savings;
    }

    std::vector<int> sorted_ids;
    for (int id : reachable) {
        sorted_ids.push_back(id);
    }
    std::sort(sorted_ids.begin(), sorted_ids.end());

    std::vector<uint8_t> candidate;
    candidate.reserve(input.size());

    const char* header = "%PDF-1.7\n%\x80\x81\x82\x83\n";
    candidate.insert(candidate.end(), header, header + std::strlen(header));

    std::vector<size_t> xref_offsets;
    int max_id = sorted_ids.empty() ? 0 : sorted_ids.back();
    xref_offsets.resize(max_id + 1, 0);

    for (int id : sorted_ids) {
        auto it = objects.find(id);
        if (it == objects.end()) continue;
        const auto& obj = it->second;

        xref_offsets[id] = candidate.size();

        std::string obj_head = std::to_string(id) + " " + std::to_string(obj.gen) + " obj\n";
        candidate.insert(candidate.end(), obj_head.begin(), obj_head.end());

        if (obj.has_stream) {
            std::string hdict = obj.header_dict;
            if (!hdict.empty() && hdict.back() != '\n') hdict.push_back('\n');
            candidate.insert(candidate.end(), hdict.begin(), hdict.end());

            const char* st_kw = "stream\n";
            candidate.insert(candidate.end(), st_kw, st_kw + 7);
            candidate.insert(candidate.end(), obj.stream_data.begin(), obj.stream_data.end());
            const char* end_st = "\nendstream\nendobj\n";
            candidate.insert(candidate.end(), end_st, end_st + 18);
        } else {
            std::string b = obj.body;
            while (!b.empty() && std::isspace(static_cast<unsigned char>(b.front()))) b.erase(b.begin());
            while (!b.empty() && std::isspace(static_cast<unsigned char>(b.back()))) b.pop_back();
            candidate.insert(candidate.end(), b.begin(), b.end());
            const char* end_obj = "\nendobj\n";
            candidate.insert(candidate.end(), end_obj, end_obj + 8);
        }
    }

    std::vector<uint8_t> candidate_ascii = candidate;
    size_t xref_start = candidate_ascii.size();
    std::string xref_table = "xref\n0 " + std::to_string(max_id + 1) + "\n";
    char line_buf[32];
    std::snprintf(line_buf, sizeof(line_buf), "0000000000 65535 f \n");
    xref_table.append(line_buf);

    for (int i = 1; i <= max_id; ++i) {
        if (xref_offsets[i] > 0) {
            std::snprintf(line_buf, sizeof(line_buf), "%010zu %05d n \n", xref_offsets[i], 0);
        } else {
            std::snprintf(line_buf, sizeof(line_buf), "0000000000 65535 f \n");
        }
        xref_table.append(line_buf);
    }

    candidate_ascii.insert(candidate_ascii.end(), xref_table.begin(), xref_table.end());

    std::string trailer_str = "trailer\n<<\n/Size " + std::to_string(max_id + 1) + "\n/Root " + std::to_string(root_id) + " 0 R\n>>\n";
    candidate_ascii.insert(candidate_ascii.end(), trailer_str.begin(), trailer_str.end());

    std::string eof_str = "startxref\n" + std::to_string(xref_start) + "\n%%EOF\n";
    candidate_ascii.insert(candidate_ascii.end(), eof_str.begin(), eof_str.end());

    std::vector<uint8_t> candidate_xref_stm = candidate;
    int xref_obj_id = max_id + 1;
    size_t xref_stm_offset = candidate_xref_stm.size();

    size_t total_xref_entries = xref_obj_id + 1;
    std::vector<uint8_t> raw_xref_bytes(total_xref_entries * 7);

    for (size_t i = 0; i < total_xref_entries; ++i) {
        size_t off = (i <= static_cast<size_t>(max_id)) ? xref_offsets[i] : 0;
        uint8_t type = 0;
        uint32_t f2 = 0;
        uint16_t f3 = 0;

        if (i == 0) {
            type = 0;
            f2 = 0;
            f3 = 65535;
        } else if (i == static_cast<size_t>(xref_obj_id)) {
            type = 1;
            f2 = static_cast<uint32_t>(xref_stm_offset);
            f3 = 0;
        } else if (off > 0) {
            type = 1;
            f2 = static_cast<uint32_t>(off);
            f3 = 0;
        } else {
            type = 0;
            f2 = 0;
            f3 = 65535;
        }

        size_t base = i * 7;
        raw_xref_bytes[base] = type;
        raw_xref_bytes[base + 1] = static_cast<uint8_t>((f2 >> 24) & 0xFF);
        raw_xref_bytes[base + 2] = static_cast<uint8_t>((f2 >> 16) & 0xFF);
        raw_xref_bytes[base + 3] = static_cast<uint8_t>((f2 >> 8) & 0xFF);
        raw_xref_bytes[base + 4] = static_cast<uint8_t>(f2 & 0xFF);
        raw_xref_bytes[base + 5] = static_cast<uint8_t>((f3 >> 8) & 0xFF);
        raw_xref_bytes[base + 6] = static_cast<uint8_t>(f3 & 0xFF);
    }

    auto comp_xref_stream = DeflateEngine::zlib_compress_best(raw_xref_bytes);

    std::string xhead = std::to_string(xref_obj_id) + " 0 obj\n<<\n/Type /XRef\n/Size " +
                        std::to_string(total_xref_entries) + "\n/Root " + std::to_string(root_id) +
                        " 0 R\n/W [1 4 2]\n/Filter /FlateDecode\n/Length " +
                        std::to_string(comp_xref_stream.size()) + "\n>>\nstream\n";
    candidate_xref_stm.insert(candidate_xref_stm.end(), xhead.begin(), xhead.end());
    candidate_xref_stm.insert(candidate_xref_stm.end(), comp_xref_stream.begin(), comp_xref_stream.end());
    std::string xtail = "\nendstream\nendobj\nstartxref\n" + std::to_string(xref_stm_offset) + "\n%%EOF\n";
    candidate_xref_stm.insert(candidate_xref_stm.end(), xtail.begin(), xtail.end());

    std::vector<int> stream_ids;
    std::vector<int> obj_stm_ids;
    for (int id : sorted_ids) {
        if (objects[id].has_stream) {
            stream_ids.push_back(id);
        } else {
            obj_stm_ids.push_back(id);
        }
    }

    std::vector<uint8_t> candidate_obj_stm;
    if (!obj_stm_ids.empty()) {
        std::string objects_body;
        std::vector<size_t> obj_offsets(obj_stm_ids.size());
        for (size_t k = 0; k < obj_stm_ids.size(); ++k) {
            int oid = obj_stm_ids[k];
            const auto& obj = objects[oid];
            std::string b = obj.body;
            while (!b.empty() && std::isspace(static_cast<unsigned char>(b.front()))) b.erase(b.begin());
            while (!b.empty() && std::isspace(static_cast<unsigned char>(b.back()))) b.pop_back();

            obj_offsets[k] = objects_body.size();
            objects_body.append(b);
            objects_body.push_back('\n');
        }

        std::string index_header;
        for (size_t k = 0; k < obj_stm_ids.size(); ++k) {
            index_header += std::to_string(obj_stm_ids[k]) + " " + std::to_string(obj_offsets[k]);
            if (k + 1 < obj_stm_ids.size()) index_header += " ";
        }
        index_header += "\n";

        size_t first_offset = index_header.size();
        std::string uncomp_obj_stm = index_header + objects_body;
        auto comp_obj_stm = DeflateEngine::zlib_compress_best(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(uncomp_obj_stm.data()), uncomp_obj_stm.size())
        );

        int obj_stm_id = max_id + 1;
        int xref_obj_id_2 = max_id + 2;

        const char* pdf_hdr = "%PDF-1.7\n%\x80\x81\x82\x83\n";
        candidate_obj_stm.insert(candidate_obj_stm.end(), pdf_hdr, pdf_hdr + std::strlen(pdf_hdr));

        std::vector<size_t> ostm_xref_offsets(xref_obj_id_2 + 1, 0);

        for (int id : stream_ids) {
            const auto& obj = objects[id];
            ostm_xref_offsets[id] = candidate_obj_stm.size();

            std::string obj_head = std::to_string(id) + " 0 obj\n";
            candidate_obj_stm.insert(candidate_obj_stm.end(), obj_head.begin(), obj_head.end());

            std::string hdict = obj.header_dict;
            if (!hdict.empty() && hdict.back() != '\n') hdict.push_back('\n');
            candidate_obj_stm.insert(candidate_obj_stm.end(), hdict.begin(), hdict.end());

            const char* st_kw = "stream\n";
            candidate_obj_stm.insert(candidate_obj_stm.end(), st_kw, st_kw + 7);
            candidate_obj_stm.insert(candidate_obj_stm.end(), obj.stream_data.begin(), obj.stream_data.end());
            const char* end_st = "\nendstream\nendobj\n";
            candidate_obj_stm.insert(candidate_obj_stm.end(), end_st, end_st + 18);
        }

        ostm_xref_offsets[obj_stm_id] = candidate_obj_stm.size();
        std::string ostm_head = std::to_string(obj_stm_id) + " 0 obj\n<<\n/Type /ObjStm\n/N " +
                                std::to_string(obj_stm_ids.size()) + "\n/First " +
                                std::to_string(first_offset) + "\n/Filter /FlateDecode\n/Length " +
                                std::to_string(comp_obj_stm.size()) + "\n>>\nstream\n";
        candidate_obj_stm.insert(candidate_obj_stm.end(), ostm_head.begin(), ostm_head.end());
        candidate_obj_stm.insert(candidate_obj_stm.end(), comp_obj_stm.begin(), comp_obj_stm.end());
        const char* end_st = "\nendstream\nendobj\n";
        candidate_obj_stm.insert(candidate_obj_stm.end(), end_st, end_st + 18);

        size_t xref2_stm_offset = candidate_obj_stm.size();
        size_t total_xref2_entries = xref_obj_id_2 + 1;
        std::vector<uint8_t> raw_xref2_bytes(total_xref2_entries * 7);

        std::unordered_map<int, size_t> obj_stm_index_map;
        for (size_t k = 0; k < obj_stm_ids.size(); ++k) {
            obj_stm_index_map[obj_stm_ids[k]] = k;
        }

        for (size_t i = 0; i < total_xref2_entries; ++i) {
            uint8_t type = 0;
            uint32_t f2 = 0;
            uint16_t f3 = 0;

            if (i == 0) {
                type = 0;
                f2 = 0;
                f3 = 65535;
            } else if (i == static_cast<size_t>(xref_obj_id_2)) {
                type = 1;
                f2 = static_cast<uint32_t>(xref2_stm_offset);
                f3 = 0;
            } else if (i == static_cast<size_t>(obj_stm_id)) {
                type = 1;
                f2 = static_cast<uint32_t>(ostm_xref_offsets[obj_stm_id]);
                f3 = 0;
            } else if (obj_stm_index_map.contains(static_cast<int>(i))) {
                type = 2;
                f2 = static_cast<uint32_t>(obj_stm_id);
                f3 = static_cast<uint16_t>(obj_stm_index_map[static_cast<int>(i)]);
            } else if (ostm_xref_offsets[i] > 0) {
                type = 1;
                f2 = static_cast<uint32_t>(ostm_xref_offsets[i]);
                f3 = 0;
            } else {
                type = 0;
                f2 = 0;
                f3 = 65535;
            }

            size_t base = i * 7;
            raw_xref2_bytes[base] = type;
            raw_xref2_bytes[base + 1] = static_cast<uint8_t>((f2 >> 24) & 0xFF);
            raw_xref2_bytes[base + 2] = static_cast<uint8_t>((f2 >> 16) & 0xFF);
            raw_xref2_bytes[base + 3] = static_cast<uint8_t>((f2 >> 8) & 0xFF);
            raw_xref2_bytes[base + 4] = static_cast<uint8_t>(f2 & 0xFF);
            raw_xref2_bytes[base + 5] = static_cast<uint8_t>((f3 >> 8) & 0xFF);
            raw_xref2_bytes[base + 6] = static_cast<uint8_t>(f3 & 0xFF);
        }

        auto comp_xref2_stream = DeflateEngine::zlib_compress_best(raw_xref2_bytes);

        std::string xhead2 = std::to_string(xref_obj_id_2) + " 0 obj\n<<\n/Type /XRef\n/Size " +
                             std::to_string(total_xref2_entries) + "\n/Root " + std::to_string(root_id) +
                             " 0 R\n/W [1 4 2]\n/Filter /FlateDecode\n/Length " +
                             std::to_string(comp_xref2_stream.size()) + "\n>>\nstream\n";
        candidate_obj_stm.insert(candidate_obj_stm.end(), xhead2.begin(), xhead2.end());
        candidate_obj_stm.insert(candidate_obj_stm.end(), comp_xref2_stream.begin(), comp_xref2_stream.end());
        std::string xtail2 = "\nendstream\nendobj\nstartxref\n" + std::to_string(xref2_stm_offset) + "\n%%EOF\n";
        candidate_obj_stm.insert(candidate_obj_stm.end(), xtail2.begin(), xtail2.end());
    }

    if (!candidate_obj_stm.empty() &&
        candidate_obj_stm.size() < candidate_xref_stm.size() &&
        candidate_obj_stm.size() < input.size() &&
        Validator::validate_pdf(candidate_obj_stm, input)) {
        output = std::move(candidate_obj_stm);
        return true;
    }

    if (candidate_xref_stm.size() < candidate_ascii.size() &&
        candidate_xref_stm.size() < input.size() &&
        Validator::validate_pdf(candidate_xref_stm, input)) {
        output = std::move(candidate_xref_stm);
        return true;
    }

    if (candidate_ascii.size() < input.size() && Validator::validate_pdf(candidate_ascii, input)) {
        output = std::move(candidate_ascii);
        return true;
    }

    return false;
}

}
