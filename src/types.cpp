#include "lowdoc/types.hpp"

namespace lowdoc {

const char* format_to_string(DocumentFormat fmt) {
    switch (fmt) {
        case DocumentFormat::Docx: return "DOCX";
        case DocumentFormat::Docm: return "DOCM";
        case DocumentFormat::Dotx: return "DOTX";
        case DocumentFormat::Dotm: return "DOTM";
        case DocumentFormat::Xlsx: return "XLSX";
        case DocumentFormat::Xlsm: return "XLSM";
        case DocumentFormat::Xltx: return "XLTX";
        case DocumentFormat::Xltm: return "XLTM";
        case DocumentFormat::Pptx: return "PPTX";
        case DocumentFormat::Pptm: return "PPTM";
        case DocumentFormat::Potx: return "POTX";
        case DocumentFormat::Potm: return "POTM";
        case DocumentFormat::Ppsx: return "PPSX";
        case DocumentFormat::Ppsm: return "PPSM";
        case DocumentFormat::Pdf: return "PDF";
        case DocumentFormat::Odt: return "ODT";
        case DocumentFormat::Ods: return "ODS";
        case DocumentFormat::Odp: return "ODP";
        case DocumentFormat::Epub: return "EPUB";
        case DocumentFormat::Rtf: return "RTF";
        case DocumentFormat::Png: return "PNG";
        case DocumentFormat::Jpeg: return "JPEG";
        case DocumentFormat::Webp: return "WEBP";
        case DocumentFormat::Svg: return "SVG";
        case DocumentFormat::Html: return "HTML";
        case DocumentFormat::Fodt: return "FODT";
        default: return "Unknown";
    }
}

}
