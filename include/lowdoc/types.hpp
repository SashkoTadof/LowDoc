#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>

namespace lowdoc {

enum class DocumentFormat {
    Unknown,
    Docx,
    Docm,
    Dotx,
    Dotm,
    Xlsx,
    Xlsm,
    Xltx,
    Xltm,
    Pptx,
    Pptm,
    Potx,
    Potm,
    Ppsx,
    Ppsm,
    Pdf,
    Odt,
    Ods,
    Odp,
    Epub,
    Rtf,
    Png,
    Jpeg,
    Webp,
    Svg,
    Html,
    Fodt
};

struct OptimizationReport {
    bool success = false;
    DocumentFormat format = DocumentFormat::Unknown;
    std::string format_name;
    size_t original_size = 0;
    size_t optimized_size = 0;
    size_t candidates_evaluated = 0;
    std::map<std::string, int64_t> savings_by_pass;
    std::string message;
    std::string output_path;
    std::filesystem::path output_file_path;

    double reduction_percentage() const {
        if (original_size == 0 || optimized_size >= original_size) {
            return 0.0;
        }
        return (1.0 - (static_cast<double>(optimized_size) / static_cast<double>(original_size))) * 100.0;
    }

    size_t bytes_saved() const {
        if (optimized_size >= original_size) {
            return 0;
        }
        return original_size - optimized_size;
    }
};

struct OptimizationOptions {
    bool preserve_original = true;
    bool fast_mode = false;
    bool verbose = false;
    std::string custom_output_path;
};

const char* format_to_string(DocumentFormat fmt);

}
