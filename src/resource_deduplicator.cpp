#include "lowdoc/resource_deduplicator.hpp"
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <cstring>
#include <regex>

namespace lowdoc {

static uint64_t compute_fast_hash(const std::vector<uint8_t>& data) {
    uint64_t h = 14695981039346656037ULL;
    for (uint8_t b : data) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

bool ResourceDeduplicator::deduplicate_archive_media(ZipArchive& archive, DocumentFormat, int64_t& saved_bytes) {
    saved_bytes = 0;

    std::vector<std::string> media_entries;
    for (const auto& name : archive.entry_names()) {
        if (name.find("/media/") != std::string::npos ||
            name.starts_with("Pictures/") ||
            name.find("/images/") != std::string::npos ||
            name.starts_with("images/")) {
            media_entries.push_back(name);
        }
    }

    if (media_entries.size() < 2) return false;

    std::unordered_map<uint64_t, std::vector<std::string>> buckets;
    for (const auto& name : media_entries) {
        const auto* entry = archive.get_entry(name);
        if (entry && !entry->data.empty()) {
            uint64_t h = compute_fast_hash(entry->data);
            buckets[h].push_back(name);
        }
    }

    std::vector<std::pair<std::string, std::string>> duplicates;
    for (const auto& [h, names] : buckets) {
        if (names.size() < 2) continue;

        for (size_t i = 0; i < names.size(); ++i) {
            const auto* entry_i = archive.get_entry(names[i]);
            if (!entry_i) continue;

            for (size_t j = i + 1; j < names.size(); ++j) {
                const auto* entry_j = archive.get_entry(names[j]);
                if (!entry_j) continue;

                if (entry_i->data.size() == entry_j->data.size() &&
                    std::memcmp(entry_i->data.data(), entry_j->data.data(), entry_i->data.size()) == 0) {
                    duplicates.emplace_back(names[j], names[i]);
                }
            }
        }
    }

    if (duplicates.empty()) return false;

    for (const auto& [dup_name, canon_name] : duplicates) {
        std::string dup_filename = std::filesystem::path(dup_name).filename().string();
        std::string canon_filename = std::filesystem::path(canon_name).filename().string();

        for (const auto& name : archive.entry_names()) {
            if (name.ends_with(".rels")) {
                auto rels_entry = archive.get_entry_mut(name);
                if (rels_entry) {
                    std::string content(reinterpret_cast<const char*>(rels_entry->data.data()), rels_entry->data.size());
                    std::string target_pattern = "Target=\"media/" + dup_filename + "\"";
                    std::string target_replace = "Target=\"media/" + canon_filename + "\"";
                    size_t pos = 0;
                    bool modified = false;
                    while ((pos = content.find(target_pattern, pos)) != std::string::npos) {
                        content.replace(pos, target_pattern.length(), target_replace);
                        pos += target_replace.length();
                        modified = true;
                    }
                    if (modified) {
                        rels_entry->data.assign(content.begin(), content.end());
                    }
                }
            } else if (name == "content.xml" || name == "styles.xml" || name.ends_with(".xhtml") || name.ends_with(".html")) {
                auto doc_entry = archive.get_entry_mut(name);
                if (doc_entry) {
                    std::string content(reinterpret_cast<const char*>(doc_entry->data.data()), doc_entry->data.size());
                    std::string pat1 = "xlink:href=\"" + dup_name + "\"";
                    std::string rep1 = "xlink:href=\"" + canon_name + "\"";
                    std::string pat2 = "src=\"" + dup_name + "\"";
                    std::string rep2 = "src=\"" + canon_name + "\"";
                    bool modified = false;
                    size_t pos = 0;
                    while ((pos = content.find(pat1, pos)) != std::string::npos) {
                        content.replace(pos, pat1.length(), rep1);
                        pos += rep1.length();
                        modified = true;
                    }
                    pos = 0;
                    while ((pos = content.find(pat2, pos)) != std::string::npos) {
                        content.replace(pos, pat2.length(), rep2);
                        pos += rep2.length();
                        modified = true;
                    }
                    if (modified) {
                        doc_entry->data.assign(content.begin(), content.end());
                    }
                }
            } else if (name == "META-INF/manifest.xml") {
                auto man_entry = archive.get_entry_mut(name);
                if (man_entry) {
                    std::string content(reinterpret_cast<const char*>(man_entry->data.data()), man_entry->data.size());
                    std::string pat = "<manifest:file-entry manifest:full-path=\"" + dup_name + "\"";
                    size_t p = content.find(pat);
                    if (p != std::string::npos) {
                        size_t end = content.find("/>", p);
                        if (end != std::string::npos) {
                            content.erase(p, end + 2 - p);
                            man_entry->data.assign(content.begin(), content.end());
                        }
                    }
                }
            }
        }

        const auto* dup_entry = archive.get_entry(dup_name);
        if (dup_entry) {
            saved_bytes += static_cast<int64_t>(dup_entry->data.size());
            archive.remove(dup_name);
        }
    }

    return saved_bytes > 0;
}

}
