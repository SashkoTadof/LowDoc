#include "lowdoc/types.hpp"
#include "lowdoc/engine.hpp"
#include "lowdoc/validator.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <span>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            g_tests_failed++; \
            return; \
        } \
    } while(0)

static void test_reference_spec_docx() {
    std::filesystem::path ref_path = "tests/reference_spec.docx";
    if (!std::filesystem::exists(ref_path)) {
        ref_path = "../tests/reference_spec.docx";
    }
    if (!std::filesystem::exists(ref_path)) {
        ref_path = "../../tests/reference_spec.docx";
    }

    TEST_ASSERT(std::filesystem::exists(ref_path), "tests/reference_spec.docx exists");

    lowdoc::OptimizationOptions options;
    options.preserve_original = true;
    options.fast_mode = false;

    auto report = lowdoc::OptimizationEngine::optimize_file(ref_path, options);
    TEST_ASSERT(report.success, "Optimization succeeded");
    TEST_ASSERT(!report.output_file_path.empty(), "Output file generated");
    TEST_ASSERT(std::filesystem::exists(report.output_file_path), "Output file exists on disk");
    TEST_ASSERT(report.optimized_size <= report.original_size, "File size decreased or stayed optimal");

    std::ifstream file(report.output_file_path, std::ios::binary);
    TEST_ASSERT(file.is_open(), "Optimized file opened");
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    bool is_valid = lowdoc::Validator::validate_ooxml(buffer);
    TEST_ASSERT(is_valid, "Optimized DOCX passed structural OOXML validation");

    std::filesystem::remove(report.output_file_path);

    g_tests_passed++;
    std::cout << "PASS: test_reference_spec_docx (" << report.original_size << " -> " << report.optimized_size 
              << " bytes, -" << report.reduction_percentage() << "%)\n";
}

int main() {
    std::cout << "=== LowDoc Reference Test ===\n";
    test_reference_spec_docx();

    std::cout << "\n========================================\n";
    std::cout << "Tests passed: " << g_tests_passed << "\n";
    std::cout << "Tests failed: " << g_tests_failed << "\n";
    std::cout << "========================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
