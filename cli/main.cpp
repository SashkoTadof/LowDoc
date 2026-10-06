#include "lowdoc/engine.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void print_banner() {
    std::cout << "LowDoc - Lossless Document Optimizer\n";
    std::cout << "Copyright (c) 2026 SashkoTadof\n\n";
}

static void print_usage() {
    std::cout << "Usage: lowdoc [options] <files or directories...>\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o, --output <path>    Specify output file path (single file only)\n";
    std::cout << "  -r, --recursive        Recursively process directories\n";
    std::cout << "  -q, --quiet            Quiet mode (suppress banner and pass details)\n";
    std::cout << "  -j, --json             Output JSON summary\n";
    std::cout << "  -h, --help             Show this help message\n\n";
}

static std::string format_bytes(size_t bytes) {
    std::ostringstream oss;
    if (bytes < 1024) {
        oss << bytes << " B";
    } else if (bytes < 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KB";
    } else {
        oss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    return oss.str();
}

static std::string path_to_utf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

static int run_cli(const std::vector<std::filesystem::path>& args) {
    if (args.empty()) {
        print_banner();
        print_usage();
        return 1;
    }

    std::vector<std::filesystem::path> input_paths;
    std::filesystem::path custom_output;
    bool recursive = false;
    bool quiet = false;
    bool json_output = false;

    for (size_t i = 0; i < args.size(); ++i) {
        std::string arg = path_to_utf8(args[i]);
        if (arg == "-h" || arg == "--help") {
            print_banner();
            print_usage();
            return 0;
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < args.size()) {
                custom_output = args[++i];
            }
        } else if (arg == "-r" || arg == "--recursive") {
            recursive = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg == "-j" || arg == "--json") {
            json_output = true;
        } else if (!arg.starts_with("-")) {
            input_paths.push_back(args[i]);
        }
    }

    if (input_paths.empty()) {
        std::cerr << "Error: No input files specified.\n";
        return 1;
    }

    if (!quiet && !json_output) {
        print_banner();
    }

    std::vector<std::filesystem::path> files_to_process;
    for (const auto& p : input_paths) {
        if (!std::filesystem::exists(p)) {
            std::cerr << "Warning: Path does not exist: " << path_to_utf8(p) << "\n";
            continue;
        }

        if (std::filesystem::is_directory(p)) {
            if (recursive) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(p)) {
                    if (entry.is_regular_file()) {
                        files_to_process.push_back(entry.path());
                    }
                }
            } else {
                for (const auto& entry : std::filesystem::directory_iterator(p)) {
                    if (entry.is_regular_file()) {
                        files_to_process.push_back(entry.path());
                    }
                }
            }
        } else if (std::filesystem::is_regular_file(p)) {
            files_to_process.push_back(p);
        }
    }

    if (files_to_process.empty()) {
        std::cerr << "No matching files to process.\n";
        return 1;
    }

    lowdoc::OptimizationOptions options;
    if (files_to_process.size() == 1 && !custom_output.empty()) {
        options.custom_output_path = path_to_utf8(custom_output);
    }

    size_t total_orig = 0;
    size_t total_opt = 0;
    size_t processed_count = 0;
    size_t optimized_count = 0;

    for (const auto& file_path : files_to_process) {
        if (!quiet && !json_output) {
            std::cout << "Optimizing " << path_to_utf8(file_path.filename()) << "... ";
            std::cout.flush();
        }

        auto report = lowdoc::OptimizationEngine::optimize_file(file_path, options);
        processed_count++;
        total_orig += report.original_size;

        if (report.success) {
            optimized_count++;
            total_opt += report.optimized_size;

            if (json_output) {
                std::cout << "{\n";
                std::cout << "  \"file\": \"" << path_to_utf8(file_path) << "\",\n";
                std::cout << "  \"output\": \"" << path_to_utf8(report.output_file_path) << "\",\n";
                std::cout << "  \"format\": \"" << report.format_name << "\",\n";
                std::cout << "  \"original_size\": " << report.original_size << ",\n";
                std::cout << "  \"optimized_size\": " << report.optimized_size << ",\n";
                std::cout << "  \"saved_bytes\": " << report.bytes_saved() << ",\n";
                std::cout << "  \"reduction_pct\": " << std::fixed << std::setprecision(2) << report.reduction_percentage() << "\n";
                std::cout << "}\n";
            } else if (!quiet) {
                std::cout << "DONE\n";
                std::cout << "  Format:    " << report.format_name << "\n";
                std::cout << "  Before:    " << format_bytes(report.original_size) << " (" << report.original_size << " bytes)\n";
                std::cout << "  After:     " << format_bytes(report.optimized_size) << " (" << report.optimized_size << " bytes)\n";
                std::cout << "  Saved:     " << format_bytes(report.bytes_saved()) << "\n";
                std::cout << "  Reduction: " << std::fixed << std::setprecision(1) << report.reduction_percentage() << "%\n";
                std::cout << "  Output:    " << path_to_utf8(report.output_file_path) << "\n";

                if (!report.savings_by_pass.empty()) {
                    std::cout << "  Pass breakdown:\n";
                    for (const auto& [pass, saved] : report.savings_by_pass) {
                        std::cout << "    - " << pass << ": -" << format_bytes(static_cast<size_t>(saved)) << "\n";
                    }
                }
                std::cout << "\n";
            }
        } else {
            total_opt += report.original_size;
            if (json_output) {
                std::cout << "{\n";
                std::cout << "  \"file\": \"" << path_to_utf8(file_path) << "\",\n";
                std::cout << "  \"status\": \"unchanged\",\n";
                std::cout << "  \"message\": \"" << report.message << "\"\n";
                std::cout << "}\n";
            } else if (!quiet) {
                std::cout << "UNCHANGED\n";
                std::cout << "  Reason: " << report.message << "\n\n";
            }
        }
    }

    if (!quiet && !json_output && files_to_process.size() > 1) {
        std::cout << "========================================\n";
        std::cout << "Total files processed: " << processed_count << "\n";
        std::cout << "Files reduced:         " << optimized_count << "\n";
        std::cout << "Original total:        " << format_bytes(total_orig) << "\n";
        std::cout << "Optimized total:       " << format_bytes(total_opt) << "\n";
        if (total_orig > total_opt) {
            double total_pct = (1.0 - (static_cast<double>(total_opt) / static_cast<double>(total_orig))) * 100.0;
            std::cout << "Total saved:           " << format_bytes(total_orig - total_opt) << " (" << std::fixed << std::setprecision(1) << total_pct << "%)\n";
        }
        std::cout << "========================================\n";
    }

    return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::vector<std::filesystem::path> args;
    args.reserve(argc > 1 ? argc - 1 : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return run_cli(args);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::filesystem::path> args;
    args.reserve(argc > 1 ? argc - 1 : 0);
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return run_cli(args);
}
#endif
