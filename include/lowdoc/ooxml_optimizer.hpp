#pragma once

#include "types.hpp"
#include "zip_archive.hpp"
#include <span>
#include <vector>

namespace lowdoc {

class OoxmlOptimizer {
public:
    static bool optimize(ZipArchive& archive, DocumentFormat format, OptimizationReport& report);
    static bool remove_thumbnails(ZipArchive& archive, int64_t& saved_bytes);
    static bool clean_metadata(ZipArchive& archive, int64_t& saved_bytes);
    static bool strip_rsids(ZipArchive& archive, int64_t& saved_bytes);
    static bool prune_unused_styles(ZipArchive& archive, int64_t& saved_bytes);
    static bool deduplicate_media(ZipArchive& archive, int64_t& saved_bytes);
    static bool optimize_embedded_images(ZipArchive& archive, int64_t& saved_bytes);
    static bool minify_all_xml(ZipArchive& archive, int64_t& saved_bytes);
    static bool subset_embedded_fonts(ZipArchive& archive, int64_t& saved_bytes);
    static bool optimize_xlsx_shared_strings(ZipArchive& archive, int64_t& saved_bytes);
    static bool normalize_drawingml(ZipArchive& archive, int64_t& saved_bytes);
    static bool remove_hidden_objects(ZipArchive& archive, int64_t& saved_bytes);
    static bool clean_orphan_relationships(ZipArchive& archive, int64_t& saved_bytes);
    static bool optimize_embedded_packages(ZipArchive& archive, int64_t& saved_bytes);
    static bool normalize_content_types(ZipArchive& archive, int64_t& saved_bytes);
    static bool shorten_media_paths(ZipArchive& archive, int64_t& saved_bytes);
    static bool lift_paragraph_properties(ZipArchive& archive, int64_t& saved_bytes);
    static bool prune_numbering(ZipArchive& archive, int64_t& saved_bytes);
    static bool normalize_table_cells(ZipArchive& archive, int64_t& saved_bytes);
    static bool strip_run_properties(ZipArchive& archive, int64_t& saved_bytes);
    static bool prune_custom_xml(ZipArchive& archive, int64_t& saved_bytes);
    static bool minimize_theme(ZipArchive& archive, int64_t& saved_bytes);
    static bool prune_empty_headers_footers(ZipArchive& archive, int64_t& saved_bytes);
    static bool clean_font_table(ZipArchive& archive, int64_t& saved_bytes);
    static bool clean_settings(ZipArchive& archive, int64_t& saved_bytes);
    static bool prune_bookmarks(ZipArchive& archive, int64_t& saved_bytes);
    static bool normalize_literals(ZipArchive& archive, int64_t& saved_bytes);
};

}
