# LowDoc

Aggressive, 100% lossless document and image optimizer in native C++20.

## Features

### Native RFC 1951 Deflate Engine
Custom-built LZ77 compressor with adaptive sliding-window search, static and dynamic canonical Huffman trees, and candidate-race evaluation between optimal Deflate and uncompressed Store passes.

### Multi-Format Deep Optimization
Surgical container and content cleanup across 17 formats: OOXML (`DOCX`, `XLSX`, `PPTX`, macros & templates), `PDF`, OpenDocument (`ODT`, `ODS`, `ODP`), `EPUB`, `RTF`, and vector/raster graphics (`PNG`, `JPEG`, `SVG`).

### Package & Asset Deduplication
Automatic SHA-256 media hashing and content deduplication across internal parts, pruning unused Word/Excel styles, RSID edit tracking history, redundant XML namespaces, and orphaned relationships.

### Digital Signature Protection
Built-in security guards for PDF and signed archives: detects `/ByteRange` signatures and cryptographic sign-blocks, immediately halting modifications to guarantee integrity and avoid breaking digital seals.

## More Highlights

- **Strictly Lossless**: Zero compromise on fidelity. Preserves identical visual rendering, vector geometry, fonts, formatting, unicode strings, and exact decoded pixel buffers.
- **Native Win32 GUI**: Ultra-compact desktop interface written in pure Win32 API. Zero Electron, zero WebViews, zero bloated runtime frameworks.
- **Context Menu Integration**: Optional right-click integration in Windows Explorer ("Compress with LowDoc") with an unobtrusive progress HUD.
- **Dark & Light Mode Adaptation**: Matches system visual styles automatically with Segoe UI typography and dark-mode DWM window frames.
- **Zero Bloat & Zero Telemetry**: Portable standalone binaries, completely offline, zero background services, zero external DLL dependencies.
- **Safe Output Policy**: Never destroys your original file. Saves output as `filename.lowdoc.<ext>` with automatic incrementing collision avoidance.

## Screenshots

<p align="center">
  <img src="docs/screenshots/optimized_result.png" alt="LowDoc Desktop GUI" width="480">
  <br>
  <em>Native Win32 GUI with optimization metrics and reduction statistics</em>
</p>

<p align="center">
  <img src="docs/screenshots/context_progress_hud.png" alt="LowDoc Context Menu Progress HUD" width="420">
  <br>
  <em>Unobtrusive context-menu progress HUD with instant feedback</em>
</p>

## Architecture

LowDoc is structured into standalone, decoupled native components:

- **`lowdoc_core`**: Static engine library containing RFC 1951 Deflate, ZIP container serializer, XML/HTML stream parsers, format detectors, asset deduplicator, and format optimizers.
- **`lowdoc.exe`**: High-performance CLI tool with multi-threading, recursive directory traversal, batch processing, and machine-readable JSON telemetry.
- **`lowdoc_gui.exe`**: Lightweight native Win32 GUI featuring drag-and-drop, real-time metrics, Explorer context menu registry integration, and system theme adaptation.

## How it works

1. **Format Detection**: Inspects leading magic bytes and container manifests rather than relying strictly on file extensions.
2. **Decontainerization**: Unpacks ZIP/OPC archives and parses internal object graphs into memory streams.
3. **Pass Orchestration**:
   - Strips edit tracking metadata, redundant RSID identifiers, and application thumbnails.
   - Normalizes and minifies internal XML markup while strictly respecting `xml:space="preserve"`.
   - Hashes and deduplicates shared media assets (images, fonts, sounds) and rewires relationship targets.
   - Prunes unused built-in and orphaned user styles.
4. **Multi-Candidate Race**: Compresses streams across competing configurations (Fast, Maximum, Dynamic Huffman, Store), comparing resulting bitstreams byte-for-byte.
5. **Strict Validation**: Decodes the winning candidate through built-in validators; if integrity fails or no space was saved, the original file is preserved byte-identical.

## Benchmark

Evaluation on a comprehensive reference document (`reference_spec.docx`) containing full character sets (Latin, Cyrillic, Greek, Math), structured tables, nested styles, lists, and repeated media assets:

| Metric | Original | Optimized | Delta |
|---|---|---|---|
| **File Size** | 5,834 B | 4,932 B | **-902 B (-15.5%)** |
| **Media Deduplication** | 2 unique streams | 1 shared stream | -24.4 KB payload uncompressed |
| **Styles & RSIDs** | 8 styles, 28 RSID tags | 3 active styles, 0 RSIDs | -846 B |
| **XML Structure** | Verbose formatting | Normalized & minified | -424 B |
| **Visual Integrity** | Baseline | 100% Bit-Identical | Verified Lossless |

## Download & Usage

Get the latest release from [Releases](https://github.com/SashkoTadof/LowDoc/releases).

### GUI
Launch `lowdoc_gui.exe`, drag and drop files directly, or toggle the Explorer context menu option for one-click document compression.

### CLI
```powershell
# Optimize a single document
lowdoc document.docx

# Custom destination path
lowdoc document.docx -o optimized.docx

# Batch optimize entire directory recursively
lowdoc ./reports --recursive

# Output JSON summary
lowdoc document.docx --json

# Quiet mode
lowdoc document.docx --quiet
```

## Requirements & Notes

- **OS**: Windows 10 / 11 64-bit (or Linux for CLI builds).
- **Permissions**: Standard user permissions (registry write access for current user when toggling the context menu).

## Build

Requires Visual Studio 2022 (C++20), Windows SDK 10.0.22621+, and CMake 3.20+.

```powershell
cmake -B build -S .
cmake --build build --config Release
```

Output binaries will be generated in `build/Release/`:
- `lowdoc.exe`
- `lowdoc_gui.exe`
- `lowdoc_test.exe`

### Running Tests
```powershell
.\build\Release\lowdoc_test.exe
```

## License

[MIT](LICENSE)
