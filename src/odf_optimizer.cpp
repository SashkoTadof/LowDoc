#include "lowdoc/odf_optimizer.hpp"
#include "lowdoc/xml_optimizer.hpp"
#include "lowdoc/image_optimizer.hpp"
#include "lowdoc/resource_deduplicator.hpp"
#include "lowdoc/ooxml_optimizer.hpp"
#include <regex>
#include <filesystem>

namespace lowdoc {

bool OdfOptimizer::optimize(ZipArchive& archive, DocumentFormat format, OptimizationReport& report) {
    int64_t meta_saved = 0;
    auto meta_entry = archive.get_entry_mut("meta.xml");
    if (meta_entry) {
        size_t orig_sz = meta_entry->data.size();
        std::string meta_str(reinterpret_cast<const char*>(meta_entry->data.data()), meta_entry->data.size());

        std::vector<std::regex> strip_tags = {
            std::regex(R"(<meta:generator>[^<]*</meta:generator>)"),
            std::regex(R"(<meta:initial-creator>[^<]*</meta:initial-creator>)"),
            std::regex(R"(<dc:creator>[^<]*</dc:creator>)"),
            std::regex(R"(<meta:creation-date>[^<]*</meta:creation-date>)"),
            std::regex(R"(<dc:date>[^<]*</dc:date>)"),
            std::regex(R"(<meta:editing-cycles>[^<]*</meta:editing-cycles>)"),
            std::regex(R"(<meta:editing-duration>[^<]*</meta:editing-duration>)"),
            std::regex(R"(<meta:document-statistic\b[^>]*/>)")
        };

        for (const auto& r : strip_tags) {
            meta_str = std::regex_replace(meta_str, r, "");
        }

        if (XmlOptimizer::is_well_formed(meta_str)) {
            meta_entry->data.assign(meta_str.begin(), meta_str.end());
            if (meta_entry->data.size() < orig_sz) {
                meta_saved = static_cast<int64_t>(orig_sz - meta_entry->data.size());
                report.savings_by_pass["Metadata Cleanup"] += meta_saved;
            }
        }
    }

    int64_t thumb_saved = 0;
    const auto* th = archive.get_entry("Thumbnails/thumbnail.png");
    if (th) {
        thumb_saved = static_cast<int64_t>(th->data.size());
        archive.remove("Thumbnails/thumbnail.png");
        report.savings_by_pass["Thumbnail Removal"] += thumb_saved;

        auto manifest_entry = archive.get_entry_mut("META-INF/manifest.xml");
        if (manifest_entry) {
            std::string man_str(reinterpret_cast<const char*>(manifest_entry->data.data()), manifest_entry->data.size());
            std::regex thumb_manifest_regex(R"(<manifest:file-entry\b[^>]*manifest:full-path="Thumbnails/thumbnail\.png"[^>]*/>)");
            std::string cleaned_man = std::regex_replace(man_str, thumb_manifest_regex, "");
            if (XmlOptimizer::is_well_formed(cleaned_man)) {
                manifest_entry->data.assign(cleaned_man.begin(), cleaned_man.end());
            }
        }
    }

    std::vector<std::string> empty_configs;
    for (const auto& name : archive.entry_names()) {
        if (name.starts_with("Configurations2/") && name.ends_with("/")) {
            const auto* e = archive.get_entry(name);
            if (e && e->data.empty()) {
                empty_configs.push_back(name);
            }
        }
    }
    for (const auto& name : empty_configs) {
        archive.remove(name);
    }

    int64_t hidden_saved = 0;
    auto content_entry = archive.get_entry_mut("content.xml");
    if (content_entry) {
        size_t orig_sz = content_entry->data.size();
        std::string content_str(reinterpret_cast<const char*>(content_entry->data.data()), content_entry->data.size());
        std::vector<std::regex> hidden_odf = {
            std::regex(R"(<draw:frame\b[^>]*svg:width="0cm"[^>]*svg:height="0cm"[^>]*/>)"),
            std::regex(R"(<draw:frame\b[^>]*svg:width="0cm"[^>]*svg:height="0cm"[^>]*>.*?</draw:frame>)"),
            std::regex(R"(<office:annotation\b[^>]*>\s*</office:annotation>)")
        };
        for (const auto& r : hidden_odf) {
            content_str = std::regex_replace(content_str, r, "");
        }
        if (content_str.size() < orig_sz && XmlOptimizer::is_well_formed(content_str)) {
            hidden_saved += static_cast<int64_t>(orig_sz - content_str.size());
            content_entry->data.assign(content_str.begin(), content_str.end());
            report.savings_by_pass["Hidden Objects Pruning"] += hidden_saved;
        }
    }

    int64_t dedup_saved = 0;
    if (ResourceDeduplicator::deduplicate_archive_media(archive, format, dedup_saved)) {
        report.savings_by_pass["Media Deduplication"] += dedup_saved;
    }

    int64_t img_saved = 0;
    for (const auto& name : archive.entry_names()) {
        if (!name.starts_with("Pictures/")) continue;
        std::string ext = std::filesystem::path(name).extension().string();
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
    if (img_saved > 0) {
        report.savings_by_pass["Image Optimization"] += img_saved;
    }

    int64_t xml_saved = 0;
    for (const auto& name : archive.entry_names()) {
        if (!name.ends_with(".xml")) continue;
        auto entry = archive.get_entry_mut(name);
        if (!entry) continue;

        size_t orig_sz = entry->data.size();
        std::string cur(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
        std::string dedup_ns = XmlOptimizer::deduplicate_namespaces(cur);
        if (dedup_ns.size() <= cur.size() && XmlOptimizer::is_well_formed(dedup_ns)) {
            cur = std::move(dedup_ns);
        }
        std::string minified = XmlOptimizer::minify(cur);
        if (minified.size() <= cur.size() && XmlOptimizer::is_well_formed(minified)) {
            cur = std::move(minified);
        }

        if (cur.size() < orig_sz && XmlOptimizer::is_well_formed(cur)) {
            xml_saved += static_cast<int64_t>(orig_sz - cur.size());
            entry->data.assign(cur.begin(), cur.end());
        }
    }
    if (xml_saved > 0) {
        report.savings_by_pass["XML Minification"] += xml_saved;
    }

    int64_t emb_saved = 0;
    for (const auto& name : archive.entry_names()) {
        if (name == "mimetype") continue;
        auto* entry = archive.get_entry_mut(name);
        if (!entry || entry->data.size() < 30) continue;
        if (entry->data[0] == 'P' && entry->data[1] == 'K' && entry->data[2] == 0x03 && entry->data[3] == 0x04) {
            ZipArchive sub_archive;
            if (ZipArchive::read(entry->data, sub_archive)) {
                OptimizationReport sub_rep;
                bool sub_ok = false;
                for (const auto& ename : sub_archive.entry_names()) {
                    if (ename.starts_with("xl/")) { sub_ok = OoxmlOptimizer::optimize(sub_archive, DocumentFormat::Xlsx, sub_rep); break; }
                    if (ename.starts_with("word/")) { sub_ok = OoxmlOptimizer::optimize(sub_archive, DocumentFormat::Docx, sub_rep); break; }
                    if (ename.starts_with("ppt/")) { sub_ok = OoxmlOptimizer::optimize(sub_archive, DocumentFormat::Pptx, sub_rep); break; }
                }
                std::vector<uint8_t> opt_sub;
                if (sub_archive.write(opt_sub, true) && opt_sub.size() < entry->data.size()) {
                    emb_saved += static_cast<int64_t>(entry->data.size() - opt_sub.size());
                    entry->data = std::move(opt_sub);
                }
            }
        }
    }
    if (emb_saved > 0) {
        report.savings_by_pass["Embedded Package Optimization"] += emb_saved;
    }

    return true;
}

}
