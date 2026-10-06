cmake_minimum_required(VERSION 3.20)
project(LowDoc VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX- /O2 /permissive- /utf-8)
endif()

set(LOWDOC_CORE_SOURCES
    src/types.cpp
    src/format_detector.cpp
    src/deflate.cpp
    src/zip_archive.cpp
    src/xml_optimizer.cpp
    src/image_optimizer.cpp
    src/ooxml_optimizer.cpp
    src/pdf_optimizer.cpp
    src/odf_optimizer.cpp
    src/epub_optimizer.cpp
    src/rtf_optimizer.cpp
    src/html_optimizer.cpp
    src/resource_deduplicator.cpp
    src/font_subsetter.cpp
    src/validator.cpp
    src/engine.cpp
)

add_library(lowdoc_core STATIC ${LOWDOC_CORE_SOURCES})
target_include_directories(lowdoc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(lowdoc cli/main.cpp resources/resource.rc)
target_link_libraries(lowdoc PRIVATE lowdoc_core)

add_executable(lowdoc_gui WIN32 gui/win_main.cpp resources/resource.rc)
target_link_libraries(lowdoc_gui PRIVATE lowdoc_core shell32 comdlg32 user32 gdi32 advapi32 dwmapi uxtheme comctl32)

add_executable(lowdoc_test tests/test_runner.cpp)
target_link_libraries(lowdoc_test PRIVATE lowdoc_core)
