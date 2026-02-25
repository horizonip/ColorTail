#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <richedit.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cstdio>

// ── Constants ───────────────────────────────────────────────────────────────
static constexpr int  TIMER_ID        = 1;
static constexpr int  TIMER_INTERVAL  = 1000;   // 1 second
static constexpr int  DEFAULT_LINES   = 50;
static constexpr int  IDC_RICHEDIT    = 100;
static constexpr int  IDC_STATUSBAR   = 101;
static constexpr int  STATUS_HEIGHT   = 22;

// 8-colour cycle matching the PowerShell version
static const COLORREF g_colors[] = {
    RGB(0,   255, 0),    // LimeGreen
    RGB(0,   255, 255),  // Cyan
    RGB(255, 255, 0),    // Yellow
    RGB(255, 0,   255),  // Magenta
    RGB(255, 165, 0),    // Orange
    RGB(255, 105, 180),  // HotPink
    RGB(173, 216, 230),  // LightBlue
    RGB(144, 238, 144),  // LightGreen
};
static constexpr int COLOR_COUNT = _countof(g_colors);

// ── Global state ────────────────────────────────────────────────────────────
static std::wstring  g_filePath;
static int           g_lineCount    = DEFAULT_LINES;
static int           g_colorIndex   = 0;
static std::wstring  g_lastContent;
static LARGE_INTEGER g_lastSize     = {};
static HWND          g_hRichEdit    = nullptr;
static HWND          g_hStatusBar   = nullptr;
static HMODULE       g_hRichEditLib = nullptr;

// ── Helpers ─────────────────────────────────────────────────────────────────

// Read the last N lines of a text file. Returns the lines and total byte size.
static std::vector<std::wstring> ReadTailLines(const std::wstring& path,
                                               int maxLines,
                                               LARGE_INTEGER& fileSizeOut)
{
    std::vector<std::wstring> result;
    fileSizeOut.QuadPart = 0;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return result;

    GetFileSizeEx(hFile, &fileSizeOut);

    // Read entire file into a buffer (practical for log tailing)
    DWORD sizeToRead = (fileSizeOut.QuadPart > 16 * 1024 * 1024)
                        ? 16 * 1024 * 1024
                        : static_cast<DWORD>(fileSizeOut.QuadPart);

    if (sizeToRead == 0) {
        CloseHandle(hFile);
        return result;
    }

    // Seek to read only the tail portion for large files
    if (fileSizeOut.QuadPart > static_cast<LONGLONG>(sizeToRead)) {
        LARGE_INTEGER offset;
        offset.QuadPart = fileSizeOut.QuadPart - sizeToRead;
        SetFilePointerEx(hFile, offset, nullptr, FILE_BEGIN);
    }

    std::vector<char> buf(sizeToRead + 1);
    DWORD bytesRead = 0;
    ReadFile(hFile, buf.data(), sizeToRead, &bytesRead, nullptr);
    CloseHandle(hFile);
    buf[bytesRead] = '\0';

    // Convert from UTF-8 (or ANSI) to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), bytesRead, nullptr, 0);
    if (wideLen == 0) {
        // Fallback to ANSI
        wideLen = MultiByteToWideChar(CP_ACP, 0, buf.data(), bytesRead, nullptr, 0);
        if (wideLen == 0) return result;
        std::wstring wide(wideLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, buf.data(), bytesRead, wide.data(), wideLen);
        // Split into lines
        std::wistringstream iss(wide);
        std::wstring line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == L'\r')
                line.pop_back();
            result.push_back(std::move(line));
        }
    } else {
        std::wstring wide(wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buf.data(), bytesRead, wide.data(), wideLen);
        std::wistringstream iss(wide);
        std::wstring line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == L'\r')
                line.pop_back();
            result.push_back(std::move(line));
        }
    }

    // Keep only the last N lines
    if (static_cast<int>(result.size()) > maxLines) {
        result.erase(result.begin(), result.end() - maxLines);
    }

    return result;
}

// Join lines into a single string with \r\n separators.
static std::wstring JoinLines(const std::vector<std::wstring>& lines)
{
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out += L"\r\n";
        out += lines[i];
    }
    return out;
}

// Append coloured text to the RichEdit control.
static void AppendColored(HWND hRich, const std::wstring& text, COLORREF color)
{
    // Move selection to end
    int len = GetWindowTextLengthW(hRich);
    SendMessageW(hRich, EM_SETSEL, len, len);

    // Set character format for the selection
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    SendMessageW(hRich, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));

    // Insert text
    SendMessageW(hRich, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

// Check whether the RichEdit is scrolled to the bottom.
static bool IsScrolledToBottom(HWND hRich)
{
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(hRich, SB_VERT, &si))
        return true; // no scrollbar means everything visible
    return (si.nPos + static_cast<int>(si.nPage)) >= si.nMax;
}

// Scroll the RichEdit to the bottom.
static void ScrollToBottom(HWND hRich)
{
    int len = GetWindowTextLengthW(hRich);
    SendMessageW(hRich, EM_SETSEL, len, len);
    SendMessageW(hRich, EM_SCROLLCARET, 0, 0);
}

// Format the status bar text.
static void UpdateStatusBar(int lineCount, LARGE_INTEGER fileSize)
{
    wchar_t timeBuf[16];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(timeBuf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    double sizeKB = fileSize.QuadPart / 1024.0;

    wchar_t status[256];
    swprintf_s(status, L"Last updated: %s | Size: %.2f KB | Lines shown: %d",
               timeBuf, sizeKB, lineCount);

    SetWindowTextW(g_hStatusBar, status);
}

// ── Core update logic (called by timer and initial load) ────────────────────
static void UpdateContent()
{
    LARGE_INTEGER fileSize;
    std::vector<std::wstring> lines = ReadTailLines(g_filePath, g_lineCount, fileSize);
    std::wstring newContent = JoinLines(lines);

    if (newContent != g_lastContent) {
        bool wasAtBottom = IsScrolledToBottom(g_hRichEdit);

        // Freeze drawing
        SendMessageW(g_hRichEdit, WM_SETREDRAW, FALSE, 0);

        // Split old content into lines for comparison
        std::vector<std::wstring> oldLines;
        if (!g_lastContent.empty()) {
            std::wistringstream iss(g_lastContent);
            std::wstring line;
            while (std::getline(iss, line)) {
                if (!line.empty() && line.back() == L'\r')
                    line.pop_back();
                oldLines.push_back(std::move(line));
            }
        }

        int oldCount = static_cast<int>(oldLines.size());
        int newCount = static_cast<int>(lines.size());

        // Find overlap: how many lines at the end of oldLines match
        // the beginning of the new lines (the tail window may have shifted).
        int overlap = 0;
        if (oldCount > 0) {
            for (int try_ov = std::min(oldCount, newCount); try_ov > 0; --try_ov) {
                bool match = true;
                for (int j = 0; j < try_ov; ++j) {
                    if (oldLines[oldCount - try_ov + j] != lines[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) { overlap = try_ov; break; }
            }
        }

        int numNew = newCount - overlap;

        if (numNew > 0 && overlap > 0) {
            // Lines were appended — cycle colour and add only the new ones
            g_colorIndex = (g_colorIndex + 1) % COLOR_COUNT;
            COLORREF color = g_colors[g_colorIndex];

            for (int i = overlap; i < newCount; ++i) {
                AppendColored(g_hRichEdit, L"\r\n" + lines[i], color);
            }

            // Trim the top of the RichEdit to keep it in sync with the tail window.
            // Remove lines that shifted out (oldCount - overlap lines from top).
            int linesToRemove = oldCount - overlap;
            if (linesToRemove > 0) {
                int charIdx = 0;
                for (int i = 0; i < linesToRemove; ++i) {
                    charIdx = static_cast<int>(
                        SendMessageW(g_hRichEdit, EM_LINEINDEX, i + 1, 0));
                }
                if (charIdx > 0) {
                    SendMessageW(g_hRichEdit, EM_SETSEL, 0, charIdx);
                    SendMessageW(g_hRichEdit, EM_REPLACESEL, FALSE,
                                 reinterpret_cast<LPARAM>(L""));
                }
            }
        } else {
            // No recognisable overlap — full redraw (truncation / replacement)
            SetWindowTextW(g_hRichEdit, L"");
            COLORREF color = g_colors[g_colorIndex];
            AppendColored(g_hRichEdit, newContent, color);
        }

        // Restore drawing
        SendMessageW(g_hRichEdit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_hRichEdit, nullptr, TRUE);

        if (wasAtBottom)
            ScrollToBottom(g_hRichEdit);

        g_lastContent = newContent;
    }

    g_lastSize = fileSize;
    UpdateStatusBar(static_cast<int>(lines.size()), fileSize);
}

// ── Window procedure ────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE: {
        // Create status bar (plain static control — no comctl32 dependency)
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        g_hStatusBar = CreateWindowExW(
            WS_EX_STATICEDGE, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, rcClient.bottom - STATUS_HEIGHT, rcClient.right, STATUS_HEIGHT,
            hWnd,
            reinterpret_cast<HMENU>(IDC_STATUSBAR),
            GetModuleHandleW(nullptr), nullptr);

        // Create RichEdit control
        g_hRichEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 100, 100, hWnd,
            reinterpret_cast<HMENU>(IDC_RICHEDIT),
            GetModuleHandleW(nullptr), nullptr);

        // Set font and colours
        HFONT hFont = CreateFontW(
            -MulDiv(10, GetDeviceCaps(GetDC(hWnd), LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(g_hRichEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        // Background colour → black
        SendMessageW(g_hRichEdit, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(RGB(0, 0, 0)));

        // Disable URL auto-detection
        SendMessageW(g_hRichEdit, EM_AUTOURLDETECT, FALSE, 0);

        // Increase text limit (default is 32 KB)
        SendMessageW(g_hRichEdit, EM_EXLIMITTEXT, 0, 4 * 1024 * 1024);

        // Initial load
        UpdateContent();

        // Scroll to bottom on initial load
        ScrollToBottom(g_hRichEdit);

        // Start timer
        SetTimer(hWnd, TIMER_ID, TIMER_INTERVAL, nullptr);
        return 0;
    }

    case WM_SIZE: {
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        MoveWindow(g_hStatusBar, 0, rcClient.bottom - STATUS_HEIGHT,
                   rcClient.right, STATUS_HEIGHT, TRUE);
        MoveWindow(g_hRichEdit, 0, 0,
                   rcClient.right, rcClient.bottom - STATUS_HEIGHT, TRUE);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_ID)
            UpdateContent();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── Entry point ─────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
    // Parse command line: ColorTail.exe <filepath> [lines]
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc < 2) {
        // No file argument — show an Open File dialog
        wchar_t fileBuf[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = L"All Files\0*.*\0Text Files\0*.txt;*.log\0";
        ofn.lpstrFile   = fileBuf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrTitle  = L"ColorTail - Select File to Monitor";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) {
            LocalFree(argv);
            return 0;
        }
        g_filePath = fileBuf;
    } else {
        g_filePath = argv[1];
        if (argc >= 3) {
            g_lineCount = _wtoi(argv[2]);
            if (g_lineCount <= 0) g_lineCount = DEFAULT_LINES;
        }
    }
    LocalFree(argv);

    // Verify file exists
    DWORD attrs = GetFileAttributesW(g_filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::wstring msg = L"File not found: " + g_filePath;
        MessageBoxW(nullptr, msg.c_str(), L"ColorTail", MB_ICONERROR);
        return 1;
    }

    // Load RichEdit library
    g_hRichEditLib = LoadLibraryW(L"msftedit.dll");
    if (!g_hRichEditLib) {
        MessageBoxW(nullptr, L"Failed to load msftedit.dll (RichEdit)",
                    L"ColorTail", MB_ICONERROR);
        return 1;
    }

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ColorTailClass";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // Build window title
    std::wstring title = L"Tail Viewer - " + g_filePath;

    // Create main window (800×600 client area)
    RECT rc = { 0, 0, 800, 600 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hWnd = CreateWindowExW(
        0, L"ColorTailClass", title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FreeLibrary(g_hRichEditLib);
    return static_cast<int>(msg.wParam);
}
