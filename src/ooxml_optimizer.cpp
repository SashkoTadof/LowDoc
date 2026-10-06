#include "lowdoc/ooxml_optimizer.hpp"
#include "lowdoc/xml_optimizer.hpp"
#include "lowdoc/image_optimizer.hpp"
#include "lowdoc/resource_deduplicator.hpp"
#include "lowdoc/font_subsetter.hpp"
#include "lowdoc/deflate.hpp"
#include <algorithm>
#include <regex>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>

namespace lowdoc {

bool OoxmlOptimizer::remove_thumbnails(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    std::vector<std::string> to_remove;
    for (const auto& entry : archive.entries()) {
        if (entry.name.starts_with("docProps/thumbnail.") || entry.name == "docProps/thumbnail.jpeg") {
            to_remove.push_back(entry.name);
            saved_bytes += static_cast<int64_t>(entry.data.size());
        }
    }

    if (to_remove.empty()) return false;

    for (const auto& name : to_remove) {
        archive.remove(name);
    }

    auto rels_entry = archive.get_entry_mut("_rels/.rels");
    if (rels_entry) {
        std::string rels_str(reinterpret_cast<const char*>(rels_entry->data.data()), rels_entry->data.size());
        std::regex thumb_rel_regex(R"rx(<Relationship[^>]*Type="[^"]*metadata/thumbnail"[^>]*/>)rx");
        std::string cleaned_rels = std::regex_replace(rels_str, thumb_rel_regex, "");
        rels_entry->data.assign(cleaned_rels.begin(), cleaned_rels.end());
    }

    auto ct_entry = archive.get_entry_mut("[Content_Types].xml");
    if (ct_entry) {
        std::string ct_str(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
        std::regex thumb_ct_regex(R"rx(<Override[^>]*PartName="/docProps/thumbnail\.[^"]*"[^>]*/>)rx");
        std::string cleaned_ct = std::regex_replace(ct_str, thumb_ct_regex, "");
        ct_entry->data.assign(cleaned_ct.begin(), cleaned_ct.end());
    }

    return true;
}

bool OoxmlOptimizer::clean_metadata(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    auto core_entry = archive.get_entry_mut("docProps/core.xml");
    if (core_entry) {
        size_t orig_sz = core_entry->data.size();
        std::string core_str(reinterpret_cast<const char*>(core_entry->data.data()), core_entry->data.size());

        std::vector<std::regex> strip_tags = {
            std::regex(R"rx(<dc:creator>[^<]*</dc:creator>)rx"),
            std::regex(R"rx(<cp:lastModifiedBy>[^<]*</cp:lastModifiedBy>)rx"),
            std::regex(R"rx(<cp:revision>[^<]*</cp:revision>)rx"),
            std::regex(R"rx(<dcterms:created[^>]*>[^<]*</dcterms:created>)rx"),
            std::regex(R"rx(<dcterms:modified[^>]*>[^<]*</dcterms:modified>)rx"),
            std::regex(R"rx(<cp:category>[^<]*</cp:category>)rx"),
            std::regex(R"rx(<dc:description>[^<]*</dc:description>)rx")
        };

        for (const auto& r : strip_tags) {
            core_str = std::regex_replace(core_str, r, "");
        }

        if (XmlOptimizer::is_well_formed(core_str)) {
            core_entry->data.assign(core_str.begin(), core_str.end());
            if (core_entry->data.size() < orig_sz) {
                saved_bytes += static_cast<int64_t>(orig_sz - core_entry->data.size());
            }
        }
    }

    auto app_entry = archive.get_entry_mut("docProps/app.xml");
    if (app_entry) {
        size_t orig_sz = app_entry->data.size();
        std::string app_str(reinterpret_cast<const char*>(app_entry->data.data()), app_entry->data.size());

        std::vector<std::regex> strip_tags = {
            std::regex(R"rx(<TotalTime>[^<]*</TotalTime>)rx"),
            std::regex(R"rx(<Application>[^<]*</Application>)rx"),
            std::regex(R"rx(<AppVersion>[^<]*</AppVersion>)rx"),
            std::regex(R"rx(<Company>[^<]*</Company>)rx"),
            std::regex(R"rx(<Manager>[^<]*</Manager>)rx")
        };

        for (const auto& r : strip_tags) {
            app_str = std::regex_replace(app_str, r, "");
        }

        if (XmlOptimizer::is_well_formed(app_str)) {
            app_entry->data.assign(app_str.begin(), app_str.end());
            if (app_entry->data.size() < orig_sz) {
                saved_bytes += static_cast<int64_t>(orig_sz - app_entry->data.size());
            }
        }
    }

    auto* custom_entry = archive.get_entry("docProps/custom.xml");
    if (custom_entry) {
        std::string_view custom_sv(reinterpret_cast<const char*>(custom_entry->data.data()), custom_entry->data.size());
        if (custom_sv.find("<property") == std::string_view::npos) {
            saved_bytes += static_cast<int64_t>(custom_entry->data.size());
            archive.remove("docProps/custom.xml");

            auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
            if (ct_entry) {
                std::string ct(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
                std::regex ct_r(R"rx(<Override\b[^>]*PartName="/docProps/custom\.xml"[^>]*/>)rx");
                ct = std::regex_replace(ct, ct_r, "");
                if (XmlOptimizer::is_well_formed(ct)) {
                    ct_entry->data.assign(ct.begin(), ct.end());
                }
            }

            auto* rels_entry = archive.get_entry_mut("_rels/.rels");
            if (rels_entry) {
                std::string rels(reinterpret_cast<const char*>(rels_entry->data.data()), rels_entry->data.size());
                std::regex rel_r(R"rx(<Relationship\b[^>]*Target="docProps/custom\.xml"[^>]*/>)rx");
                rels = std::regex_replace(rels, rel_r, "");
                if (XmlOptimizer::is_well_formed(rels)) {
                    rels_entry->data.assign(rels.begin(), rels.end());
                }
            }
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::strip_rsids(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    const std::vector<std::string> rsid_attrs = {
        "w:rsidR",
        "w:rsidRDefault",
        "w:rsidP",
        "w:rsidRPr",
        "w:rsidTr",
        "w:rsidDel"
    };

    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".xml")) continue;
        if (!name.starts_with("word/")) continue;

        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        size_t orig_sz = entry->data.size();
        std::string_view sv(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());

        std::string stripped = XmlOptimizer::remove_attributes_matching(sv, rsid_attrs);
        if (name == "word/settings.xml") {
            std::regex rsids_block(R"rx(<w:rsids>.*?</w:rsids>)rx");
            stripped = std::regex_replace(stripped, rsids_block, "");
        }

        if (stripped.size() < orig_sz && XmlOptimizer::is_well_formed(stripped)) {
            saved_bytes += static_cast<int64_t>(orig_sz - stripped.size());
            entry->data.assign(stripped.begin(), stripped.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::prune_unused_styles(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* main_styles_entry = archive.get_entry_mut("word/styles.xml");
    if (!main_styles_entry) return false;

    std::unordered_set<std::string> referenced_styles = {
        "Normal", "DefaultParagraphFont", "TableNormal", "NoList"
    };

    std::regex style_ref_regex(R"rx(<w:[^>]*Style\b[^>]*\bw:val="([^"]+)")rx");
    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        if (name == "word/styles.xml" || name == "word/stylesWithEffects.xml") continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string content(reinterpret_cast<const char*>(e->data.data()), e->data.size());
        auto words_begin = std::sregex_iterator(content.begin(), content.end(), style_ref_regex);
        auto words_end = std::sregex_iterator();
        for (auto it = words_begin; it != words_end; ++it) {
            referenced_styles.insert((*it)[1].str());
        }
    }

    struct StyleMeta {
        std::string id;
        bool is_default = false;
        std::string based_on;
        std::string next_style;
        std::string link_style;
        size_t start = 0;
        size_t length = 0;
    };

    auto parse_and_prune = [&](const std::string& entry_name) -> bool {
        auto* entry = archive.get_entry_mut(entry_name);
        if (!entry) return false;

        size_t orig_sz = entry->data.size();
        std::string xml(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());

        size_t ls_start = xml.find("<w:latentStyles");
        if (ls_start != std::string::npos) {
            size_t ls_end = xml.find("</w:latentStyles>", ls_start);
            if (ls_end != std::string::npos) {
                xml.erase(ls_start, (ls_end + 17) - ls_start);
            } else {
                size_t ls_close = xml.find('>', ls_start);
                if (ls_close != std::string::npos && xml[ls_close - 1] == '/') {
                    xml.erase(ls_start, (ls_close + 1) - ls_start);
                }
            }
        }

        std::vector<StyleMeta> styles;
        size_t pos = 0;
        while (pos < xml.size()) {
            size_t tag_start = xml.find("<w:style", pos);
            if (tag_start == std::string::npos) break;
            if (tag_start + 8 < xml.size()) {
                char c = xml[tag_start + 8];
                if (c != ' ' && c != '>' && c != '\r' && c != '\n' && c != '\t') {
                    pos = tag_start + 8;
                    continue;
                }
            }
            size_t tag_close = xml.find('>', tag_start);
            if (tag_close == std::string::npos) break;
            size_t elem_end = std::string::npos;
            if (xml[tag_close - 1] == '/') {
                elem_end = tag_close + 1;
            } else {
                size_t end_tag = xml.find("</w:style>", tag_close);
                if (end_tag == std::string::npos) break;
                elem_end = end_tag + 10;
            }

            std::string_view elem(xml.data() + tag_start, elem_end - tag_start);
            StyleMeta meta;
            meta.start = tag_start;
            meta.length = elem_end - tag_start;

            size_t id_pos = elem.find("w:styleId=\"");
            if (id_pos != std::string_view::npos) {
                size_t v_start = id_pos + 11;
                size_t v_end = elem.find('"', v_start);
                if (v_end != std::string_view::npos) {
                    meta.id = std::string(elem.substr(v_start, v_end - v_start));
                }
            }

            meta.is_default = (elem.find("w:default=\"1\"") != std::string_view::npos ||
                               elem.find("w:default=\"true\"") != std::string_view::npos);

            size_t bo = elem.find("<w:basedOn");
            if (bo != std::string_view::npos) {
                size_t bo_val = elem.find("w:val=\"", bo);
                if (bo_val != std::string_view::npos) {
                    size_t v_start = bo_val + 7;
                    size_t v_end = elem.find('"', v_start);
                    if (v_end != std::string_view::npos) {
                        meta.based_on = std::string(elem.substr(v_start, v_end - v_start));
                    }
                }
            }

            size_t nx = elem.find("<w:next");
            if (nx != std::string_view::npos) {
                size_t nx_val = elem.find("w:val=\"", nx);
                if (nx_val != std::string_view::npos) {
                    size_t v_start = nx_val + 7;
                    size_t v_end = elem.find('"', v_start);
                    if (v_end != std::string_view::npos) {
                        meta.next_style = std::string(elem.substr(v_start, v_end - v_start));
                    }
                }
            }

            size_t lk = elem.find("<w:link");
            if (lk != std::string_view::npos) {
                size_t lk_val = elem.find("w:val=\"", lk);
                if (lk_val != std::string_view::npos) {
                    size_t v_start = lk_val + 7;
                    size_t v_end = elem.find('"', v_start);
                    if (v_end != std::string_view::npos) {
                        meta.link_style = std::string(elem.substr(v_start, v_end - v_start));
                    }
                }
            }

            if (meta.is_default && !meta.id.empty()) {
                referenced_styles.insert(meta.id);
            }

            styles.push_back(std::move(meta));
            pos = elem_end;
        }

        bool added_new = true;
        while (added_new) {
            added_new = false;
            for (const auto& s : styles) {
                if (referenced_styles.contains(s.id)) {
                    if (!s.based_on.empty() && !referenced_styles.contains(s.based_on)) {
                        referenced_styles.insert(s.based_on);
                        added_new = true;
                    }
                    if (!s.next_style.empty() && !referenced_styles.contains(s.next_style)) {
                        referenced_styles.insert(s.next_style);
                        added_new = true;
                    }
                    if (!s.link_style.empty() && !referenced_styles.contains(s.link_style)) {
                        referenced_styles.insert(s.link_style);
                        added_new = true;
                    }
                }
            }
        }

        std::string rebuilt;
        rebuilt.reserve(xml.size());
        size_t last_idx = 0;
        for (const auto& s : styles) {
            rebuilt.append(xml.substr(last_idx, s.start - last_idx));
            last_idx = s.start + s.length;
            if (s.is_default || referenced_styles.contains(s.id)) {
                rebuilt.append(xml.substr(s.start, s.length));
            }
        }
        rebuilt.append(xml.substr(last_idx));

        if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
            saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
            entry->data.assign(rebuilt.begin(), rebuilt.end());
            return true;
        }
        return false;
    };

    bool res1 = parse_and_prune("word/styles.xml");
    bool res2 = parse_and_prune("word/stylesWithEffects.xml");
    return res1 || res2;
}

bool OoxmlOptimizer::deduplicate_media(ZipArchive& archive, int64_t& saved_bytes) {
    return ResourceDeduplicator::deduplicate_archive_media(archive, DocumentFormat::Docx, saved_bytes);
}

bool OoxmlOptimizer::optimize_embedded_images(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    for (const auto& name : archive.entry_names()) {
        if (name.find("/media/") == std::string::npos) continue;

        std::string ext = std::filesystem::path(name).extension().string();
        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        size_t orig_sz = entry->data.size();
        std::vector<uint8_t> opt_img;
        if (ImageOptimizer::optimize_image(entry->data, ext, opt_img)) {
            if (opt_img.size() < orig_sz) {
                saved_bytes += static_cast<int64_t>(orig_sz - opt_img.size());
                entry->data = std::move(opt_img);
            }
        }
    }
    return saved_bytes > 0;
}

bool OoxmlOptimizer::minify_all_xml(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".xml") && !name.ends_with(".rels")) continue;

        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        size_t orig_sz = entry->data.size();
        std::string cur(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());

        if (name.ends_with(".xml")) {
            std::string consolidated = XmlOptimizer::consolidate_text_runs(cur);
            if (consolidated.size() <= cur.size() && XmlOptimizer::is_well_formed(consolidated)) {
                cur = std::move(consolidated);
            }
            std::string dedup_ns = XmlOptimizer::deduplicate_namespaces(cur);
            if (dedup_ns.size() <= cur.size() && XmlOptimizer::is_well_formed(dedup_ns)) {
                cur = std::move(dedup_ns);
            }
        }

        std::string minified = XmlOptimizer::minify(cur);
        if (minified.size() <= cur.size() && XmlOptimizer::is_well_formed(minified)) {
            cur = std::move(minified);
        }

        if (cur.size() < orig_sz && XmlOptimizer::is_well_formed(cur)) {
            saved_bytes += static_cast<int64_t>(orig_sz - cur.size());
            entry->data.assign(cur.begin(), cur.end());
        }
    }
    return saved_bytes > 0;
}

bool OoxmlOptimizer::subset_embedded_fonts(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::vector<std::string> font_entries;
    for (const auto& name : archive.entry_names()) {
        if (name.find("/fonts/") != std::string::npos || name.ends_with(".odttf") || name.ends_with(".ttf")) {
            font_entries.push_back(name);
        }
    }
    if (font_entries.empty()) return false;

    std::unordered_set<uint32_t> used_codepoints;
    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".xml")) continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string_view content(reinterpret_cast<const char*>(e->data.data()), e->data.size());

        size_t pos = 0;
        while (pos < content.size()) {
            size_t t_open = content.find("<w:t", pos);
            if (t_open == std::string_view::npos) {
                t_open = content.find("<a:t", pos);
            }
            if (t_open == std::string_view::npos) break;

            size_t tag_end = content.find('>', t_open);
            if (tag_end == std::string_view::npos) break;

            size_t t_close = content.find("</", tag_end);
            if (t_close == std::string_view::npos) break;

            std::string_view text = content.substr(tag_end + 1, t_close - tag_end - 1);
            size_t idx = 0;
            while (idx < text.size()) {
                uint8_t c = static_cast<uint8_t>(text[idx]);
                uint32_t cp = 0;
                if ((c & 0x80) == 0) {
                    cp = c;
                    idx += 1;
                } else if ((c & 0xE0) == 0xC0 && idx + 1 < text.size()) {
                    cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(text[idx + 1]) & 0x3F);
                    idx += 2;
                } else if ((c & 0xF0) == 0xE0 && idx + 2 < text.size()) {
                    cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(text[idx + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(text[idx + 2]) & 0x3F);
                    idx += 3;
                } else if ((c & 0xF8) == 0xF0 && idx + 3 < text.size()) {
                    cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(text[idx + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(text[idx + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(text[idx + 3]) & 0x3F);
                    idx += 4;
                } else {
                    idx += 1;
                }
                if (cp > 0) used_codepoints.insert(cp);
            }
            pos = t_close + 2;
        }
    }

    if (used_codepoints.empty()) return false;

    std::unordered_map<std::string, std::string> font_guid_map;
    const auto* font_table = archive.get_entry("word/fontTable.xml");
    if (font_table) {
        std::string ft_content(reinterpret_cast<const char*>(font_table->data.data()), font_table->data.size());
        std::regex key_regex(R"(fontKey="\{([A-Fa-f0-9\-]+)\}")");
        auto it_begin = std::sregex_iterator(ft_content.begin(), ft_content.end(), key_regex);
        auto it_end = std::sregex_iterator();
        for (auto it = it_begin; it != it_end; ++it) {
            std::string guid = (*it)[1].str();
            for (const auto& fe : font_entries) {
                font_guid_map[fe] = guid;
            }
        }
    }

    for (const auto& name : font_entries) {
        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        size_t orig_sz = entry->data.size();
        if (name.ends_with(".odttf")) {
            std::string guid = font_guid_map.contains(name) ? font_guid_map[name] : std::filesystem::path(name).stem().string();
            std::vector<uint8_t> deobf;
            if (FontSubsetter::deobfuscate_odttf(entry->data, guid, deobf)) {
                std::vector<uint8_t> sub;
                if (FontSubsetter::subset_ttf(deobf, used_codepoints, sub)) {
                    std::vector<uint8_t> reobf;
                    if (FontSubsetter::obfuscate_odttf(sub, guid, reobf) && reobf.size() < orig_sz) {
                        saved_bytes += static_cast<int64_t>(orig_sz - reobf.size());
                        entry->data = std::move(reobf);
                    }
                }
            }
        } else if (name.ends_with(".ttf")) {
            std::vector<uint8_t> sub;
            if (FontSubsetter::subset_ttf(entry->data, used_codepoints, sub) && sub.size() < orig_sz) {
                saved_bytes += static_cast<int64_t>(orig_sz - sub.size());
                entry->data = std::move(sub);
            }
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::optimize_xlsx_shared_strings(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto sst_entry = archive.get_entry_mut("xl/sharedStrings.xml");
    if (!sst_entry) return false;

    size_t orig_sst_sz = sst_entry->data.size();
    std::string sst_str(reinterpret_cast<const char*>(sst_entry->data.data()), sst_entry->data.size());

    std::vector<std::string> old_strings;
    size_t pos = 0;
    while (pos < sst_str.size()) {
        size_t si_start = sst_str.find("<si", pos);
        if (si_start == std::string::npos) break;
        size_t si_end = sst_str.find("</si>", si_start);
        if (si_end == std::string::npos) break;
        old_strings.push_back(sst_str.substr(si_start, si_end + 5 - si_start));
        pos = si_end + 5;
    }
    if (old_strings.empty()) return false;

    std::unordered_set<size_t> used_indices;
    std::vector<std::string> sheet_names;
    for (const auto& name : archive.entry_names()) {
        if (name.starts_with("xl/worksheets/sheet") && name.ends_with(".xml")) {
            sheet_names.push_back(name);
        }
    }
    if (sheet_names.empty()) return false;

    std::regex cell_ref_regex(R"(<c\b[^>]*\bt="s"[^>]*><v>(\d+)</v></c>)");
    for (const auto& sname : sheet_names) {
        const auto* se = archive.get_entry(sname);
        if (!se) continue;
        std::string_view sheet_sv(reinterpret_cast<const char*>(se->data.data()), se->data.size());
        auto it_begin = std::cregex_iterator(sheet_sv.data(), sheet_sv.data() + sheet_sv.size(), cell_ref_regex);
        auto it_end = std::cregex_iterator();
        for (auto it = it_begin; it != it_end; ++it) {
            size_t idx = std::stoull((*it)[1].str());
            if (idx < old_strings.size()) {
                used_indices.insert(idx);
            }
        }
    }

    std::unordered_map<std::string, size_t> content_to_new_idx;
    std::vector<std::string> new_strings;
    std::unordered_map<size_t, size_t> old_to_new;

    for (size_t old_idx = 0; old_idx < old_strings.size(); ++old_idx) {
        if (!used_indices.contains(old_idx)) continue;
        const auto& item = old_strings[old_idx];
        auto it = content_to_new_idx.find(item);
        if (it != content_to_new_idx.end()) {
            old_to_new[old_idx] = it->second;
        } else {
            size_t new_idx = new_strings.size();
            content_to_new_idx[item] = new_idx;
            new_strings.push_back(item);
            old_to_new[old_idx] = new_idx;
        }
    }

    if (new_strings.size() >= old_strings.size()) return false;

    for (const auto& sname : sheet_names) {
        auto se = archive.get_entry_mut(sname);
        if (!se) continue;
        std::string s_content(reinterpret_cast<const char*>(se->data.data()), se->data.size());

        std::string new_content;
        new_content.reserve(s_content.size());

        auto it_begin = std::sregex_iterator(s_content.begin(), s_content.end(), cell_ref_regex);
        auto it_end = std::sregex_iterator();
        size_t last_idx = 0;

        bool sheet_mod = false;
        for (auto it = it_begin; it != it_end; ++it) {
            size_t m_pos = it->position();
            size_t m_len = it->length();
            size_t old_val = std::stoull((*it)[1].str());

            new_content.append(s_content.substr(last_idx, m_pos - last_idx));
            last_idx = m_pos + m_len;

            if (old_to_new.contains(old_val)) {
                size_t nv = old_to_new[old_val];
                std::string full_match = it->str();
                std::string old_v_tag = "<v>" + std::to_string(old_val) + "</v>";
                std::string new_v_tag = "<v>" + std::to_string(nv) + "</v>";
                size_t v_pos = full_match.find(old_v_tag);
                if (v_pos != std::string::npos) {
                    full_match.replace(v_pos, old_v_tag.size(), new_v_tag);
                }
                new_content.append(full_match);
                sheet_mod = true;
            } else {
                new_content.append(it->str());
            }
        }
        new_content.append(s_content.substr(last_idx));

        if (sheet_mod && XmlOptimizer::is_well_formed(new_content)) {
            se->data.assign(new_content.begin(), new_content.end());
        }
    }

    std::string new_sst;
    new_sst.reserve(orig_sst_sz);
    new_sst = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    new_sst += "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"" +
               std::to_string(used_indices.size()) + "\" uniqueCount=\"" + std::to_string(new_strings.size()) + "\">";
    for (const auto& s : new_strings) {
        new_sst += s;
    }
    new_sst += "</sst>";

    if (XmlOptimizer::is_well_formed(new_sst)) {
        if (new_sst.size() < orig_sst_sz) {
            saved_bytes = static_cast<int64_t>(orig_sst_sz - new_sst.size());
            sst_entry->data.assign(new_sst.begin(), new_sst.end());
            return true;
        }
    }

    return false;
}

bool OoxmlOptimizer::normalize_drawingml(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    const std::vector<std::regex> redundant_patterns = {
        std::regex(R"rx(\s+rot="0")rx"),
        std::regex(R"rx(\s+flipH="(0|false)")rx"),
        std::regex(R"rx(\s+flipV="(0|false)")rx"),
        std::regex(R"rx(\s+noGrp="(0|false)")rx"),
        std::regex(R"rx(\s+noSelect="(0|false)")rx"),
        std::regex(R"rx(\s+noRot="(0|false)")rx"),
        std::regex(R"rx(\s+noChangeAspect="(0|false)")rx"),
        std::regex(R"rx(\s+noMove="(0|false)")rx"),
        std::regex(R"rx(\s+noResize="(0|false)")rx"),
        std::regex(R"rx(\s+noEditPoints="(0|false)")rx"),
        std::regex(R"rx(\s+noAdjustHandles="(0|false)")rx"),
        std::regex(R"rx(\s+noChangeArrowheads="(0|false)")rx"),
        std::regex(R"rx(\s+noChangeShapeType="(0|false)")rx"),
        std::regex(R"rx(<a:avLst>\s*</a:avLst>)rx"),
        std::regex(R"rx(<a:extLst>\s*</a:extLst>)rx"),
        std::regex(R"rx(<a:effectLst>\s*</a:effectLst>)rx"),
        std::regex(R"rx(<a:spPr>\s*</a:spPr>)rx")
    };

    const std::vector<std::string> replacements = {
        "", "", "", "", "", "", "", "", "", "", "", "", "",
        "<a:avLst/>", "<a:extLst/>", "<a:effectLst/>", "<a:spPr/>"
    };

    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".xml")) continue;
        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string_view sv(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        if (sv.find("<a:") == std::string_view::npos &&
            sv.find("<wp:") == std::string_view::npos &&
            sv.find("<xdr:") == std::string_view::npos &&
            sv.find("<p:sp") == std::string_view::npos) {
            continue;
        }

        size_t orig_sz = entry->data.size();
        std::string text(sv);

        for (size_t i = 0; i < redundant_patterns.size(); ++i) {
            text = std::regex_replace(text, redundant_patterns[i], replacements[i]);
        }

        if (text.size() < orig_sz && XmlOptimizer::is_well_formed(text)) {
            saved_bytes += static_cast<int64_t>(orig_sz - text.size());
            entry->data.assign(text.begin(), text.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::remove_hidden_objects(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    const std::vector<std::regex> hidden_patterns = {
        std::regex(R"rx(<w:proofErr\b[^>]*/>)rx"),
        std::regex(R"rx(<w:r\b[^>]*><w:rPr\b[^>]*><w:vanish\b[^>]*/>.*?</w:rPr>\s*</w:r>)rx"),
        std::regex(R"rx(<w:r\b[^>]*><w:rPr\b[^>]*><w:vanish\b[^>]*/>.*?</w:rPr>\s*<w:t\b[^>]*></w:t>\s*</w:r>)rx"),
        std::regex(R"rx(<w:r\b[^>]*><w:rPr\b[^>]*><w:vanish\b[^>]*/>.*?</w:rPr>\s*<w:t\b[^>]*/>\s*</w:r>)rx"),
        std::regex(R"rx(<w:commentRangeStart\b[^>]*/>)rx"),
        std::regex(R"rx(<w:commentRangeEnd\b[^>]*/>)rx"),
        std::regex(R"rx(<w:r\b[^>]*><w:commentReference\b[^>]*/>\s*</w:r>)rx")
    };

    bool has_comments = archive.has_entry("word/comments.xml");

    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".xml")) continue;
        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string_view sv(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        if (sv.find("<w:proofErr") == std::string_view::npos &&
            sv.find("<w:vanish") == std::string_view::npos &&
            (!has_comments ? sv.find("<w:comment") == std::string_view::npos : true)) {
            continue;
        }

        size_t orig_sz = entry->data.size();
        std::string text(sv);

        for (size_t i = 0; i < hidden_patterns.size(); ++i) {
            if (i >= 4 && has_comments) continue;
            text = std::regex_replace(text, hidden_patterns[i], "");
        }

        if (text.size() < orig_sz && XmlOptimizer::is_well_formed(text)) {
            saved_bytes += static_cast<int64_t>(orig_sz - text.size());
            entry->data.assign(text.begin(), text.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::clean_orphan_relationships(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::unordered_set<std::string> referenced_targets;
    std::unordered_set<std::string> referenced_filenames;

    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".rels")) continue;
        const auto* re = archive.get_entry(name);
        if (!re) continue;

        std::string_view content(reinterpret_cast<const char*>(re->data.data()), re->data.size());
        size_t base_end = name.rfind('/');
        std::string base_dir = (base_end != std::string::npos) ? name.substr(0, base_end) : "";
        if (base_dir.ends_with("/_rels")) {
            base_dir = base_dir.substr(0, base_dir.size() - 6);
        } else if (base_dir == "_rels") {
            base_dir = "";
        }

        size_t pos = 0;
        while (pos < content.size()) {
            size_t t_open = content.find("Target=\"", pos);
            if (t_open == std::string_view::npos) break;
            size_t val_start = t_open + 8;
            size_t val_end = content.find('"', val_start);
            if (val_end == std::string_view::npos) break;

            std::string_view target = content.substr(val_start, val_end - val_start);
            pos = val_end + 1;

            if (target.starts_with("http://") || target.starts_with("https://") || target.starts_with("mailto:")) {
                continue;
            }

            std::filesystem::path p;
            if (target.starts_with("/")) {
                p = std::filesystem::path(target.substr(1)).lexically_normal();
            } else if (!base_dir.empty()) {
                p = (std::filesystem::path(base_dir) / target).lexically_normal();
            } else {
                p = std::filesystem::path(target).lexically_normal();
            }

            std::string norm = p.generic_string();
            referenced_targets.insert(norm);
            referenced_targets.insert("/" + norm);
            referenced_filenames.insert(p.filename().generic_string());
        }
    }

    std::vector<std::string> to_remove;
    for (const auto& entry : archive.entries()) {
        if (entry.name.find("/media/") == std::string::npos &&
            entry.name.find("/embeddings/") == std::string::npos &&
            entry.name.find("/customPayload/") == std::string::npos) {
            continue;
        }

        std::filesystem::path p(entry.name);
        std::string fname = p.filename().generic_string();

        if (!referenced_targets.contains(entry.name) && !referenced_filenames.contains(fname)) {
            to_remove.push_back(entry.name);
            saved_bytes += static_cast<int64_t>(entry.data.size());
        }
    }

    if (to_remove.empty()) return false;

    for (const auto& name : to_remove) {
        archive.remove(name);
    }

    auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
    if (ct_entry) {
        std::string ct(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
        for (const auto& name : to_remove) {
            std::regex r(R"rx(<Override\b[^>]*PartName="/)rx" + name + R"rx("[^>]*/>)rx");
            ct = std::regex_replace(ct, r, "");
        }
        if (XmlOptimizer::is_well_formed(ct)) {
            ct_entry->data.assign(ct.begin(), ct.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::optimize_embedded_packages(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    std::vector<std::string> pkg_names;
    for (const auto& name : archive.entry_names()) {
        if (name.find("/embeddings/") != std::string::npos &&
            (name.ends_with(".xlsx") || name.ends_with(".docx") || name.ends_with(".pptx") ||
             name.ends_with(".xlsm") || name.ends_with(".docm") || name.ends_with(".pptm"))) {
            pkg_names.push_back(name);
        }
    }

    for (const auto& pkg_name : pkg_names) {
        auto* entry = archive.get_entry_mut(pkg_name);
        if (!entry || entry->data.size() < 30) continue;

        DocumentFormat sub_format = DocumentFormat::Docx;
        if (pkg_name.ends_with(".xlsx") || pkg_name.ends_with(".xlsm")) sub_format = DocumentFormat::Xlsx;
        else if (pkg_name.ends_with(".pptx") || pkg_name.ends_with(".pptm")) sub_format = DocumentFormat::Pptx;

        ZipArchive sub_archive;
        if (ZipArchive::read(entry->data, sub_archive)) {
            OptimizationReport sub_report;
            if (OoxmlOptimizer::optimize(sub_archive, sub_format, sub_report)) {
                std::vector<uint8_t> new_pkg;
                if (sub_archive.write(new_pkg, true)) {
                    if (new_pkg.size() < entry->data.size()) {
                        saved_bytes += static_cast<int64_t>(entry->data.size() - new_pkg.size());
                        entry->data = std::move(new_pkg);
                    }
                }
            }
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::normalize_content_types(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
    if (!ct_entry) return false;

    size_t orig_sz = ct_entry->data.size();
    std::string xml(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());

    std::unordered_set<std::string> existing_parts;
    for (const auto& name : archive.entry_names()) {
        existing_parts.insert(name);
        existing_parts.insert("/" + name);
    }

    std::unordered_map<std::string, std::string> defaults;
    std::regex def_regex(R"rx(<Default\b[^>]*\bExtension="([^"]+)"[^>]*\bContentType="([^"]+)"[^>]*/>)rx");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), def_regex); it != std::sregex_iterator(); ++it) {
        std::string ext = (*it)[1].str();
        std::string ctype = (*it)[2].str();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        defaults[ext] = ctype;
    }

    std::regex def_regex_rev(R"rx(<Default\b[^>]*\bContentType="([^"]+)"[^>]*\bExtension="([^"]+)"[^>]*/>)rx");
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), def_regex_rev); it != std::sregex_iterator(); ++it) {
        std::string ext = (*it)[2].str();
        std::string ctype = (*it)[1].str();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        defaults[ext] = ctype;
    }

    std::regex override_regex(R"rx(<Override\b[^>]*\bPartName="([^"]+)"[^>]*\bContentType="([^"]+)"[^>]*/>)rx");
    std::regex override_regex_rev(R"rx(<Override\b[^>]*\bContentType="([^"]+)"[^>]*\bPartName="([^"]+)"[^>]*/>)rx");

    std::unordered_map<std::string, std::vector<std::string>> ext_overrides;

    auto collect_overrides = [&](const std::regex& rgx, bool rev) {
        for (auto it = std::sregex_iterator(xml.begin(), xml.end(), rgx); it != std::sregex_iterator(); ++it) {
            std::string part = rev ? (*it)[2].str() : (*it)[1].str();
            std::string ctype = rev ? (*it)[1].str() : (*it)[2].str();
            size_t dot = part.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = part.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                ext_overrides[ext].push_back(ctype);
            }
        }
    };
    collect_overrides(override_regex, false);
    collect_overrides(override_regex_rev, true);

    std::string new_defaults;
    for (const auto& [ext, ctypes] : ext_overrides) {
        if (!defaults.contains(ext) && ctypes.size() >= 2) {
            bool all_same = true;
            for (const auto& ct : ctypes) {
                if (ct != ctypes[0]) {
                    all_same = false;
                    break;
                }
            }
            if (all_same) {
                defaults[ext] = ctypes[0];
                new_defaults += "<Default Extension=\"" + ext + "\" ContentType=\"" + ctypes[0] + "\"/>";
            }
        }
    }

    auto filter_overrides = [&](const std::regex& rgx, bool rev, std::string& target_xml) {
        std::string result;
        result.reserve(target_xml.size());
        size_t last_idx = 0;
        for (auto it = std::sregex_iterator(target_xml.begin(), target_xml.end(), rgx); it != std::sregex_iterator(); ++it) {
            result.append(target_xml.substr(last_idx, it->position() - last_idx));
            last_idx = it->position() + it->length();

            std::string part = rev ? (*it)[2].str() : (*it)[1].str();
            std::string ctype = rev ? (*it)[1].str() : (*it)[2].str();

            if (!existing_parts.contains(part)) {
                continue;
            }

            size_t dot = part.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = part.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (defaults.contains(ext) && defaults[ext] == ctype) {
                    continue;
                }
            }

            result.append(it->str());
        }
        result.append(target_xml.substr(last_idx));
        target_xml = std::move(result);
    };

    filter_overrides(override_regex, false, xml);
    filter_overrides(override_regex_rev, true, xml);

    if (!new_defaults.empty()) {
        size_t types_tag = xml.find("<Types");
        if (types_tag != std::string::npos) {
            size_t open_close = xml.find('>', types_tag);
            if (open_close != std::string::npos) {
                xml.insert(open_close + 1, new_defaults);
            }
        }
    }

    if (xml.size() < orig_sz && XmlOptimizer::is_well_formed(xml)) {
        saved_bytes += static_cast<int64_t>(orig_sz - xml.size());
        ct_entry->data.assign(xml.begin(), xml.end());
        return true;
    }
    return false;
}

bool OoxmlOptimizer::shorten_media_paths(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    std::unordered_map<std::string, std::string> renames;
    std::unordered_map<std::string, std::string> rel_target_renames;

    int counter = 1;
    for (const auto& name : archive.entry_names()) {
        size_t med_pos = name.find("/media/");
        if (med_pos == std::string::npos) continue;

        size_t dot_pos = name.rfind('.');
        if (dot_pos == std::string::npos || dot_pos < med_pos + 7) continue;

        std::string ext = name.substr(dot_pos);
        std::string prefix = name.substr(0, med_pos + 7);
        std::string old_filename = name.substr(med_pos + 7);

        std::string new_filename = std::to_string(counter++) + ext;
        if (new_filename.size() >= old_filename.size()) continue;

        std::string new_name = prefix + new_filename;
        renames[name] = new_name;
        rel_target_renames["media/" + old_filename] = "media/" + new_filename;
        rel_target_renames["/word/media/" + old_filename] = "/word/media/" + new_filename;
        rel_target_renames["/ppt/media/" + old_filename] = "/ppt/media/" + new_filename;
        rel_target_renames["/xl/media/" + old_filename] = "/xl/media/" + new_filename;
    }

    if (renames.empty()) return false;

    for (const auto& [old_path, new_path] : renames) {
        const auto* old_entry = archive.get_entry(old_path);
        if (old_entry) {
            ZipEntry new_entry = *old_entry;
            new_entry.name = new_path;
            archive.remove(old_path);
            archive.add_or_replace(new_entry);
            saved_bytes += static_cast<int64_t>(old_path.size() - new_path.size()) * 2;
        }
    }

    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".rels")) continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string content(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = content.size();
        for (const auto& [old_t, new_t] : rel_target_renames) {
            size_t p = 0;
            while ((p = content.find(old_t, p)) != std::string::npos) {
                content.replace(p, old_t.size(), new_t);
                p += new_t.size();
            }
        }
        if (content.size() != orig_sz) {
            entry->data.assign(content.begin(), content.end());
            if (content.size() < orig_sz) {
                saved_bytes += static_cast<int64_t>(orig_sz - content.size());
            }
        }
    }

    auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
    if (ct_entry) {
        std::string content(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
        size_t orig_sz = content.size();
        for (const auto& [old_path, new_path] : renames) {
            std::string old_part = "/" + old_path;
            std::string new_part = "/" + new_path;
            size_t p = 0;
            while ((p = content.find(old_part, p)) != std::string::npos) {
                content.replace(p, old_part.size(), new_part);
                p += new_part.size();
            }
        }
        if (content.size() != orig_sz) {
            ct_entry->data.assign(content.begin(), content.end());
            if (content.size() < orig_sz) {
                saved_bytes += static_cast<int64_t>(orig_sz - content.size());
            }
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::lift_paragraph_properties(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* styles_entry = archive.get_entry_mut("word/styles.xml");
    if (!styles_entry) return false;

    std::string styles_xml(reinterpret_cast<const char*>(styles_entry->data.data()), styles_entry->data.size());

    std::string default_style = "Normal";
    size_t s_pos = 0;
    while ((s_pos = styles_xml.find("<w:style ", s_pos)) != std::string::npos) {
        size_t s_end = styles_xml.find('>', s_pos);
        if (s_end == std::string::npos) break;
        std::string_view tag(styles_xml.data() + s_pos, s_end - s_pos + 1);
        if (tag.find("w:type=\"paragraph\"") != std::string_view::npos &&
            (tag.find("w:default=\"1\"") != std::string_view::npos || tag.find("w:default=\"true\"") != std::string_view::npos)) {
            size_t id_pos = tag.find("w:styleId=\"");
            if (id_pos != std::string_view::npos) {
                size_t v_start = id_pos + 11;
                size_t v_end = tag.find('"', v_start);
                if (v_end != std::string_view::npos) {
                    default_style = std::string(tag.substr(v_start, v_end - v_start));
                }
            }
            break;
        }
        s_pos = s_end + 1;
    }

    struct ElementAttrMap {
        std::string tag_name;
        std::map<std::string, std::string> attrs;
        std::string raw_element;
    };

    auto parse_attrs = [](std::string_view elem) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> res;
        size_t p = 0;
        while (p < elem.size() && elem[p] != ' ' && elem[p] != '>' && elem[p] != '/') ++p;
        while (p < elem.size()) {
            while (p < elem.size() && (elem[p] == ' ' || elem[p] == '\r' || elem[p] == '\n' || elem[p] == '\t')) ++p;
            if (p >= elem.size() || elem[p] == '>' || elem[p] == '/') break;
            size_t eq = elem.find('=', p);
            if (eq == std::string_view::npos) break;
            std::string attr_name(elem.substr(p, eq - p));
            size_t q1 = elem.find('"', eq);
            if (q1 == std::string_view::npos) break;
            size_t q2 = elem.find('"', q1 + 1);
            if (q2 == std::string_view::npos) break;
            res[attr_name] = std::string(elem.substr(q1 + 1, q2 - (q1 + 1)));
            p = q2 + 1;
        }
        return res;
    };

    auto parse_ppr_elements = [&](std::string_view ppr_content) -> std::vector<ElementAttrMap> {
        std::vector<ElementAttrMap> elements;
        size_t pos = 0;
        while (pos < ppr_content.size()) {
            size_t tag_start = ppr_content.find('<', pos);
            if (tag_start == std::string_view::npos) break;
            if (tag_start + 1 < ppr_content.size() && ppr_content[tag_start + 1] == '/') {
                pos = tag_start + 1;
                continue;
            }
            size_t name_end = tag_start + 1;
            while (name_end < ppr_content.size() && ppr_content[name_end] != ' ' && ppr_content[name_end] != '>' && ppr_content[name_end] != '/') {
                ++name_end;
            }
            std::string tag_name(ppr_content.substr(tag_start + 1, name_end - (tag_start + 1)));
            size_t tag_close = ppr_content.find('>', tag_start);
            if (tag_close == std::string_view::npos) break;

            if (ppr_content[tag_close - 1] == '/') {
                std::string_view raw(ppr_content.data() + tag_start, tag_close - tag_start + 1);
                elements.push_back({tag_name, parse_attrs(raw), std::string(raw)});
                pos = tag_close + 1;
            } else {
                std::string close_tag = "</" + tag_name + ">";
                size_t close_pos = ppr_content.find(close_tag, tag_close);
                if (close_pos != std::string_view::npos) {
                    size_t full_end = close_pos + close_tag.size();
                    std::string_view raw(ppr_content.data() + tag_start, full_end - tag_start);
                    elements.push_back({tag_name, parse_attrs(raw), std::string(raw)});
                    pos = full_end;
                } else {
                    pos = tag_close + 1;
                }
            }
        }
        return elements;
    };

    std::unordered_map<std::string, std::vector<ElementAttrMap>> style_ppr_map;
    s_pos = 0;
    while ((s_pos = styles_xml.find("<w:style ", s_pos)) != std::string::npos) {
        size_t s_tag_close = styles_xml.find('>', s_pos);
        if (s_tag_close == std::string::npos) break;
        std::string_view s_open(styles_xml.data() + s_pos, s_tag_close - s_pos + 1);
        if (s_open.find("w:type=\"paragraph\"") == std::string_view::npos) {
            s_pos = s_tag_close + 1;
            continue;
        }
        std::string style_id;
        size_t id_pos = s_open.find("w:styleId=\"");
        if (id_pos != std::string_view::npos) {
            size_t v_start = id_pos + 11;
            size_t v_end = s_open.find('"', v_start);
            if (v_end != std::string_view::npos) {
                style_id = std::string(s_open.substr(v_start, v_end - v_start));
            }
        }
        size_t style_end = styles_xml.find("</w:style>", s_tag_close);
        if (style_end == std::string::npos) break;

        size_t ppr_start = styles_xml.find("<w:pPr>", s_tag_close);
        if (ppr_start != std::string_view::npos && ppr_start < style_end) {
            size_t ppr_end = styles_xml.find("</w:pPr>", ppr_start);
            if (ppr_end != std::string_view::npos && ppr_end <= style_end) {
                std::string_view ppr_body(styles_xml.data() + ppr_start + 7, ppr_end - (ppr_start + 7));
                if (!style_id.empty()) {
                    style_ppr_map[style_id] = parse_ppr_elements(ppr_body);
                }
            }
        }
        s_pos = style_end + 10;
    }

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        if (name == "word/styles.xml" || name == "word/stylesWithEffects.xml") continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string xml(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = xml.size();
        std::string rebuilt;
        rebuilt.reserve(xml.size());

        size_t last_pos = 0;
        size_t p_pos = 0;
        while ((p_pos = xml.find("<w:p", p_pos)) != std::string::npos) {
            if (p_pos + 4 < xml.size()) {
                char c = xml[p_pos + 4];
                if (c != ' ' && c != '>' && c != '\r' && c != '\n' && c != '\t') {
                    p_pos += 4;
                    continue;
                }
            }
            size_t p_tag_close = xml.find('>', p_pos);
            if (p_tag_close == std::string::npos) break;
            if (xml[p_tag_close - 1] == '/') {
                p_pos = p_tag_close + 1;
                continue;
            }
            size_t p_end = xml.find("</w:p>", p_tag_close);
            if (p_end == std::string::npos) break;

            size_t ppr_start = xml.find("<w:pPr", p_tag_close);
            if (ppr_start != std::string_view::npos && ppr_start < p_end) {
                size_t ppr_tag_close = xml.find('>', ppr_start);
                if (ppr_tag_close != std::string_view::npos && ppr_tag_close < p_end) {
                    if (xml[ppr_tag_close - 1] == '/') {
                        rebuilt.append(xml.substr(last_pos, ppr_start - last_pos));
                        last_pos = ppr_tag_close + 1;
                    } else {
                        size_t ppr_end_tag = xml.find("</w:pPr>", ppr_tag_close);
                        if (ppr_end_tag != std::string_view::npos && ppr_end_tag < p_end) {
                            std::string ppr_inner = xml.substr(ppr_tag_close + 1, ppr_end_tag - (ppr_tag_close + 1));
                            std::string effective_style = default_style;
                            std::regex pstyle_regex(R"rx(<w:pStyle\b[^>]*\bw:val="([^"]+)"[^>]*/>)rx");
                            std::smatch match;
                            if (std::regex_search(ppr_inner, match, pstyle_regex)) {
                                std::string found_style = match[1].str();
                                if (found_style == default_style) {
                                    ppr_inner.erase(match.position(), match.length());
                                } else {
                                    effective_style = found_style;
                                }
                            }

                            if (style_ppr_map.contains(effective_style)) {
                                const auto& st_elems = style_ppr_map[effective_style];
                                auto cur_elems = parse_ppr_elements(ppr_inner);
                                for (const auto& ce : cur_elems) {
                                    if (ce.tag_name == "w:pStyle" || ce.tag_name == "w:rPr" || ce.tag_name == "w:sectPr") continue;
                                    bool matches = false;
                                    for (const auto& se : st_elems) {
                                        if (se.tag_name == ce.tag_name && se.attrs == ce.attrs) {
                                            matches = true;
                                            break;
                                        }
                                    }
                                    if (matches) {
                                        size_t found_idx = ppr_inner.find(ce.raw_element);
                                        if (found_idx != std::string::npos) {
                                            ppr_inner.erase(found_idx, ce.raw_element.size());
                                        }
                                    }
                                }
                            }

                            bool only_ws = true;
                            for (char ch : ppr_inner) {
                                if (ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t') {
                                    only_ws = false;
                                    break;
                                }
                            }

                            rebuilt.append(xml.substr(last_pos, ppr_start - last_pos));
                            if (!only_ws) {
                                rebuilt.append(xml.substr(ppr_start, ppr_tag_close - ppr_start + 1));
                                rebuilt.append(ppr_inner);
                                rebuilt.append("</w:pPr>");
                            }
                            last_pos = ppr_end_tag + 8;
                        }
                    }
                }
            }
            p_pos = p_end + 6;
        }

        rebuilt.append(xml.substr(last_pos));
        if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
            saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
            entry->data.assign(rebuilt.begin(), rebuilt.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::prune_numbering(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* num_entry = archive.get_entry_mut("word/numbering.xml");
    if (!num_entry) return false;

    std::unordered_set<std::string> used_num_ids;
    std::regex num_id_regex(R"rx(<w:numId\b[^>]*\bw:val="([^"]+)")rx");

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml") || name == "word/numbering.xml") continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string content(reinterpret_cast<const char*>(e->data.data()), e->data.size());
        auto words_begin = std::sregex_iterator(content.begin(), content.end(), num_id_regex);
        auto words_end = std::sregex_iterator();
        for (auto it = words_begin; it != words_end; ++it) {
            used_num_ids.insert((*it)[1].str());
        }
    }

    if (used_num_ids.empty()) {
        saved_bytes += static_cast<int64_t>(num_entry->data.size());
        archive.remove("word/numbering.xml");

        for (const auto& name : archive.entry_names()) {
            if (!name.ends_with(".rels")) continue;
            auto* rels = archive.get_entry_mut(name);
            if (!rels) continue;
            std::string content(reinterpret_cast<const char*>(rels->data.data()), rels->data.size());
            size_t orig_sz = content.size();
            std::regex num_rel_regex(R"rx(<Relationship[^>]*Target="([^"]*numbering\.xml)"[^>]*/>)rx");
            content = std::regex_replace(content, num_rel_regex, "");
            if (content.size() != orig_sz) {
                rels->data.assign(content.begin(), content.end());
            }
        }

        auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
        if (ct_entry) {
            std::string content(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
            size_t orig_sz = content.size();
            std::regex num_ct_regex(R"rx(<Override[^>]*PartName="/word/numbering\.xml"[^>]*/>)rx");
            content = std::regex_replace(content, num_ct_regex, "");
            if (content.size() != orig_sz) {
                ct_entry->data.assign(content.begin(), content.end());
            }
        }

        return true;
    }

    std::string num_xml(reinterpret_cast<const char*>(num_entry->data.data()), num_entry->data.size());
    size_t orig_sz = num_xml.size();

    std::unordered_set<std::string> used_abstract_ids;
    std::vector<std::string> kept_nums;
    std::regex num_elem_regex(R"rx(<w:num\b[^>]*\bw:numId="([^"]+)"[^>]*>[\s\S]*?</w:num>)rx");
    auto num_begin = std::sregex_iterator(num_xml.begin(), num_xml.end(), num_elem_regex);
    auto num_end = std::sregex_iterator();
    for (auto it = num_begin; it != num_end; ++it) {
        std::string nid = (*it)[1].str();
        if (used_num_ids.contains(nid)) {
            std::string full_num = (*it).str();
            kept_nums.push_back(full_num);
            std::regex abs_id_regex(R"rx(<w:abstractNumId\b[^>]*\bw:val="([^"]+)")rx");
            std::smatch m;
            if (std::regex_search(full_num, m, abs_id_regex)) {
                used_abstract_ids.insert(m[1].str());
            }
        }
    }

    std::vector<std::string> kept_abstracts;
    std::regex abs_elem_regex(R"rx(<w:abstractNum\b[^>]*\bw:abstractNumId="([^"]+)"[^>]*>[\s\S]*?</w:abstractNum>)rx");
    auto abs_begin = std::sregex_iterator(num_xml.begin(), num_xml.end(), abs_elem_regex);
    auto abs_end = std::sregex_iterator();
    for (auto it = abs_begin; it != abs_end; ++it) {
        std::string aid = (*it)[1].str();
        if (used_abstract_ids.contains(aid)) {
            kept_abstracts.push_back((*it).str());
        }
    }

    size_t root_open_start = num_xml.find("<w:numbering");
    if (root_open_start == std::string::npos) return false;
    size_t root_open_end = num_xml.find('>', root_open_start);
    if (root_open_end == std::string::npos) return false;
    size_t root_close_start = num_xml.rfind("</w:numbering>");
    if (root_close_start == std::string::npos) return false;

    std::string rebuilt;
    rebuilt.reserve(orig_sz);
    rebuilt.append(num_xml.substr(0, root_open_end + 1));
    for (const auto& a : kept_abstracts) rebuilt.append(a);
    for (const auto& n : kept_nums) rebuilt.append(n);
    rebuilt.append("</w:numbering>");

    if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
        saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
        num_entry->data.assign(rebuilt.begin(), rebuilt.end());
        return true;
    }

    return false;
}

bool OoxmlOptimizer::normalize_table_cells(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        if (name == "word/styles.xml" || name == "word/stylesWithEffects.xml" || name == "word/numbering.xml") continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string xml(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = xml.size();
        std::string rebuilt;
        rebuilt.reserve(xml.size());

        size_t last_tbl_pos = 0;
        size_t tbl_pos = 0;
        while ((tbl_pos = xml.find("<w:tbl", tbl_pos)) != std::string::npos) {
            size_t tbl_open_end = xml.find('>', tbl_pos);
            if (tbl_open_end == std::string::npos) break;
            size_t tbl_end = xml.find("</w:tbl>", tbl_open_end);
            if (tbl_end == std::string::npos) break;
            tbl_end += 8;

            std::string tbl_content = xml.substr(tbl_pos, tbl_end - tbl_pos);

            std::vector<std::string> grid_cols;
            size_t grid_start = tbl_content.find("<w:tblGrid");
            if (grid_start != std::string::npos) {
                size_t grid_end = tbl_content.find("</w:tblGrid>", grid_start);
                if (grid_end != std::string::npos) {
                    std::string grid_str = tbl_content.substr(grid_start, (grid_end + 12) - grid_start);
                    std::regex col_regex(R"rx(<w:gridCol\b[^>]*\bw:w="([^"]+)"[^>]*/>)rx");
                    auto cb = std::sregex_iterator(grid_str.begin(), grid_str.end(), col_regex);
                    auto ce = std::sregex_iterator();
                    for (auto it = cb; it != ce; ++it) {
                        grid_cols.push_back((*it)[1].str());
                    }
                }
            }

            if (!grid_cols.empty()) {
                std::string tbl_rebuilt;
                tbl_rebuilt.reserve(tbl_content.size());
                size_t last_tr_pos = 0;
                size_t tr_pos = 0;

                while ((tr_pos = tbl_content.find("<w:tr", tr_pos)) != std::string::npos) {
                    size_t tr_open_end = tbl_content.find('>', tr_pos);
                    if (tr_open_end == std::string::npos) break;
                    size_t tr_end = tbl_content.find("</w:tr>", tr_open_end);
                    if (tr_end == std::string::npos) break;
                    tr_end += 7;

                    std::string tr_content = tbl_content.substr(tr_pos, tr_end - tr_pos);
                    std::string tr_rebuilt;
                    tr_rebuilt.reserve(tr_content.size());

                    size_t last_tc_pos = 0;
                    size_t tc_pos = 0;
                    size_t col_idx = 0;

                    while ((tc_pos = tr_content.find("<w:tc", tc_pos)) != std::string::npos) {
                        if (tc_pos + 5 < tr_content.size()) {
                            char c = tr_content[tc_pos + 5];
                            if (c != ' ' && c != '>' && c != '\r' && c != '\n' && c != '\t') {
                                tc_pos += 5;
                                continue;
                            }
                        }
                        size_t tc_open_end = tr_content.find('>', tc_pos);
                        if (tc_open_end == std::string::npos) break;
                        size_t tc_end = tr_content.find("</w:tc>", tc_open_end);
                        if (tc_end == std::string::npos) break;
                        tc_end += 7;

                        std::string tc_content = tr_content.substr(tc_pos, tc_end - tc_pos);
                        size_t span = 1;
                        std::regex span_regex(R"rx(<w:gridSpan\b[^>]*\bw:val="([0-9]+)"[^>]*/>)rx");
                        std::smatch span_m;
                        if (std::regex_search(tc_content, span_m, span_regex)) {
                            span = static_cast<size_t>(std::stoul(span_m[1].str()));
                        }

                        if (span == 1 && col_idx < grid_cols.size()) {
                            const std::string& expected_w = grid_cols[col_idx];
                            size_t tcpr_start = tc_content.find("<w:tcPr");
                            if (tcpr_start != std::string::npos) {
                                size_t tcpr_tag_close = tc_content.find('>', tcpr_start);
                                if (tcpr_tag_close != std::string::npos) {
                                    if (tc_content[tcpr_tag_close - 1] == '/') {
                                        tc_content.erase(tcpr_start, tcpr_tag_close - tcpr_start + 1);
                                    } else {
                                        size_t tcpr_end = tc_content.find("</w:tcPr>", tcpr_tag_close);
                                        if (tcpr_end != std::string::npos) {
                                            std::string tcpr_inner = tc_content.substr(tcpr_tag_close + 1, tcpr_end - (tcpr_tag_close + 1));
                                            std::regex tcw_regex(R"rx(<w:tcW\b[^>]*\bw:w="([^"]+)"[^>]*/>)rx");
                                            std::smatch tcw_m;
                                            if (std::regex_search(tcpr_inner, tcw_m, tcw_regex)) {
                                                if (tcw_m[1].str() == expected_w) {
                                                    tcpr_inner.erase(tcw_m.position(), tcw_m.length());
                                                }
                                            }
                                            bool only_ws = true;
                                            for (char ch : tcpr_inner) {
                                                if (ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t') {
                                                    only_ws = false;
                                                    break;
                                                }
                                            }
                                            if (only_ws) {
                                                tc_content.erase(tcpr_start, (tcpr_end + 9) - tcpr_start);
                                            } else {
                                                tc_content.replace(tcpr_tag_close + 1, tcpr_end - (tcpr_tag_close + 1), tcpr_inner);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        col_idx += span;
                        tr_rebuilt.append(tr_content.substr(last_tc_pos, tc_pos - last_tc_pos));
                        tr_rebuilt.append(tc_content);
                        last_tc_pos = tc_end;
                        tc_pos = tc_end;
                    }

                    tr_rebuilt.append(tr_content.substr(last_tc_pos));
                    tbl_rebuilt.append(tbl_content.substr(last_tr_pos, tr_pos - last_tr_pos));
                    tbl_rebuilt.append(tr_rebuilt);
                    last_tr_pos = tr_end;
                    tr_pos = tr_end;
                }

                tbl_rebuilt.append(tbl_content.substr(last_tr_pos));
                tbl_content = std::move(tbl_rebuilt);
            }

            rebuilt.append(xml.substr(last_tbl_pos, tbl_pos - last_tbl_pos));
            rebuilt.append(tbl_content);
            last_tbl_pos = tbl_end;
            tbl_pos = tbl_end;
        }

        rebuilt.append(xml.substr(last_tbl_pos));
        if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
            saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
            entry->data.assign(rebuilt.begin(), rebuilt.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::strip_run_properties(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* styles_entry = archive.get_entry_mut("word/styles.xml");
    if (!styles_entry) return false;

    std::string styles_xml(reinterpret_cast<const char*>(styles_entry->data.data()), styles_entry->data.size());

    std::string default_style = "Normal";
    size_t s_pos = 0;
    while ((s_pos = styles_xml.find("<w:style ", s_pos)) != std::string::npos) {
        size_t s_end = styles_xml.find('>', s_pos);
        if (s_end == std::string::npos) break;
        std::string_view tag(styles_xml.data() + s_pos, s_end - s_pos + 1);
        if (tag.find("w:type=\"paragraph\"") != std::string_view::npos &&
            (tag.find("w:default=\"1\"") != std::string_view::npos || tag.find("w:default=\"true\"") != std::string_view::npos)) {
            size_t id_pos = tag.find("w:styleId=\"");
            if (id_pos != std::string_view::npos) {
                size_t v_start = id_pos + 11;
                size_t v_end = tag.find('"', v_start);
                if (v_end != std::string_view::npos) {
                    default_style = std::string(tag.substr(v_start, v_end - v_start));
                }
            }
            break;
        }
        s_pos = s_end + 1;
    }

    struct RprElement {
        std::string tag_name;
        std::map<std::string, std::string> attrs;
        std::string raw_element;
    };

    auto parse_attrs = [](std::string_view elem) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> res;
        size_t p = 0;
        while (p < elem.size() && elem[p] != ' ' && elem[p] != '>' && elem[p] != '/') ++p;
        while (p < elem.size()) {
            while (p < elem.size() && (elem[p] == ' ' || elem[p] == '\r' || elem[p] == '\n' || elem[p] == '\t')) ++p;
            if (p >= elem.size() || elem[p] == '>' || elem[p] == '/') break;
            size_t eq = elem.find('=', p);
            if (eq == std::string_view::npos) break;
            std::string attr_name(elem.substr(p, eq - p));
            size_t q1 = elem.find('"', eq);
            if (q1 == std::string_view::npos) break;
            size_t q2 = elem.find('"', q1 + 1);
            if (q2 == std::string_view::npos) break;
            res[attr_name] = std::string(elem.substr(q1 + 1, q2 - (q1 + 1)));
            p = q2 + 1;
        }
        return res;
    };

    auto parse_rpr_elements = [&](std::string_view rpr_content) -> std::vector<RprElement> {
        std::vector<RprElement> elements;
        size_t pos = 0;
        while (pos < rpr_content.size()) {
            size_t tag_start = rpr_content.find('<', pos);
            if (tag_start == std::string_view::npos) break;
            if (tag_start + 1 < rpr_content.size() && rpr_content[tag_start + 1] == '/') {
                pos = tag_start + 1;
                continue;
            }
            size_t name_end = tag_start + 1;
            while (name_end < rpr_content.size() && rpr_content[name_end] != ' ' && rpr_content[name_end] != '>' && rpr_content[name_end] != '/') {
                ++name_end;
            }
            std::string tag_name(rpr_content.substr(tag_start + 1, name_end - (tag_start + 1)));
            size_t tag_close = rpr_content.find('>', tag_start);
            if (tag_close == std::string_view::npos) break;

            if (rpr_content[tag_close - 1] == '/') {
                std::string_view raw(rpr_content.data() + tag_start, tag_close - tag_start + 1);
                elements.push_back({tag_name, parse_attrs(raw), std::string(raw)});
                pos = tag_close + 1;
            } else {
                std::string close_tag = "</" + tag_name + ">";
                size_t close_pos = rpr_content.find(close_tag, tag_close);
                if (close_pos != std::string_view::npos) {
                    size_t full_end = close_pos + close_tag.size();
                    std::string_view raw(rpr_content.data() + tag_start, full_end - tag_start);
                    elements.push_back({tag_name, parse_attrs(raw), std::string(raw)});
                    pos = full_end;
                } else {
                    pos = tag_close + 1;
                }
            }
        }
        return elements;
    };

    auto* main_doc_entry = archive.get_entry("word/document.xml");
    if (main_doc_entry) {
        std::string doc_str(reinterpret_cast<const char*>(main_doc_entry->data.data()), main_doc_entry->data.size());
        std::map<std::string, int> lang_freq;
        std::regex lang_r(R"rx(<w:lang\b[^>]*/>)rx");
        for (auto it = std::sregex_iterator(doc_str.begin(), doc_str.end(), lang_r); it != std::sregex_iterator(); ++it) {
            lang_freq[it->str()]++;
        }
        std::string dominant_lang;
        int max_l_cnt = 0;
        int total_l_cnt = 0;
        for (const auto& [l_tag, cnt] : lang_freq) {
            total_l_cnt += cnt;
            if (cnt > max_l_cnt) {
                max_l_cnt = cnt;
                dominant_lang = l_tag;
            }
        }
        if (!dominant_lang.empty() && max_l_cnt >= 10 && max_l_cnt * 10 >= total_l_cnt * 7) {
            size_t n_pos = styles_xml.find("w:styleId=\"" + default_style + "\"");
            if (n_pos != std::string::npos) {
                size_t n_end = styles_xml.find("</w:style>", n_pos);
                if (n_end != std::string::npos) {
                    size_t rpr_pos = styles_xml.find("<w:rPr>", n_pos);
                    if (rpr_pos != std::string::npos && rpr_pos < n_end) {
                        size_t rpr_close = styles_xml.find("</w:rPr>", rpr_pos);
                        if (rpr_close != std::string::npos && rpr_close <= n_end) {
                            std::regex ex_lang(R"rx(<w:lang\b[^>]*/>)rx");
                            std::string rpr_str = styles_xml.substr(rpr_pos + 7, rpr_close - (rpr_pos + 7));
                            if (std::regex_search(rpr_str, ex_lang)) {
                                rpr_str = std::regex_replace(rpr_str, ex_lang, dominant_lang);
                            } else {
                                rpr_str.append(dominant_lang);
                            }
                            styles_xml.replace(rpr_pos + 7, rpr_close - (rpr_pos + 7), rpr_str);
                        }
                    }
                }
            }
        }
    }

    std::regex empty_cs(R"rx(w:cs="")rx");
    std::regex ascii_f(R"rx(w:ascii="([^"]+)")rx");
    std::smatch af_m;
    if (std::regex_search(styles_xml, af_m, ascii_f)) {
        styles_xml = std::regex_replace(styles_xml, empty_cs, "w:cs=\"" + af_m[1].str() + "\"");
    }
    if (XmlOptimizer::is_well_formed(styles_xml)) {
        styles_entry->data.assign(styles_xml.begin(), styles_xml.end());
    }

    std::unordered_map<std::string, std::vector<RprElement>> style_rpr_map;
    s_pos = 0;
    while ((s_pos = styles_xml.find("<w:style ", s_pos)) != std::string::npos) {
        size_t s_tag_close = styles_xml.find('>', s_pos);
        if (s_tag_close == std::string::npos) break;
        std::string_view s_open(styles_xml.data() + s_pos, s_tag_close - s_pos + 1);
        std::string style_id;
        size_t id_pos = s_open.find("w:styleId=\"");
        if (id_pos != std::string_view::npos) {
            size_t v_start = id_pos + 11;
            size_t v_end = s_open.find('"', v_start);
            if (v_end != std::string_view::npos) {
                style_id = std::string(s_open.substr(v_start, v_end - v_start));
            }
        }
        size_t style_end = styles_xml.find("</w:style>", s_tag_close);
        if (style_end == std::string::npos) break;

        size_t ppr_end_in_style = styles_xml.find("</w:pPr>", s_tag_close);
        size_t search_rpr_start = (ppr_end_in_style != std::string::npos && ppr_end_in_style < style_end)
                                  ? ppr_end_in_style + 8 : s_tag_close;
        size_t rpr_start = styles_xml.find("<w:rPr", search_rpr_start);
        if (rpr_start != std::string::npos && rpr_start < style_end) {
            size_t rpr_tag_c = styles_xml.find('>', rpr_start);
            if (rpr_tag_c != std::string::npos && rpr_tag_c < style_end) {
                size_t rpr_end = styles_xml.find("</w:rPr>", rpr_tag_c);
                if (rpr_end != std::string::npos && rpr_end <= style_end) {
                    std::string_view rpr_body(styles_xml.data() + rpr_tag_c + 1, rpr_end - (rpr_tag_c + 1));
                    if (!style_id.empty()) {
                        style_rpr_map[style_id] = parse_rpr_elements(rpr_body);
                    }
                }
            }
        }
        s_pos = style_end + 10;
    }

    auto clean_rpr_str = [&](std::string& target_str, size_t rpr_start, size_t rpr_tag_close, size_t rpr_end, const std::vector<RprElement>& s_rprs) {
        std::string rpr_inner = target_str.substr(rpr_tag_close + 1, rpr_end - (rpr_tag_close + 1));
        auto run_elems = parse_rpr_elements(rpr_inner);
        for (const auto& re : run_elems) {
            bool matches = false;
            std::string clean_tag = re.tag_name.starts_with("w:") ? re.tag_name.substr(2) : re.tag_name;
            for (const auto& se : s_rprs) {
                std::string se_clean_tag = se.tag_name.starts_with("w:") ? se.tag_name.substr(2) : se.tag_name;
                if (se_clean_tag == clean_tag) {
                    if (se.attrs == re.attrs) {
                        matches = true;
                        break;
                    }
                    if (clean_tag == "rFonts") {
                        bool sub_match = true;
                        for (const auto& [rk, rv] : re.attrs) {
                            auto sit = se.attrs.find(rk);
                            if (sit != se.attrs.end()) {
                                if (sit->second != rv) { sub_match = false; break; }
                            } else {
                                auto a_it = se.attrs.find("w:ascii");
                                if (a_it != se.attrs.end() && a_it->second == rv) continue;
                                sub_match = false;
                                break;
                            }
                        }
                        if (sub_match) {
                            matches = true;
                            break;
                        }
                    }
                }
            }

            if (!matches) {
                static const std::unordered_set<std::string> onoff_set = {
                    "b", "i", "caps", "smallCaps", "strike", "dstrike", "outline", "shadow", "emboss", "imprint", "vanish", "webHidden"
                };
                if (onoff_set.contains(clean_tag)) {
                    auto vit = re.attrs.find("w:val");
                    if (vit != re.attrs.end() && (vit->second == "0" || vit->second == "false" || vit->second == "off")) {
                        bool style_has_active = false;
                        for (const auto& se : s_rprs) {
                            std::string se_clean_tag = se.tag_name.starts_with("w:") ? se.tag_name.substr(2) : se.tag_name;
                            if (se_clean_tag == clean_tag) {
                                auto svit = se.attrs.find("w:val");
                                if (svit == se.attrs.end() || svit->second == "1" || svit->second == "true" || svit->second == "on") {
                                    style_has_active = true;
                                    break;
                                }
                            }
                        }
                        if (!style_has_active) {
                            matches = true;
                        }
                    }
                }
            }

            if (matches) {
                size_t found_idx = rpr_inner.find(re.raw_element);
                if (found_idx != std::string::npos) {
                    rpr_inner.erase(found_idx, re.raw_element.size());
                }
            }
        }

        bool only_ws = true;
        for (char ch : rpr_inner) {
            if (ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t') {
                only_ws = false;
                break;
            }
        }

        if (only_ws) {
            target_str.erase(rpr_start, (rpr_end + 8) - rpr_start);
        } else {
            target_str.replace(rpr_tag_close + 1, rpr_end - (rpr_tag_close + 1), rpr_inner);
        }
    };

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        if (name == "word/styles.xml" || name == "word/stylesWithEffects.xml" || name == "word/numbering.xml") continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string xml(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = xml.size();
        std::string rebuilt;
        rebuilt.reserve(xml.size());

        size_t last_p_pos = 0;
        size_t p_pos = 0;
        while ((p_pos = xml.find("<w:p", p_pos)) != std::string::npos) {
            if (p_pos + 4 < xml.size()) {
                char c = xml[p_pos + 4];
                if (c != ' ' && c != '>' && c != '\r' && c != '\n' && c != '\t') {
                    p_pos += 4;
                    continue;
                }
            }
            size_t p_tag_close = xml.find('>', p_pos);
            if (p_tag_close == std::string::npos) break;
            if (xml[p_tag_close - 1] == '/') {
                p_pos = p_tag_close + 1;
                continue;
            }
            size_t p_end = xml.find("</w:p>", p_tag_close);
            if (p_end == std::string::npos) break;
            p_end += 6;

            std::string p_content = xml.substr(p_pos, p_end - p_pos);
            std::string effective_style = default_style;
            std::regex pstyle_regex(R"rx(<w:pStyle\b[^>]*\bw:val="([^"]+)"[^>]*/>)rx");
            std::smatch pstyle_m;
            if (std::regex_search(p_content, pstyle_m, pstyle_regex)) {
                effective_style = pstyle_m[1].str();
            }

            if (style_rpr_map.contains(effective_style)) {
                const auto& style_rprs = style_rpr_map[effective_style];

                size_t ppr_s = p_content.find("<w:pPr");
                if (ppr_s != std::string::npos) {
                    size_t ppr_e = p_content.find("</w:pPr>", ppr_s);
                    if (ppr_e != std::string::npos) {
                        size_t prpr_s = p_content.find("<w:rPr", ppr_s);
                        if (prpr_s != std::string::npos && prpr_s < ppr_e) {
                            size_t prpr_c = p_content.find('>', prpr_s);
                            if (prpr_c != std::string::npos && prpr_c < ppr_e) {
                                if (p_content[prpr_c - 1] == '/') {
                                    p_content.erase(prpr_s, prpr_c - prpr_s + 1);
                                } else {
                                    size_t prpr_end = p_content.find("</w:rPr>", prpr_c);
                                    if (prpr_end != std::string::npos && prpr_end <= ppr_e) {
                                        clean_rpr_str(p_content, prpr_s, prpr_c, prpr_end, style_rprs);
                                    }
                                }
                            }
                        }
                    }
                }

                std::string p_rebuilt;
                p_rebuilt.reserve(p_content.size());
                size_t last_r_pos = 0;
                size_t r_pos = 0;

                while ((r_pos = p_content.find("<w:r", r_pos)) != std::string::npos) {
                    if (r_pos + 4 < p_content.size()) {
                        char c = p_content[r_pos + 4];
                        if (c != ' ' && c != '>' && c != '\r' && c != '\n' && c != '\t') {
                            r_pos += 4;
                            continue;
                        }
                    }
                    size_t r_tag_close = p_content.find('>', r_pos);
                    if (r_tag_close == std::string::npos) break;
                    if (p_content[r_tag_close - 1] == '/') {
                        r_pos = r_tag_close + 1;
                        continue;
                    }
                    size_t r_end = p_content.find("</w:r>", r_tag_close);
                    if (r_end == std::string::npos) break;
                    r_end += 6;

                    std::string r_content = p_content.substr(r_pos, r_end - r_pos);
                    size_t rpr_start = r_content.find("<w:rPr");
                    if (rpr_start != std::string::npos) {
                        size_t rpr_tag_close = r_content.find('>', rpr_start);
                        if (rpr_tag_close != std::string::npos && rpr_tag_close < r_content.size()) {
                            if (r_content[rpr_tag_close - 1] == '/') {
                                r_content.erase(rpr_start, rpr_tag_close - rpr_start + 1);
                            } else {
                                size_t rpr_end = r_content.find("</w:rPr>", rpr_tag_close);
                                if (rpr_end != std::string::npos) {
                                    clean_rpr_str(r_content, rpr_start, rpr_tag_close, rpr_end, style_rprs);
                                }
                            }
                        }
                    }

                    p_rebuilt.append(p_content.substr(last_r_pos, r_pos - last_r_pos));
                    p_rebuilt.append(r_content);
                    last_r_pos = r_end;
                    r_pos = r_end;
                }

                p_rebuilt.append(p_content.substr(last_r_pos));
                p_content = std::move(p_rebuilt);
            }

            rebuilt.append(xml.substr(last_p_pos, p_pos - last_p_pos));
            rebuilt.append(p_content);
            last_p_pos = p_end;
            p_pos = p_end;
        }

        rebuilt.append(xml.substr(last_p_pos));
        if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
            saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
            entry->data.assign(rebuilt.begin(), rebuilt.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::prune_custom_xml(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::vector<std::string> custom_entries;
    for (const auto& name : archive.entry_names()) {
        if (name.starts_with("customXml/")) {
            custom_entries.push_back(name);
        }
    }
    if (custom_entries.empty()) return false;

    bool has_data_binding = false;
    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string_view content(reinterpret_cast<const char*>(e->data.data()), e->data.size());
        if (content.find("dataBinding") != std::string_view::npos) {
            has_data_binding = true;
            break;
        }
    }

    if (has_data_binding) return false;

    for (const auto& name : custom_entries) {
        const auto* e = archive.get_entry(name);
        if (e) {
            saved_bytes += static_cast<int64_t>(e->data.size());
        }
        archive.remove(name);
    }

    std::regex custom_rel_regex(R"rx(<Relationship[^>]*Target="([^"]*customXml/[^"]*)"[^>]*/>)rx");
    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".rels")) continue;
        auto* rels = archive.get_entry_mut(name);
        if (!rels) continue;
        std::string content(reinterpret_cast<const char*>(rels->data.data()), rels->data.size());
        size_t orig_sz = content.size();
        content = std::regex_replace(content, custom_rel_regex, "");
        if (content.size() != orig_sz) {
            rels->data.assign(content.begin(), content.end());
        }
    }

    auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
    if (ct_entry) {
        std::string content(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
        size_t orig_sz = content.size();
        std::regex custom_ct_regex(R"rx(<Override[^>]*PartName="/customXml/[^"]*"[^>]*/>)rx");
        content = std::regex_replace(content, custom_ct_regex, "");
        if (content.size() != orig_sz) {
            ct_entry->data.assign(content.begin(), content.end());
        }
    }

    return true;
}

bool OoxmlOptimizer::minimize_theme(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* theme_entry = archive.get_entry_mut("word/theme/theme1.xml");
    if (!theme_entry) return false;

    bool uses_fmt_ref = false;
    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        if (name == "word/theme/theme1.xml") continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string_view content(reinterpret_cast<const char*>(e->data.data()), e->data.size());
        if (content.find("<a:fillRef") != std::string_view::npos ||
            content.find("<a:effectRef") != std::string_view::npos ||
            content.find("<a:lnRef") != std::string_view::npos) {
            uses_fmt_ref = true;
            break;
        }
    }
    if (uses_fmt_ref) return false;

    std::string theme_xml(reinterpret_cast<const char*>(theme_entry->data.data()), theme_entry->data.size());
    size_t orig_sz = theme_xml.size();

    size_t fmt_start = theme_xml.find("<a:fmtScheme");
    if (fmt_start == std::string::npos) return false;
    size_t fmt_end = theme_xml.find("</a:fmtScheme>", fmt_start);
    if (fmt_end == std::string::npos) return false;
    fmt_end += 14;

    std::string minimal_fmt =
        "<a:fmtScheme name=\"Office\">"
        "<a:fillStyleLst>"
        "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
        "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
        "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
        "</a:fillStyleLst>"
        "<a:lnStyleLst>"
        "<a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
        "<a:ln w=\"25400\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
        "<a:ln w=\"38100\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
        "</a:lnStyleLst>"
        "<a:effectStyleLst>"
        "<a:effectStyle><a:effectLst/></a:effectStyle>"
        "<a:effectStyle><a:effectLst/></a:effectStyle>"
        "<a:effectStyle><a:effectLst/></a:effectStyle>"
        "</a:effectStyleLst>"
        "<a:bgFillStyleLst>"
        "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
        "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
        "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
        "</a:bgFillStyleLst>"
        "</a:fmtScheme>";

    std::string rebuilt;
    rebuilt.reserve(orig_sz);
    rebuilt.append(theme_xml.substr(0, fmt_start));
    rebuilt.append(minimal_fmt);
    rebuilt.append(theme_xml.substr(fmt_end));

    size_t font_start = rebuilt.find("<a:fontScheme");
    if (font_start != std::string::npos) {
        size_t font_end = rebuilt.find("</a:fontScheme>", font_start);
        if (font_end != std::string::npos) {
            font_end += 15;
            std::string minimal_fonts =
                "<a:fontScheme name=\"Office\">"
                "<a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
                "<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
                "</a:fontScheme>";
            rebuilt.replace(font_start, font_end - font_start, minimal_fonts);
        }
    }

    if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
        saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
        theme_entry->data.assign(rebuilt.begin(), rebuilt.end());
        return true;
    }

    return false;
}

bool OoxmlOptimizer::prune_empty_headers_footers(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::vector<std::string> empty_files;
    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/header") && !name.starts_with("word/footer")) continue;
        if (!name.ends_with(".xml")) continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;

        std::string_view content(reinterpret_cast<const char*>(e->data.data()), e->data.size());
        bool has_substance = false;

        if (content.find("<w:drawing") != std::string_view::npos ||
            content.find("<w:fldSimple") != std::string_view::npos ||
            content.find("<w:fldChar") != std::string_view::npos ||
            content.find("<w:instrText") != std::string_view::npos ||
            content.find("<w:pict") != std::string_view::npos ||
            content.find("<w:object") != std::string_view::npos) {
            has_substance = true;
        } else {
            size_t t_pos = 0;
            while ((t_pos = content.find("<w:t", t_pos)) != std::string_view::npos) {
                size_t t_close = content.find('>', t_pos);
                if (t_close != std::string_view::npos && content[t_close - 1] != '/') {
                    size_t t_end = content.find("</w:t>", t_close);
                    if (t_end != std::string_view::npos) {
                        std::string_view t_text = content.substr(t_close + 1, t_end - (t_close + 1));
                        for (char c : t_text) {
                            if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
                                has_substance = true;
                                break;
                            }
                        }
                    }
                }
                if (has_substance) break;
                t_pos = t_close != std::string_view::npos ? t_close + 1 : t_pos + 4;
            }
        }

        if (!has_substance) {
            empty_files.push_back(name);
        }
    }

    if (empty_files.empty()) return false;

    std::unordered_set<std::string> empty_rel_ids;
    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".rels")) continue;
        auto* rels = archive.get_entry_mut(name);
        if (!rels) continue;

        std::string content(reinterpret_cast<const char*>(rels->data.data()), rels->data.size());
        size_t orig_sz = content.size();

        for (const auto& file_path : empty_files) {
            std::string base_name = file_path.substr(file_path.rfind('/') + 1);
            std::regex rel_regex(R"rx(<Relationship\b[^>]*\bId="([^"]+)"[^>]*\bTarget="([^"]*)"[^>]*/>)rx");
            auto rbegin = std::sregex_iterator(content.begin(), content.end(), rel_regex);
            auto rend = std::sregex_iterator();
            for (auto it = rbegin; it != rend; ++it) {
                std::string target = (*it)[2].str();
                if (target == base_name || target == file_path || target == "/" + file_path) {
                    empty_rel_ids.insert((*it)[1].str());
                }
            }
            std::regex del_regex(R"rx(<Relationship\b[^>]*\bTarget="([^"]*)"[^>]*/>)rx");
            std::string new_content;
            size_t last_pos = 0;
            for (auto it = std::sregex_iterator(content.begin(), content.end(), del_regex); it != std::sregex_iterator(); ++it) {
                std::string target = (*it)[1].str();
                if (target == base_name || target == file_path || target == "/" + file_path) {
                    new_content.append(content.substr(last_pos, it->position() - last_pos));
                    last_pos = it->position() + it->length();
                }
            }
            new_content.append(content.substr(last_pos));
            content = std::move(new_content);
        }

        if (content.size() != orig_sz) {
            rels->data.assign(content.begin(), content.end());
        }
    }

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string content(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = content.size();

        for (const auto& rid : empty_rel_ids) {
            std::regex ref_regex(std::string("<w:(?:headerReference|footerReference)\\b[^>]*\\br:id=\"") + rid + "\"[^>]*/>");
            content = std::regex_replace(content, ref_regex, "");
        }

        if (content.size() != orig_sz && XmlOptimizer::is_well_formed(content)) {
            entry->data.assign(content.begin(), content.end());
        }
    }

    auto* ct_entry = archive.get_entry_mut("[Content_Types].xml");
    if (ct_entry) {
        std::string content(reinterpret_cast<const char*>(ct_entry->data.data()), ct_entry->data.size());
        size_t orig_sz = content.size();
        for (const auto& file_path : empty_files) {
            std::regex ct_regex(R"rx(<Override\b[^>]*\bPartName="/)rx" + file_path + R"rx("[^>]*/>)rx");
            content = std::regex_replace(content, ct_regex, "");
        }
        if (content.size() != orig_sz) {
            ct_entry->data.assign(content.begin(), content.end());
        }
    }

    for (const auto& file_path : empty_files) {
        const auto* e = archive.get_entry(file_path);
        if (e) {
            saved_bytes += static_cast<int64_t>(e->data.size());
        }
        archive.remove(file_path);
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::clean_font_table(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* ft_entry = archive.get_entry_mut("word/fontTable.xml");
    if (!ft_entry) return false;

    std::unordered_set<std::string> ref_fonts;
    std::regex font_ref_regex(R"rx(\bw:(?:ascii|hAnsi|eastAsia|cs)="([^"]+)")rx");

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        if (name == "word/fontTable.xml") continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string content(reinterpret_cast<const char*>(e->data.data()), e->data.size());
        auto fbegin = std::sregex_iterator(content.begin(), content.end(), font_ref_regex);
        auto fend = std::sregex_iterator();
        for (auto it = fbegin; it != fend; ++it) {
            ref_fonts.insert((*it)[1].str());
        }
    }

    std::string ft_xml(reinterpret_cast<const char*>(ft_entry->data.data()), ft_entry->data.size());
    size_t orig_sz = ft_xml.size();

    size_t root_start = ft_xml.find("<w:fonts");
    if (root_start == std::string::npos) return false;
    size_t root_open_end = ft_xml.find('>', root_start);
    if (root_open_end == std::string::npos) return false;

    std::unordered_set<std::string> seen_fonts;
    std::vector<std::string> kept_fonts;

    std::regex font_tag_regex(R"rx(<w:font\b[^>]*\bw:name="([^"]+)"[^>]*>([\s\S]*?)</w:font>)rx");
    auto fbegin = std::sregex_iterator(ft_xml.begin(), ft_xml.end(), font_tag_regex);
    auto fend = std::sregex_iterator();
    for (auto it = fbegin; it != fend; ++it) {
        std::string fname = (*it)[1].str();
        std::string finner = (*it)[2].str();

        if (seen_fonts.contains(fname)) continue;
        seen_fonts.insert(fname);

        if (!ref_fonts.empty() && !ref_fonts.contains(fname)) continue;

        if (finner.find("embed") != std::string::npos) {
            kept_fonts.push_back((*it).str());
        } else {
            std::regex strip_meta(R"rx(<w:(?:panose1|charset|family|pitch|sig)\b[^>]*/>)rx");
            std::string clean_inner = std::regex_replace(finner, strip_meta, "");
            bool only_ws = true;
            for (char c : clean_inner) {
                if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
                    only_ws = false;
                    break;
                }
            }
            if (!only_ws) {
                kept_fonts.push_back("<w:font w:name=\"" + fname + "\">" + clean_inner + "</w:font>");
            } else {
                kept_fonts.push_back("<w:font w:name=\"" + fname + "\"/>");
            }
        }
    }

    std::string rebuilt;
    rebuilt.reserve(orig_sz);
    rebuilt.append(ft_xml.substr(0, root_open_end + 1));
    for (const auto& kf : kept_fonts) rebuilt.append(kf);
    rebuilt.append("</w:fonts>");

    if (rebuilt.size() < orig_sz && XmlOptimizer::is_well_formed(rebuilt)) {
        saved_bytes += static_cast<int64_t>(orig_sz - rebuilt.size());
        ft_entry->data.assign(rebuilt.begin(), rebuilt.end());
        return true;
    }

    return false;
}

bool OoxmlOptimizer::clean_settings(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;
    auto* entry = archive.get_entry_mut("word/settings.xml");
    if (!entry) return false;

    std::string xml(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
    size_t orig_sz = xml.size();

    std::vector<std::regex> strip_regexes = {
        std::regex(R"rx(<w:proofState\b[^>]*/>)rx"),
        std::regex(R"rx(<w:savePreviewPicture\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotShadeFormData\b[^>]*/>)rx"),
        std::regex(R"rx(<w:formsDesign\b[^>]*/>)rx"),
        std::regex(R"rx(<w:linkStyles\b[^>]*/>)rx"),
        std::regex(R"rx(<w14:docId\b[^>]*/>)rx"),
        std::regex(R"rx(<w:useFELayout\b[^>]*/>)rx"),
        std::regex(R"rx(<w:balanceSingleByteDoubleByteWidth\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotLeaveBackslashAlone\b[^>]*/>)rx"),
        std::regex(R"rx(<w:ulTrailSpace\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotExpandShiftReturn\b[^>]*/>)rx"),
        std::regex(R"rx(<w:adjustLineHeightInTable\b[^>]*/>)rx"),
        std::regex(R"rx(<w:applyBreakingRules\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotWrapTextWithPunct\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotUseEastAsianBreakRules\b[^>]*/>)rx"),
        std::regex(R"rx(<w:growAutofit\b[^>]*/>)rx"),
        std::regex(R"rx(<w:useNormalStyleForList\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotUseIndentAsNumberingTabStop\b[^>]*/>)rx"),
        std::regex(R"rx(<w:useAltKinsokuLineBreakRules\b[^>]*/>)rx"),
        std::regex(R"rx(<w:allowSpaceOfSameStyleInTable\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotSuppressIndentation\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotAutofitConstrainedTables\b[^>]*/>)rx"),
        std::regex(R"rx(<w:autofitToFirstFixedWidthCell\b[^>]*/>)rx"),
        std::regex(R"rx(<w:underlineTabInNumList\b[^>]*/>)rx"),
        std::regex(R"rx(<w:displayHangulFixedWidth\b[^>]*/>)rx"),
        std::regex(R"rx(<w:splitPgBreakAndParaMark\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotVertAlignCellWithSp\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotBreakConstrainedForcedTable\b[^>]*/>)rx"),
        std::regex(R"rx(<w:doNotVertAlignInTxbx\b[^>]*/>)rx"),
        std::regex(R"rx(<w:useAnsiKerningPairs\b[^>]*/>)rx"),
        std::regex(R"rx(<w:cachedColBalance\b[^>]*/>)rx")
    };

    for (const auto& r : strip_regexes) {
        xml = std::regex_replace(xml, r, "");
    }

    if (xml.size() < orig_sz && XmlOptimizer::is_well_formed(xml)) {
        saved_bytes += static_cast<int64_t>(orig_sz - xml.size());
        entry->data.assign(xml.begin(), xml.end());
        return true;
    }

    return false;
}

bool OoxmlOptimizer::prune_bookmarks(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::unordered_set<std::string> referenced_anchors;
    std::regex anchor_regex(R"rx(\b(?:anchor|w:anchor)="([^"]+)")rx");
    std::regex field_ref_regex(R"rx((?:REF|PAGEREF|NOTEREF)\s+(?:\\?[a-zA-Z]\s+)*"?([_a-zA-Z0-9]+)"?)rx");

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string content(reinterpret_cast<const char*>(e->data.data()), e->data.size());

        auto abegin = std::sregex_iterator(content.begin(), content.end(), anchor_regex);
        auto aend = std::sregex_iterator();
        for (auto it = abegin; it != aend; ++it) {
            referenced_anchors.insert((*it)[1].str());
        }

        auto fbegin = std::sregex_iterator(content.begin(), content.end(), field_ref_regex);
        auto fend = std::sregex_iterator();
        for (auto it = fbegin; it != fend; ++it) {
            referenced_anchors.insert((*it)[1].str());
        }
    }

    std::unordered_set<std::string> to_remove_ids;
    std::regex bm_start_regex(R"rx(<w:bookmarkStart\b[^>]*/>)rx");

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        const auto* e = archive.get_entry(name);
        if (!e) continue;
        std::string content(reinterpret_cast<const char*>(e->data.data()), e->data.size());

        auto bbegin = std::sregex_iterator(content.begin(), content.end(), bm_start_regex);
        auto bend = std::sregex_iterator();
        for (auto it = bbegin; it != bend; ++it) {
            std::string tag = (*it).str();
            std::regex id_r(R"rx(\bw:id="([^"]+)")rx");
            std::regex name_r(R"rx(\bw:name="([^"]+)")rx");
            std::smatch m_id, m_name;
            if (std::regex_search(tag, m_id, id_r) && std::regex_search(tag, m_name, name_r)) {
                std::string bid = m_id[1].str();
                std::string bname = m_name[1].str();

                if (bname == "_GoBack") {
                    to_remove_ids.insert(bid);
                } else if ((bname.starts_with("_Toc") || bname.starts_with("_Ref") || bname.starts_with("_Hlk")) &&
                           !referenced_anchors.contains(bname)) {
                    to_remove_ids.insert(bid);
                }
            }
        }
    }

    if (to_remove_ids.empty()) return false;

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string content(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = content.size();

        for (const auto& bid : to_remove_ids) {
            std::regex del_start(std::string("<w:bookmarkStart\\b[^>]*\\bw:id=\"") + bid + "\"[^>]*/>");
            std::regex del_end(std::string("<w:bookmarkEnd\\b[^>]*\\bw:id=\"") + bid + "\"[^>]*/>");
            content = std::regex_replace(content, del_start, "");
            content = std::regex_replace(content, del_end, "");
        }

        if (content.size() != orig_sz && XmlOptimizer::is_well_formed(content)) {
            saved_bytes += static_cast<int64_t>(orig_sz - content.size());
            entry->data.assign(content.begin(), content.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::normalize_literals(ZipArchive& archive, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::vector<std::string> onoff_tags = {
        "b", "i", "caps", "smallCaps", "strike", "dstrike", "outline", "shadow",
        "emboss", "imprint", "vanish", "webHidden", "keepNext", "keepLines",
        "pageBreakBefore", "widowControl", "suppressAutoHyphens", "autoHyphenation",
        "cantSplit", "tblHeader", "noWrap", "contextualSpacing", "mirrorIndents",
        "suppressOverlap", "kinsoku", "overflowPunct", "topLinePunct", "autoSpaceDE",
        "autoSpaceDN", "adjustRightInd", "snapToGrid"
    };

    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("word/") || !name.ends_with(".xml")) continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry) continue;

        std::string xml(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        size_t orig_sz = xml.size();

        for (const auto& tag : onoff_tags) {
            std::regex true_r("<w:" + tag + R"rx(\b[^>]*\bw:val="(?:true|1|on)"[^>]*/>)rx");
            xml = std::regex_replace(xml, true_r, "<w:" + tag + "/>");
            std::regex false_r("<w:" + tag + R"rx(\b[^>]*\bw:val="(?:false|off)"[^>]*/>)rx");
            xml = std::regex_replace(xml, false_r, "<w:" + tag + " w:val=\"0\"/>");
        }

        std::regex zero_r(R"rx(\b(w:[a-zA-Z0-9_]+)="(-?0(?:\.0+)?(?:pt|px|dxa)?|00+)")rx");
        xml = std::regex_replace(xml, zero_r, "$1=\"0\"");

        if (xml.size() < orig_sz && XmlOptimizer::is_well_formed(xml)) {
            saved_bytes += static_cast<int64_t>(orig_sz - xml.size());
            entry->data.assign(xml.begin(), xml.end());
        }
    }

    return saved_bytes > 0;
}

bool OoxmlOptimizer::optimize(ZipArchive& archive, DocumentFormat format, OptimizationReport& report) {
    int64_t saved = 0;

    if (remove_thumbnails(archive, saved)) {
        report.savings_by_pass["Thumbnail Removal"] += saved;
    }

    if (clean_metadata(archive, saved)) {
        report.savings_by_pass["Metadata Cleanup"] += saved;
    }

    if (strip_rsids(archive, saved)) {
        report.savings_by_pass["RSID Removal"] += saved;
    }

    if (format == DocumentFormat::Docx || format == DocumentFormat::Docm || format == DocumentFormat::Dotx || format == DocumentFormat::Dotm) {
        if (prune_unused_styles(archive, saved)) {
            report.savings_by_pass["Style Pruning"] += saved;
        }
        if (prune_numbering(archive, saved)) {
            report.savings_by_pass["Numbering Pruning"] += saved;
        }
        if (lift_paragraph_properties(archive, saved)) {
            report.savings_by_pass["Paragraph Properties Lifting"] += saved;
        }
        if (normalize_table_cells(archive, saved)) {
            report.savings_by_pass["Table Normalization"] += saved;
        }
        if (strip_run_properties(archive, saved)) {
            report.savings_by_pass["Run Properties Stripping"] += saved;
        }
        if (prune_custom_xml(archive, saved)) {
            report.savings_by_pass["Custom XML Pruning"] += saved;
        }
        if (minimize_theme(archive, saved)) {
            report.savings_by_pass["Theme Minimization"] += saved;
        }
        if (prune_empty_headers_footers(archive, saved)) {
            report.savings_by_pass["Empty Headers/Footers Pruning"] += saved;
        }
        if (clean_font_table(archive, saved)) {
            report.savings_by_pass["Font Table Cleanup"] += saved;
        }
        if (clean_settings(archive, saved)) {
            report.savings_by_pass["Settings Cleanup"] += saved;
        }
        if (prune_bookmarks(archive, saved)) {
            report.savings_by_pass["Bookmarks Pruning"] += saved;
        }
        if (normalize_literals(archive, saved)) {
            report.savings_by_pass["Literal Normalization"] += saved;
        }
    }

    if (format == DocumentFormat::Xlsx || format == DocumentFormat::Xlsm || format == DocumentFormat::Xltx || format == DocumentFormat::Xltm) {
        if (optimize_xlsx_shared_strings(archive, saved)) {
            report.savings_by_pass["Shared Strings GC"] += saved;
        }
    }

    if (subset_embedded_fonts(archive, saved)) {
        report.savings_by_pass["Font Subsetting"] += saved;
    }

    if (clean_orphan_relationships(archive, saved)) {
        report.savings_by_pass["Orphan Media Pruning"] += saved;
    }

    if (deduplicate_media(archive, saved)) {
        report.savings_by_pass["Media Deduplication"] += saved;
    }

    if (optimize_embedded_packages(archive, saved)) {
        report.savings_by_pass["Embedded Package Optimization"] += saved;
    }

    if (optimize_embedded_images(archive, saved)) {
        report.savings_by_pass["Image Optimization"] += saved;
    }

    if (normalize_drawingml(archive, saved)) {
        report.savings_by_pass["DrawingML Normalization"] += saved;
    }

    if (remove_hidden_objects(archive, saved)) {
        report.savings_by_pass["Hidden Objects Pruning"] += saved;
    }

    if (shorten_media_paths(archive, saved)) {
        report.savings_by_pass["Short Path VFS"] += saved;
    }

    if (normalize_content_types(archive, saved)) {
        report.savings_by_pass["Content Types Normalization"] += saved;
    }

    if (minify_all_xml(archive, saved)) {
        report.savings_by_pass["XML Minification"] += saved;
    }

    return true;
}

}
