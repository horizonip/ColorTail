# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure (from project root, using MinGW + Ninja)
cmake -B build -G Ninja

# Build
cmake --build build

# Output: build/ColorTail.exe
```

Requires MinGW (GCC) with `-municode -static` link flags (configured in CMakeLists.txt for MINGW). C++17 standard.

## Build Installer (WiX v4)

```bash
cd installer
wix build Package.wxs -o ColorTail.msi
```

The installer references `../build/ColorTail.exe`, so build the project first.

## Usage

```
ColorTail.exe <filepath> [lines]
```

- `filepath` — file to monitor (opens a file picker if omitted)
- `lines` — number of tail lines to display (default: 50)
- `/?`, `--help`, `-h` — show usage info with version number

## Architecture

Single-file Win32 GUI application (`ColorTail.cpp`) — no framework, no dependencies beyond Windows APIs.

**Display:** RichEdit control (msftedit.dll) with black background, Consolas font. Status bar shows last update time, file size, and line count.

**File monitoring:** A 1-second `WM_TIMER` calls `UpdateContent()`, which reads the last N lines via `ReadTailLines()` (Win32 file APIs, reads up to 16 MB tail, converts UTF-8/ANSI to wide string).

**Incremental updates:** `UpdateContent()` computes overlap between old and new line sets. When lines are appended, only new lines are added to the RichEdit (with top lines trimmed to maintain the window size). When no overlap is found (truncation/replacement), the entire control is redrawn.

**Color cycling:** 8-color palette. Color index advances each time new lines are appended, making it easy to visually distinguish successive batches of output.

**Keyboard shortcuts:** Intercepted in the message loop before `TranslateMessage`/`DispatchMessageW`. Space (pause), Ctrl+O (open), Ctrl+W (close/reopen), Ctrl+F (find), F3 (find next), Ctrl+G (go to line), Ctrl+Home/End (top/bottom), Escape (exit). Ctrl+C (copy) and Ctrl+A (select all) are handled natively by the RichEdit control.

**Dialogs:** Find and Go To Line dialogs are built with in-memory `DLGTEMPLATE` structs (no .rc resource files). Dialog procs are `FindDlgProc` and `GoToLineDlgProc`.

**Context menu:** Right-click popup menu (`WM_CONTEXTMENU` / `WM_COMMAND`) mirrors all keyboard shortcuts. Menu item IDs are `IDM_*` constants.

**Version:** `APP_VERSION` constant displayed in the title bar and help dialog. Also referenced in `installer/Package.wxs`.
