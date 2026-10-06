#include "lowdoc/engine.hpp"
#include "lowdoc/format_detector.hpp"
#include "lowdoc/ooxml_optimizer.hpp"
#include "lowdoc/odf_optimizer.hpp"
#include "lowdoc/epub_optimizer.hpp"
#include "lowdoc/pdf_optimizer.hpp"
#include "lowdoc/rtf_optimizer.hpp"
#include "lowdoc/image_optimizer.hpp"
#include "lowdoc/html_optimizer.hpp"
#include "lowdoc/xml_optimizer.hpp"
#include "lowdoc/validator.hpp"
#include "lowdoc/zip_archive.hpp"
#include <fstream>
#include <iostream>

namespace lowdoc {

std::filesystem::path OptimizationEngine::determine_output_path(const std::filesystem::path& input_path, const std::string& custom_output) {
    if (!custom_output.empty()) {
        std::u8string_view u8_sv(reinterpret_cast<const char8_t*>(custom_output.data()), custom_output.size());
        return std::filesystem::path(u8_sv);
    }

    std::filesystem::path parent = input_path.parent_path();
    auto stem = input_path.stem().native();
    auto ext = input_path.extension().native();

#if defined(_WIN32)
    std::filesystem::path candidate = parent / (stem + L".lowdoc" + ext);
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }

    int counter = 1;
    while (true) {
        std::filesystem::path indexed = parent / (stem + L".lowdoc." + std::to_wstring(counter) + ext);
        if (!std::filesystem::exists(indexed)) {
            return indexed;
        }
        counter++;
    }
#else
    std::filesystem::path candidate = parent / (stem + ".lowdoc" + ext);
    if (!std::filesystem::exists(candidate)) {
        return candidate;
    }

    int counter = 1;
    while (true) {
        std::filesystem::path indexed = parent / (stem + ".lowdoc." + std::to_string(counter) + ext);
        if (!std::filesystem::exists(indexed)) {
            return indexed;
        }
        counter++;
    }
#endif
}

std::pair<std::vector<uint8_t>, OptimizationReport> OptimizationEngine::optimize_buffer(
    std::span<const uint8_t> input_data,
    const std::string& filename_hint,
    const OptimizationOptions& options) {

    OptimizationReport report;
    report.original_size = input_data.size();
    report.optimized_size = input_data.size();
    report.format = FormatDetector::detect(input_data, filename_hint);
    report.format_name = format_to_string(report.format);

    if (input_data.empty()) {
        report.message = "Empty input";
        return {{}, report};
    }

    std::vector<std::vector<uint8_t>> candidates;

    if (FormatDetector::is_ooxml(report.format)) {
        ZipArchive archive;
        if (ZipArchive::read(input_data, archive)) {
            OoxmlOptimizer::optimize(archive, report.format, report);

            std::vector<uint8_t> c1;
            if (archive.write(c1, true)) {
                candidates.push_back(std::move(c1));
            }

            if (!options.fast_mode) {
                std::vector<uint8_t> c2;
                if (archive.write(c2, false)) {
                    candidates.push_back(std::move(c2));
                }
            }
        }
    } else if (FormatDetector::is_odf(report.format)) {
        ZipArchive archive;
        if (ZipArchive::read(input_data, archive)) {
            OdfOptimizer::optimize(archive, report.format, report);

            std::vector<uint8_t> c1;
            if (archive.write(c1, true)) {
                candidates.push_back(std::move(c1));
            }
        }
    } else if (report.format == DocumentFormat::Epub) {
        ZipArchive archive;
        if (ZipArchive::read(input_data, archive)) {
            EpubOptimizer::optimize(archive, report);

            std::vector<uint8_t> c1;
            if (archive.write(c1, true)) {
                candidates.push_back(std::move(c1));
            }
        }
    } else if (report.format == DocumentFormat::Pdf) {
        std::vector<uint8_t> c1;
        if (PdfOptimizer::optimize(input_data, c1, report)) {
            candidates.push_back(std::move(c1));
        }
    } else if (report.format == DocumentFormat::Rtf) {
        std::string_view sv(reinterpret_cast<const char*>(input_data.data()), input_data.size());
        std::string out_rtf;
        if (RtfOptimizer::optimize(sv, out_rtf, report)) {
            std::vector<uint8_t> c1(out_rtf.begin(), out_rtf.end());
            candidates.push_back(std::move(c1));
        }
    } else if (report.format == DocumentFormat::Html) {
        std::string_view sv(reinterpret_cast<const char*>(input_data.data()), input_data.size());
        std::string out_html;
        if (HtmlOptimizer::optimize(sv, out_html, report)) {
            std::vector<uint8_t> c1(out_html.begin(), out_html.end());
            candidates.push_back(std::move(c1));
        }
    } else if (report.format == DocumentFormat::Fodt) {
        std::string_view sv(reinterpret_cast<const char*>(input_data.data()), input_data.size());
        std::string minified = XmlOptimizer::minify(sv);
        if (minified.size() < input_data.size() && XmlOptimizer::is_well_formed(minified)) {
            report.savings_by_pass["XML Minification"] += static_cast<int64_t>(input_data.size() - minified.size());
            std::vector<uint8_t> c1(minified.begin(), minified.end());
            candidates.push_back(std::move(c1));
        }
    } else if (FormatDetector::is_image(report.format)) {
        std::string ext = FormatDetector::default_extension(report.format);
        std::vector<uint8_t> c1;
        if (ImageOptimizer::optimize_image(input_data, ext, c1)) {
            candidates.push_back(std::move(c1));
        }
    }

    report.candidates_evaluated = candidates.size();

    const std::vector<uint8_t>* best_candidate = nullptr;
    size_t min_size = input_data.size();

    for (const auto& cand : candidates) {
        if (cand.size() < min_size) {
            if (Validator::validate(report.format, cand, input_data)) {
                min_size = cand.size();
                best_candidate = &cand;
            }
        }
    }

    if (best_candidate && min_size < input_data.size()) {
        report.success = true;
        report.optimized_size = min_size;
        report.message = "Optimized successfully";
        return {*best_candidate, report};
    }

    report.success = false;
    report.optimized_size = input_data.size();
    if (report.message.empty()) {
        report.message = "Already optimal or no lossless reduction possible";
    }
    std::vector<uint8_t> original(input_data.begin(), input_data.end());
    return {original, report};
}

OptimizationReport OptimizationEngine::optimize_file(const std::filesystem::path& input_path, const OptimizationOptions& options) {
    OptimizationReport report;

    std::ifstream file(input_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        report.message = "Failed to open input file";
        return report;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        report.message = "Failed to read input file";
        return report;
    }
    file.close();

    auto [optimized_data, rep] = optimize_buffer(buffer, input_path.extension().string(), options);
    report = std::move(rep);

    if (report.success) {
        std::filesystem::path out_path = determine_output_path(input_path, options.custom_output_path);
        std::ofstream out_file(out_path, std::ios::binary);
        if (!out_file.is_open()) {
            report.success = false;
            report.message = "Failed to open output file for writing";
            return report;
        }

        out_file.write(reinterpret_cast<const char*>(optimized_data.data()), static_cast<std::streamsize>(optimized_data.size()));
        out_file.close();

        report.output_file_path = out_path;
        auto u8 = out_path.u8string();
        report.output_path = std::string(u8.begin(), u8.end());
    }

    return report;
}

}
