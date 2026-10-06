#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <unordered_map>

namespace lowdoc {

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    uint16_t compression_method = 8;
    uint32_t external_attributes = 0x81A40000;
    uint16_t last_mod_time = 0;
    uint16_t last_mod_date = 0x0021;
    bool force_store = false;
};

class ZipArchive {
public:
    static bool read(std::span<const uint8_t> bytes, ZipArchive& archive);
    bool write(std::vector<uint8_t>& output, bool aggressive_deflate = true) const;

    bool has_entry(const std::string& name) const;
    const ZipEntry* get_entry(const std::string& name) const;
    ZipEntry* get_entry_mut(const std::string& name);
    void add_or_replace(const ZipEntry& entry);
    bool remove(const std::string& name);
    std::vector<std::string> entry_names() const;
    size_t count() const { return entries_.size(); }
    const std::vector<ZipEntry>& entries() const { return entries_; }

private:
    std::vector<ZipEntry> entries_;
    std::unordered_map<std::string, size_t> index_map_;
    void rebuild_index();
};

}
