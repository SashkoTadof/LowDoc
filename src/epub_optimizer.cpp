#include "lowdoc/epub_optimizer.hpp"
#include "lowdoc/xml_optimizer.hpp"
#include "lowdoc/image_optimizer.hpp"
#include "lowdoc/resource_deduplicator.hpp"
#include <regex>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>

namespace lowdoc {

static std::string minify_css(std::string_view css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    const size_t n = css.size();

    while (i < n) {
        if (i + 1 < n && css[i] == '/' && css[i + 1] == '*') {
            size_t end = css.find("*/", i + 2);
            if (end != std::string_view::npos) {
                i = end + 2;
            } else {
                break;
            }
        } else if (std::isspace(static_cast<unsigned char>(css[i]))) {
            if (!out.empty() && out.back() != '{' && out.back() != '}' && out.back() != ':' && out.back() != ';' && out.back() != ' ') {
                out.push_back(' ');
            }
            i++;
        } else {
            out.push_back(css[i]);
            i++;
        }
    }
    return out;
}

bool EpubOptimizer::optimize(ZipArchive& archive, OptimizationReport& report) {
    int64_t orphan_saved = 0;
    std::string opf_name;
    for (const auto& name : archive.entry_names()) {
        if (name.ends_with(".opf")) {
            opf_name = name;
            break;
        }
    }

    if (!opf_name.empty()) {
        auto* opf_entry = archive.get_entry_mut(opf_name);
        if (opf_entry) {
            std::string opf_text(reinterpret_cast<const char*>(opf_entry->data.data()), opf_entry->data.size());
            std::filesystem::path opf_p(opf_name);
            std::string base_dir = opf_p.has_parent_path() ? opf_p.parent_path().generic_string() : "";

            struct ManifestItem {
                std::string id;
                std::string href;
                std::string media_type;
                std::string full_path;
            };

            std::vector<ManifestItem> manifest_items;
            std::unordered_set<std::string> manifest_full_paths;

            std::regex item_regex(R"rx(<item\b([^>]+)/>)rx");
            std::regex id_regex(R"rx(id="([^"]+)")rx");
            std::regex href_regex(R"rx(href="([^"]+)")rx");
            std::regex type_regex(R"rx(media-type="([^"]+)")rx");

            auto it_b = std::sregex_iterator(opf_text.begin(), opf_text.end(), item_regex);
            auto it_e = std::sregex_iterator();
            for (auto it = it_b; it != it_e; ++it) {
                std::string it_str = (*it)[1].str();
                std::smatch m_id, m_href, m_type;
                if (std::regex_search(it_str, m_id, id_regex) &&
                    std::regex_search(it_str, m_href, href_regex) &&
                    std::regex_search(it_str, m_type, type_regex)) {
                    std::string id_val = m_id[1].str();
                    std::string href_val = m_href[1].str();
                    std::string type_val = m_type[1].str();

                    std::filesystem::path full_p = base_dir.empty() ? std::filesystem::path(href_val) : (std::filesystem::path(base_dir) / href_val);
                    std::string norm_full = full_p.lexically_normal().generic_string();

                    manifest_items.push_back({id_val, href_val, type_val, norm_full});
                    manifest_full_paths.insert(norm_full);
                }
            }

            std::unordered_set<std::string> used_paths;
            std::unordered_set<std::string> used_filenames;
            for (const auto& ename : archive.entry_names()) {
                if (ename.ends_with(".xhtml") || ename.ends_with(".html") || ename.ends_with(".css") || ename.ends_with(".ncx")) {
                    const auto* e = archive.get_entry(ename);
                    if (!e) continue;
                    std::string_view sv(reinterpret_cast<const char*>(e->data.data()), e->data.size());
                    std::filesystem::path ep(ename);
                    std::string edir = ep.has_parent_path() ? ep.parent_path().generic_string() : "";

                    std::regex ref_regex(R"rx((?:src|href)="([^"]+)")rx");
                    auto rit_b = std::cregex_iterator(sv.data(), sv.data() + sv.size(), ref_regex);
                    auto rit_e = std::cregex_iterator();
                    for (auto rit = rit_b; rit != rit_e; ++rit) {
                        std::string ref_val = (*rit)[1].str();
                        size_t hash_pos = ref_val.find('#');
                        if (hash_pos != std::string::npos) ref_val = ref_val.substr(0, hash_pos);
                        if (!ref_val.empty() && !ref_val.starts_with("http://") && !ref_val.starts_with("https://")) {
                            std::filesystem::path rp = edir.empty() ? std::filesystem::path(ref_val) : (std::filesystem::path(edir) / ref_val);
                            std::string rp_norm = rp.lexically_normal().generic_string();
                            used_paths.insert(rp_norm);
                            used_filenames.insert(rp.filename().generic_string());
                        }
                    }
                }
            }

            std::vector<std::string> to_remove_paths;
            std::vector<std::string> to_remove_ids;
            for (const auto& mitem : manifest_items) {
                if (mitem.media_type.starts_with("image/")) {
                    std::filesystem::path hp(mitem.href);
                    std::string hfname = hp.filename().generic_string();
                    if (!used_paths.contains(mitem.full_path) && !used_filenames.contains(hfname)) {
                        to_remove_paths.push_back(mitem.full_path);
                        to_remove_ids.push_back(mitem.id);
                    }
                }
            }

            for (const auto& rem_p : to_remove_paths) {
                const auto* re = archive.get_entry(rem_p);
                if (re) {
                    orphan_saved += static_cast<int64_t>(re->data.size());
                    archive.remove(rem_p);
                }
            }

            if (!to_remove_ids.empty()) {
                for (const auto& rem_id : to_remove_ids) {
                    std::regex rem_item(R"rx(<item\b[^>]*\bid=")rx" + rem_id + R"rx("[^>]*/>\s*)rx");
                    opf_text = std::regex_replace(opf_text, rem_item, "");
                }
                if (XmlOptimizer::is_well_formed(opf_text)) {
                    opf_entry->data.assign(opf_text.begin(), opf_text.end());
                }
            }

            std::vector<std::string> stray_files;
            for (const auto& ename : archive.entry_names()) {
                if (ename != "mimetype" && ename != "META-INF/container.xml" && ename != opf_name && !manifest_full_paths.contains(ename)) {
                    stray_files.push_back(ename);
                }
            }
            for (const auto& stray : stray_files) {
                const auto* se = archive.get_entry(stray);
                if (se) {
                    orphan_saved += static_cast<int64_t>(se->data.size());
                    archive.remove(stray);
                }
            }

            if (orphan_saved > 0) {
                report.savings_by_pass["Orphan Asset Pruning"] += orphan_saved;
            }
        }
    }

    int64_t dedup_saved = 0;
    if (ResourceDeduplicator::deduplicate_archive_media(archive, DocumentFormat::Epub, dedup_saved)) {
        report.savings_by_pass["Media Deduplication"] += dedup_saved;
    }

    int64_t img_saved = 0;
    for (const auto& name : archive.entry_names()) {
        std::string ext = std::filesystem::path(name).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".svg") {
            auto entry = archive.get_entry_mut(name);
            if (!entry) continue;

            size_t orig_sz = entry->data.size();
            std::vector<uint8_t> opt_img;
            if (ImageOptimizer::optimize_image(entry->data, ext, opt_img)) {
                if (opt_img.size() < orig_sz) {
                    img_saved += static_cast<int64_t>(orig_sz - opt_img.size());
                    entry->data = std::move(opt_img);
                }
            }
        }
    }
    if (img_saved > 0) {
        report.savings_by_pass["Image Optimization"] += img_saved;
    }

    int64_t xml_saved = 0;
    for (const auto& name : archive.entry_names()) {
        std::string ext = std::filesystem::path(name).extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ext == ".xml" || ext == ".opf" || ext == ".ncx" || ext == ".xhtml" || ext == ".html") {
            auto entry = archive.get_entry_mut(name);
            if (!entry) continue;

            size_t orig_sz = entry->data.size();
            std::string_view sv(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
            std::string minified = XmlOptimizer::minify(sv);

            if (minified.size() < orig_sz && XmlOptimizer::is_well_formed(minified)) {
                xml_saved += static_cast<int64_t>(orig_sz - minified.size());
                entry->data.assign(minified.begin(), minified.end());
            }
        } else if (ext == ".css") {
            auto entry = archive.get_entry_mut(name);
            if (!entry) continue;

            size_t orig_sz = entry->data.size();
            std::string_view sv(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
            std::string minified = minify_css(sv);

            if (minified.size() < orig_sz) {
                xml_saved += static_cast<int64_t>(orig_sz - minified.size());
                entry->data.assign(minified.begin(), minified.end());
            }
        }
    }
    if (xml_saved > 0) {
        report.savings_by_pass["Markup Minification"] += xml_saved;
    }

    return true;
}

}
