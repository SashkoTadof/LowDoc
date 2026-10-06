#include "lowdoc/zip_archive.hpp"
#include "lowdoc/deflate.hpp"
#include <algorithm>
#include <cstring>
#include <future>
#include <vector>

namespace lowdoc {

#pragma pack(push, 1)
struct ZipLocalHeader {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
};

struct ZipCentralHeader {
    uint32_t signature;
    uint16_t version_made_by;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint16_t disk_start;
    uint16_t internal_attr;
    uint32_t external_attr;
    uint32_t local_header_offset;
};

struct ZipEOCD {
    uint32_t signature;
    uint16_t disk_number;
    uint16_t cd_start_disk;
    uint16_t total_entries_disk;
    uint16_t total_entries;
    uint32_t cd_size;
    uint32_t cd_offset;
    uint16_t comment_len;
};
#pragma pack(pop)

void ZipArchive::rebuild_index() {
    index_map_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        index_map_[entries_[i].name] = i;
    }
}

bool ZipArchive::has_entry(const std::string& name) const {
    return index_map_.find(name) != index_map_.end();
}

const ZipEntry* ZipArchive::get_entry(const std::string& name) const {
    auto it = index_map_.find(name);
    if (it == index_map_.end()) return nullptr;
    return &entries_[it->second];
}

ZipEntry* ZipArchive::get_entry_mut(const std::string& name) {
    auto it = index_map_.find(name);
    if (it == index_map_.end()) return nullptr;
    return &entries_[it->second];
}

void ZipArchive::add_or_replace(const ZipEntry& entry) {
    auto it = index_map_.find(entry.name);
    if (it != index_map_.end()) {
        entries_[it->second] = entry;
    } else {
        entries_.push_back(entry);
        rebuild_index();
    }
}

bool ZipArchive::remove(const std::string& name) {
    auto it = index_map_.find(name);
    if (it == index_map_.end()) return false;
    entries_.erase(entries_.begin() + it->second);
    rebuild_index();
    return true;
}

std::vector<std::string> ZipArchive::entry_names() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& e : entries_) {
        names.push_back(e.name);
    }
    return names;
}

bool ZipArchive::read(std::span<const uint8_t> bytes, ZipArchive& archive) {
    archive.entries_.clear();
    archive.index_map_.clear();

    if (bytes.size() < sizeof(ZipEOCD)) return false;

    size_t max_search = std::min<size_t>(bytes.size(), 65536 + sizeof(ZipEOCD));
    size_t search_start = bytes.size() - max_search;
    size_t eocd_pos = std::string::npos;

    for (size_t pos = bytes.size() - sizeof(ZipEOCD); pos >= search_start; --pos) {
        if (bytes[pos] == 0x50 && bytes[pos + 1] == 0x4B && bytes[pos + 2] == 0x05 && bytes[pos + 3] == 0x06) {
            eocd_pos = pos;
            break;
        }
        if (pos == search_start) break;
    }

    if (eocd_pos == std::string::npos) return false;

    ZipEOCD eocd;
    std::memcpy(&eocd, bytes.data() + eocd_pos, sizeof(ZipEOCD));

    size_t cd_offset = eocd.cd_offset;
    size_t cd_entries = eocd.total_entries;

    if (cd_offset + eocd.cd_size > bytes.size()) return false;

    size_t current_cd_pos = cd_offset;
    for (size_t i = 0; i < cd_entries; ++i) {
        if (current_cd_pos + sizeof(ZipCentralHeader) > bytes.size()) return false;

        ZipCentralHeader cd_hdr;
        std::memcpy(&cd_hdr, bytes.data() + current_cd_pos, sizeof(ZipCentralHeader));
        if (cd_hdr.signature != 0x02014B50) return false;

        current_cd_pos += sizeof(ZipCentralHeader);
        if (current_cd_pos + cd_hdr.filename_len + cd_hdr.extra_len + cd_hdr.comment_len > bytes.size()) return false;

        std::string filename(reinterpret_cast<const char*>(bytes.data() + current_cd_pos), cd_hdr.filename_len);
        current_cd_pos += cd_hdr.filename_len + cd_hdr.extra_len + cd_hdr.comment_len;

        if (cd_hdr.local_header_offset + sizeof(ZipLocalHeader) > bytes.size()) return false;

        ZipLocalHeader local_hdr;
        std::memcpy(&local_hdr, bytes.data() + cd_hdr.local_header_offset, sizeof(ZipLocalHeader));
        if (local_hdr.signature != 0x04034B50) return false;

        size_t data_pos = cd_hdr.local_header_offset + sizeof(ZipLocalHeader) + local_hdr.filename_len + local_hdr.extra_len;
        if (data_pos + cd_hdr.compressed_size > bytes.size()) return false;

        std::span<const uint8_t> comp_data(bytes.data() + data_pos, cd_hdr.compressed_size);

        ZipEntry entry;
        entry.name = filename;
        entry.compression_method = cd_hdr.compression_method;
        entry.external_attributes = cd_hdr.external_attr;
        entry.last_mod_time = cd_hdr.last_mod_time;
        entry.last_mod_date = cd_hdr.last_mod_date;

        if (cd_hdr.compression_method == 0) {
            entry.data.assign(comp_data.begin(), comp_data.end());
            entry.force_store = true;
        } else if (cd_hdr.compression_method == 8) {
            if (!DeflateEngine::decompress(comp_data, entry.data, cd_hdr.uncompressed_size)) {
                return false;
            }
        } else {
            return false;
        }

        uint32_t actual_crc = DeflateEngine::crc32(entry.data);
        if (actual_crc != cd_hdr.crc32) {
            return false;
        }

        archive.entries_.push_back(std::move(entry));
    }

    archive.rebuild_index();
    return true;
}

bool ZipArchive::write(std::vector<uint8_t>& output, bool aggressive_deflate) const {
    output.clear();

    struct WrittenEntryInfo {
        std::string name;
        uint32_t crc32;
        uint32_t compressed_size;
        uint32_t uncompressed_size;
        uint16_t compression_method;
        uint32_t local_offset;
        uint32_t external_attr;
    };

    std::vector<WrittenEntryInfo> written_infos;
    written_infos.reserve(entries_.size());

    std::vector<const ZipEntry*> ordered_entries;
    ordered_entries.reserve(entries_.size());

    const ZipEntry* mimetype_entry = nullptr;
    for (const auto& e : entries_) {
        if (e.name == "mimetype") {
            mimetype_entry = &e;
        } else {
            ordered_entries.push_back(&e);
        }
    }

    std::stable_sort(ordered_entries.begin(), ordered_entries.end(), [](const ZipEntry* a, const ZipEntry* b) {
        if (a->name == "[Content_Types].xml") return true;
        if (b->name == "[Content_Types].xml") return false;
        auto get_ext = [](std::string_view name) {
            size_t dot = name.rfind('.');
            return (dot != std::string_view::npos) ? name.substr(dot) : std::string_view{};
        };
        auto ext_a = get_ext(a->name);
        auto ext_b = get_ext(b->name);
        if (ext_a != ext_b) return ext_a < ext_b;
        return a->name < b->name;
    });

    if (mimetype_entry) {
        ordered_entries.insert(ordered_entries.begin(), mimetype_entry);
    }

    struct PreparedEntry {
        uint32_t crc = 0;
        uint32_t uncomp_size = 0;
        uint16_t method = 0;
        std::vector<uint8_t> compressed_data;
    };

    std::vector<PreparedEntry> prepared(ordered_entries.size());
    std::vector<std::future<void>> tasks;
    tasks.reserve(ordered_entries.size());

    for (size_t idx = 0; idx < ordered_entries.size(); ++idx) {
        tasks.push_back(std::async(std::launch::async, [&, idx]() {
            const auto* e = ordered_entries[idx];
            auto& prep = prepared[idx];
            prep.crc = DeflateEngine::crc32(e->data);
            prep.uncomp_size = static_cast<uint32_t>(e->data.size());

            if (e->name == "mimetype" || e->force_store) {
                prep.method = 0;
                prep.compressed_data.assign(e->data.begin(), e->data.end());
            } else {
                if (aggressive_deflate) {
                    prep.compressed_data = DeflateEngine::compress_best(e->data);
                } else {
                    prep.compressed_data = DeflateEngine::compress(e->data);
                }

                if (prep.compressed_data.size() < e->data.size()) {
                    std::vector<uint8_t> roundtrip;
                    if (DeflateEngine::decompress(prep.compressed_data, roundtrip, e->data.size()) &&
                        roundtrip.size() == e->data.size() &&
                        std::memcmp(roundtrip.data(), e->data.data(), e->data.size()) == 0) {
                        prep.method = 8;
                    } else {
                        prep.method = 0;
                        prep.compressed_data.assign(e->data.begin(), e->data.end());
                    }
                } else {
                    prep.method = 0;
                    prep.compressed_data.assign(e->data.begin(), e->data.end());
                }
            }
        }));
    }

    for (auto& t : tasks) {
        t.get();
    }

    for (size_t idx = 0; idx < ordered_entries.size(); ++idx) {
        const auto* e = ordered_entries[idx];
        const auto& prep = prepared[idx];
        uint32_t crc = prep.crc;
        uint32_t uncomp_size = prep.uncomp_size;
        uint16_t method = prep.method;
        const auto& compressed_data = prep.compressed_data;

        uint32_t local_offset = static_cast<uint32_t>(output.size());

        ZipLocalHeader loc{};
        loc.signature = 0x04034B50;
        loc.version_needed = (method == 8) ? 20 : 10;
        loc.flags = 0;
        loc.compression_method = method;
        loc.last_mod_time = 0;
        loc.last_mod_date = 0x0021;
        loc.crc32 = crc;
        loc.compressed_size = static_cast<uint32_t>(compressed_data.size());
        loc.uncompressed_size = uncomp_size;
        loc.filename_len = static_cast<uint16_t>(e->name.size());
        loc.extra_len = 0;

        const uint8_t* loc_bytes = reinterpret_cast<const uint8_t*>(&loc);
        output.insert(output.end(), loc_bytes, loc_bytes + sizeof(ZipLocalHeader));
        output.insert(output.end(), e->name.begin(), e->name.end());
        output.insert(output.end(), compressed_data.begin(), compressed_data.end());

        written_infos.push_back({
            e->name,
            crc,
            static_cast<uint32_t>(compressed_data.size()),
            uncomp_size,
            method,
            local_offset,
            e->external_attributes
        });
    }

    uint32_t cd_start_offset = static_cast<uint32_t>(output.size());

    for (const auto& wi : written_infos) {
        ZipCentralHeader cd{};
        cd.signature = 0x02014B50;
        cd.version_made_by = 0x031E;
        cd.version_needed = (wi.compression_method == 8) ? 20 : 10;
        cd.flags = 0;
        cd.compression_method = wi.compression_method;
        cd.last_mod_time = 0;
        cd.last_mod_date = 0x0021;
        cd.crc32 = wi.crc32;
        cd.compressed_size = wi.compressed_size;
        cd.uncompressed_size = wi.uncompressed_size;
        cd.filename_len = static_cast<uint16_t>(wi.name.size());
        cd.extra_len = 0;
        cd.comment_len = 0;
        cd.disk_start = 0;
        cd.internal_attr = 0;
        cd.external_attr = wi.external_attr;
        cd.local_header_offset = wi.local_offset;

        const uint8_t* cd_bytes = reinterpret_cast<const uint8_t*>(&cd);
        output.insert(output.end(), cd_bytes, cd_bytes + sizeof(ZipCentralHeader));
        output.insert(output.end(), wi.name.begin(), wi.name.end());
    }

    uint32_t cd_size = static_cast<uint32_t>(output.size() - cd_start_offset);

    ZipEOCD eocd{};
    eocd.signature = 0x06054B50;
    eocd.disk_number = 0;
    eocd.cd_start_disk = 0;
    eocd.total_entries_disk = static_cast<uint16_t>(written_infos.size());
    eocd.total_entries = static_cast<uint16_t>(written_infos.size());
    eocd.cd_size = cd_size;
    eocd.cd_offset = cd_start_offset;
    eocd.comment_len = 0;

    const uint8_t* eocd_bytes = reinterpret_cast<const uint8_t*>(&eocd);
    output.insert(output.end(), eocd_bytes, eocd_bytes + sizeof(ZipEOCD));

    return true;
}

}
