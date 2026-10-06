#include "lowdoc/format_detector.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace lowdoc {

static bool starts_with_bytes(std::span<const uint8_t> data, const uint8_t* prefix, size_t prefix_len) {
    if (data.size() < prefix_len) return false;
    return std::memcmp(data.data(), prefix, prefix_len) == 0;
}

static bool contains_subsequence(std::span<const uint8_t> data, const char* str) {
    size_t len = std::strlen(str);
    if (data.size() < len) return false;
    auto it = std::search(
        data.begin(), data.end(),
        reinterpret_cast<const uint8_t*>(str),
        reinterpret_cast<const uint8_t*>(str) + len
    );
    return it != data.end();
}

static std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

DocumentFormat FormatDetector::detect(std::span<const uint8_t> data, const std::string& filename_hint) {
    if (data.empty()) return DocumentFormat::Unknown;

    const uint8_t png_sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (starts_with_bytes(data, png_sig, sizeof(png_sig))) {
        return DocumentFormat::Png;
    }

    const uint8_t jpeg_sig[] = {0xFF, 0xD8, 0xFF};
    if (starts_with_bytes(data, jpeg_sig, sizeof(jpeg_sig))) {
        return DocumentFormat::Jpeg;
    }

    const uint8_t pdf_sig[] = {'%', 'P', 'D', 'F', '-'};
    if (starts_with_bytes(data, pdf_sig, sizeof(pdf_sig))) {
        return DocumentFormat::Pdf;
    }

    const uint8_t rtf_sig[] = {'{', '\\', 'r', 't', 'f'};
    if (starts_with_bytes(data, rtf_sig, sizeof(rtf_sig))) {
        return DocumentFormat::Rtf;
    }

    if (data.size() >= 12 && starts_with_bytes(data, reinterpret_cast<const uint8_t*>("RIFF"), 4)) {
        if (std::memcmp(data.data() + 8, "WEBP", 4) == 0) {
            return DocumentFormat::Webp;
        }
    }

    const uint8_t zip_sig[] = {'P', 'K', 0x03, 0x04};
    if (starts_with_bytes(data, zip_sig, sizeof(zip_sig))) {
        if (contains_subsequence(data, "application/epub+zip")) {
            return DocumentFormat::Epub;
        }
        if (contains_subsequence(data, "application/vnd.oasis.opendocument.text")) {
            return DocumentFormat::Odt;
        }
        if (contains_subsequence(data, "application/vnd.oasis.opendocument.spreadsheet")) {
            return DocumentFormat::Ods;
        }
        if (contains_subsequence(data, "application/vnd.oasis.opendocument.presentation")) {
            return DocumentFormat::Odp;
        }
        if (contains_subsequence(data, "META-INF/container.xml")) {
            return DocumentFormat::Epub;
        }
        if (contains_subsequence(data, "META-INF/manifest.xml")) {
            std::string hint = to_lower(filename_hint);
            if (hint.ends_with(".ods")) return DocumentFormat::Ods;
            if (hint.ends_with(".odp")) return DocumentFormat::Odp;
            return DocumentFormat::Odt;
        }

        bool has_content_types = contains_subsequence(data, "[Content_Types].xml");
        bool has_word = contains_subsequence(data, "word/");
        bool has_xl = contains_subsequence(data, "xl/");
        bool has_ppt = contains_subsequence(data, "ppt/");

        std::string hint = to_lower(filename_hint);

        if (has_content_types || has_word || has_xl || has_ppt) {
            if (has_word || hint.ends_with(".docx") || hint.ends_with(".docm") || hint.ends_with(".dotx") || hint.ends_with(".dotm")) {
                if (hint.ends_with(".docm")) return DocumentFormat::Docm;
                if (hint.ends_with(".dotx")) return DocumentFormat::Dotx;
                if (hint.ends_with(".dotm")) return DocumentFormat::Dotm;
                return DocumentFormat::Docx;
            }
            if (has_xl || hint.ends_with(".xlsx") || hint.ends_with(".xlsm") || hint.ends_with(".xltx") || hint.ends_with(".xltm")) {
                if (hint.ends_with(".xlsm")) return DocumentFormat::Xlsm;
                if (hint.ends_with(".xltx")) return DocumentFormat::Xltx;
                if (hint.ends_with(".xltm")) return DocumentFormat::Xltm;
                return DocumentFormat::Xlsx;
            }
            if (has_ppt || hint.ends_with(".pptx") || hint.ends_with(".pptm") || hint.ends_with(".potx") || hint.ends_with(".potm") || hint.ends_with(".ppsx") || hint.ends_with(".ppsm")) {
                if (hint.ends_with(".pptm")) return DocumentFormat::Pptm;
                if (hint.ends_with(".potx")) return DocumentFormat::Potx;
                if (hint.ends_with(".potm")) return DocumentFormat::Potm;
                if (hint.ends_with(".ppsx")) return DocumentFormat::Ppsx;
                if (hint.ends_with(".ppsm")) return DocumentFormat::Ppsm;
                return DocumentFormat::Pptx;
            }
            return DocumentFormat::Docx;
        }
    }

    if (contains_subsequence(data, "<svg") || (contains_subsequence(data, "<?xml") && contains_subsequence(data, "<svg"))) {
        return DocumentFormat::Svg;
    }

    std::string hint = to_lower(filename_hint);

    if (contains_subsequence(data, "<office:document") || hint.ends_with(".fodt")) {
        return DocumentFormat::Fodt;
    }

    if (contains_subsequence(data, "<!doctype html") || contains_subsequence(data, "<!DOCTYPE html") ||
        contains_subsequence(data, "<html") || hint.ends_with(".html") || hint.ends_with(".htm")) {
        return DocumentFormat::Html;
    }

    if (hint.ends_with(".docx")) return DocumentFormat::Docx;
    if (hint.ends_with(".xlsx")) return DocumentFormat::Xlsx;
    if (hint.ends_with(".pptx")) return DocumentFormat::Pptx;
    if (hint.ends_with(".pdf")) return DocumentFormat::Pdf;
    if (hint.ends_with(".odt")) return DocumentFormat::Odt;
    if (hint.ends_with(".ods")) return DocumentFormat::Ods;
    if (hint.ends_with(".odp")) return DocumentFormat::Odp;
    if (hint.ends_with(".epub")) return DocumentFormat::Epub;
    if (hint.ends_with(".rtf")) return DocumentFormat::Rtf;
    if (hint.ends_with(".png")) return DocumentFormat::Png;
    if (hint.ends_with(".jpg") || hint.ends_with(".jpeg")) return DocumentFormat::Jpeg;
    if (hint.ends_with(".webp")) return DocumentFormat::Webp;
    if (hint.ends_with(".svg")) return DocumentFormat::Svg;
    if (hint.ends_with(".html") || hint.ends_with(".htm")) return DocumentFormat::Html;
    if (hint.ends_with(".fodt")) return DocumentFormat::Fodt;

    return DocumentFormat::Unknown;
}

bool FormatDetector::is_ooxml(DocumentFormat fmt) {
    switch (fmt) {
        case DocumentFormat::Docx:
        case DocumentFormat::Docm:
        case DocumentFormat::Dotx:
        case DocumentFormat::Dotm:
        case DocumentFormat::Xlsx:
        case DocumentFormat::Xlsm:
        case DocumentFormat::Xltx:
        case DocumentFormat::Xltm:
        case DocumentFormat::Pptx:
        case DocumentFormat::Pptm:
        case DocumentFormat::Potx:
        case DocumentFormat::Potm:
        case DocumentFormat::Ppsx:
        case DocumentFormat::Ppsm:
            return true;
        default:
            return false;
    }
}

bool FormatDetector::is_odf(DocumentFormat fmt) {
    return fmt == DocumentFormat::Odt || fmt == DocumentFormat::Ods || fmt == DocumentFormat::Odp;
}

bool FormatDetector::is_zip_container(DocumentFormat fmt) {
    return is_ooxml(fmt) || is_odf(fmt) || fmt == DocumentFormat::Epub;
}

bool FormatDetector::is_image(DocumentFormat fmt) {
    return fmt == DocumentFormat::Png || fmt == DocumentFormat::Jpeg || fmt == DocumentFormat::Webp || fmt == DocumentFormat::Svg;
}

std::string FormatDetector::default_extension(DocumentFormat fmt) {
    switch (fmt) {
        case DocumentFormat::Docx: return ".docx";
        case DocumentFormat::Docm: return ".docm";
        case DocumentFormat::Dotx: return ".dotx";
        case DocumentFormat::Dotm: return ".dotm";
        case DocumentFormat::Xlsx: return ".xlsx";
        case DocumentFormat::Xlsm: return ".xlsm";
        case DocumentFormat::Xltx: return ".xltx";
        case DocumentFormat::Xltm: return ".xltm";
        case DocumentFormat::Pptx: return ".pptx";
        case DocumentFormat::Pptm: return ".pptm";
        case DocumentFormat::Potx: return ".potx";
        case DocumentFormat::Potm: return ".potm";
        case DocumentFormat::Ppsx: return ".ppsx";
        case DocumentFormat::Ppsm: return ".ppsm";
        case DocumentFormat::Pdf: return ".pdf";
        case DocumentFormat::Odt: return ".odt";
        case DocumentFormat::Ods: return ".ods";
        case DocumentFormat::Odp: return ".odp";
        case DocumentFormat::Epub: return ".epub";
        case DocumentFormat::Rtf: return ".rtf";
        case DocumentFormat::Png: return ".png";
        case DocumentFormat::Jpeg: return ".jpg";
        case DocumentFormat::Webp: return ".webp";
        case DocumentFormat::Svg: return ".svg";
        case DocumentFormat::Html: return ".html";
        case DocumentFormat::Fodt: return ".fodt";
        default: return "";
    }
}

}
