#include "lowdoc/validator.hpp"
#include "lowdoc/zip_archive.hpp"
#include "lowdoc/xml_optimizer.hpp"
#include "lowdoc/rtf_optimizer.hpp"
#include "lowdoc/format_detector.hpp"
#include <cstring>
#include <string_view>

namespace lowdoc {

bool Validator::validate_zip(std::span<const uint8_t> data) {
    ZipArchive archive;
    if (!ZipArchive::read(data, archive)) {
        return false;
    }
    return archive.count() > 0;
}

bool Validator::validate_ooxml(std::span<const uint8_t> data) {
    ZipArchive archive;
    if (!ZipArchive::read(data, archive)) {
        return false;
    }

    const auto* ct = archive.get_entry("[Content_Types].xml");
    if (!ct) return false;
    std::string_view ct_xml(reinterpret_cast<const char*>(ct->data.data()), ct->data.size());
    if (!XmlOptimizer::is_well_formed(ct_xml)) return false;

    const auto* rels = archive.get_entry("_rels/.rels");
    if (!rels) return false;
    std::string_view rels_xml(reinterpret_cast<const char*>(rels->data.data()), rels->data.size());
    if (!XmlOptimizer::is_well_formed(rels_xml)) return false;

    for (const auto& entry : archive.entries()) {
        if (entry.name.ends_with(".xml")) {
            std::string_view xml(reinterpret_cast<const char*>(entry.data.data()), entry.data.size());
            if (!XmlOptimizer::is_well_formed(xml)) {
                return false;
            }
        }
    }

    return true;
}

bool Validator::validate_odf(std::span<const uint8_t> data) {
    ZipArchive archive;
    if (!ZipArchive::read(data, archive)) {
        return false;
    }

    if (!archive.has_entry("mimetype")) return false;
    if (!archive.has_entry("content.xml")) return false;

    for (const auto& entry : archive.entries()) {
        if (entry.name.ends_with(".xml")) {
            std::string_view xml(reinterpret_cast<const char*>(entry.data.data()), entry.data.size());
            if (!XmlOptimizer::is_well_formed(xml)) {
                return false;
            }
        }
    }

    return true;
}

bool Validator::validate_epub(std::span<const uint8_t> data) {
    ZipArchive archive;
    if (!ZipArchive::read(data, archive)) {
        return false;
    }

    if (!archive.has_entry("mimetype")) return false;
    if (!archive.has_entry("META-INF/container.xml")) return false;

    return true;
}

bool Validator::validate_pdf(std::span<const uint8_t> data, std::span<const uint8_t>) {
    if (data.size() < 30) return false;
    std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());

    if (!sv.starts_with("%PDF-")) return false;
    if (sv.rfind("%%EOF") == std::string_view::npos) return false;
    if (sv.rfind("startxref") == std::string_view::npos) return false;
    if (sv.find("/Root") == std::string_view::npos) return false;

    return true;
}

bool Validator::validate_rtf(std::span<const uint8_t> data) {
    std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
    return RtfOptimizer::is_valid(sv);
}

bool Validator::validate_image(DocumentFormat format, std::span<const uint8_t> data) {
    if (format == DocumentFormat::Png) {
        const uint8_t png_sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (data.size() < 16 || std::memcmp(data.data(), png_sig, 8) != 0) return false;
        std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
        return sv.find("IEND") != std::string_view::npos;
    }
    if (format == DocumentFormat::Jpeg) {
        if (data.size() < 4) return false;
        return data[0] == 0xFF && data[1] == 0xD8 && data[data.size() - 2] == 0xFF && data[data.size() - 1] == 0xD9;
    }
    if (format == DocumentFormat::Svg) {
        std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
        return XmlOptimizer::is_well_formed(sv);
    }
    return true;
}

bool Validator::validate(DocumentFormat format, std::span<const uint8_t> data, std::span<const uint8_t> original_data) {
    if (data.empty()) return false;

    if (FormatDetector::is_ooxml(format)) {
        return validate_ooxml(data);
    }
    if (FormatDetector::is_odf(format)) {
        return validate_odf(data);
    }
    if (format == DocumentFormat::Epub) {
        return validate_epub(data);
    }
    if (format == DocumentFormat::Pdf) {
        return validate_pdf(data, original_data);
    }
    if (format == DocumentFormat::Rtf) {
        return validate_rtf(data);
    }
    if (FormatDetector::is_image(format)) {
        return validate_image(format, data);
    }
    if (format == DocumentFormat::Fodt) {
        std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
        return XmlOptimizer::is_well_formed(sv);
    }
    if (format == DocumentFormat::Html) {
        std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
        return sv.find("<html") != std::string_view::npos || sv.find("<!doctype") != std::string_view::npos || sv.find("<!DOCTYPE") != std::string_view::npos;
    }

    return true;
}

}
