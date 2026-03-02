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

// ── Version ─────────────────────────────────────────────────────────────────
static constexpr const wchar_t* APP_VERSION = L"1.0.0";

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
static bool          g_paused       = false;
static int           g_lastLineCount = 0;

// ── Find dialog ─────────────────────────────────────────────────────────────
static constexpr int IDC_FIND_EDIT = 201;
static std::wstring  g_findText;

static INT_PTR CALLBACK FindDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, IDC_FIND_EDIT, g_findText.c_str());
        SendDlgItemMessageW(hDlg, IDC_FIND_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(hDlg, IDC_FIND_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf[256] = {};
            GetDlgItemTextW(hDlg, IDC_FIND_EDIT, buf, _countof(buf));
            g_findText = buf;
            EndDialog(hDlg, 1);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void FindNext(HWND hRich)
{
    if (g_findText.empty()) return;

    // Get current selection end as search start
    CHARRANGE cr = {};
    SendMessageW(hRich, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));

    int textLen = GetWindowTextLengthW(hRich);

    FINDTEXTEXW ft = {};
    ft.chrg.cpMin = cr.cpMax;
    ft.chrg.cpMax = textLen;
    ft.lpstrText = g_findText.c_str();

    LRESULT pos = SendMessageW(hRich, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&ft));

    // Wrap around if not found
    if (pos == -1 && ft.chrg.cpMin > 0) {
        ft.chrg.cpMin = 0;
        ft.chrg.cpMax = cr.cpMax;
        pos = SendMessageW(hRich, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&ft));
    }

    if (pos != -1) {
        SendMessageW(hRich, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&ft.chrgText));
        SendMessageW(hRich, EM_SCROLLCARET, 0, 0);
    } else {
        MessageBoxW(GetParent(hRich), L"Text not found.", L"Find", MB_ICONINFORMATION);
    }
}

static void ShowFindDialog(HWND hParent)
{
    alignas(4) BYTE buf[512] = {};
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(buf);
    dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg->cdit = 3;
    dlg->cx = 200;
    dlg->cy = 50;

    BYTE* p = buf + sizeof(DLGTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // menu
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // class
    const wchar_t* title = L"Find";
    size_t titleLen = (wcslen(title) + 1) * sizeof(wchar_t);
    memcpy(p, title, titleLen); p += titleLen;

    p = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~3);

    // Static label
    auto* ctrl = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    ctrl->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    ctrl->x = 7; ctrl->y = 7; ctrl->cx = 30; ctrl->cy = 10;
    ctrl->id = 0xFFFF;
    p += sizeof(DLGITEMTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0xFFFF; p += 2;
    *reinterpret_cast<WORD*>(p) = 0x0082; p += 2;
    const wchar_t* label = L"Find:";
    size_t labelLen = (wcslen(label) + 1) * sizeof(wchar_t);
    memcpy(p, label, labelLen); p += labelLen;
    *reinterpret_cast<WORD*>(p) = 0; p += 2;

    p = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~3);

    // Edit box
    ctrl = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    ctrl->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    ctrl->x = 40; ctrl->y = 5; ctrl->cx = 100; ctrl->cy = 14;
    ctrl->id = IDC_FIND_EDIT;
    p += sizeof(DLGITEMTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0xFFFF; p += 2;
    *reinterpret_cast<WORD*>(p) = 0x0081; p += 2;
    *reinterpret_cast<WORD*>(p) = 0; p += 2;
    *reinterpret_cast<WORD*>(p) = 0; p += 2;

    p = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~3);

    // Find Next button (default)
    ctrl = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    ctrl->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    ctrl->x = 150; ctrl->y = 5; ctrl->cx = 42; ctrl->cy = 14;
    ctrl->id = IDOK;
    p += sizeof(DLGITEMTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0xFFFF; p += 2;
    *reinterpret_cast<WORD*>(p) = 0x0080; p += 2;
    const wchar_t* okText = L"Find Next";
    size_t okLen = (wcslen(okText) + 1) * sizeof(wchar_t);
    memcpy(p, okText, okLen); p += okLen;
    *reinterpret_cast<WORD*>(p) = 0; p += 2;

    if (DialogBoxIndirectW(GetModuleHandleW(nullptr), dlg, hParent, FindDlgProc) == 1) {
        FindNext(g_hRichEdit);
    }
}

// ── Go To Line dialog ───────────────────────────────────────────────────────
static constexpr int IDC_GOTO_EDIT = 200;

static INT_PTR CALLBACK GoToLineDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        SetFocus(GetDlgItem(hDlg, IDC_GOTO_EDIT));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buf[16] = {};
            GetDlgItemTextW(hDlg, IDC_GOTO_EDIT, buf, _countof(buf));
            int line = _wtoi(buf);
            EndDialog(hDlg, line);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, -1);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void GoToLine(HWND hParent)
{
    // Build dialog template in memory
    #pragma pack(push, 4)
    struct {
        DLGTEMPLATE dlg;
        WORD menu, cls, title;
        // controls follow via padding
    } tmplBuf = {};
    #pragma pack(pop)

    // We'll use DialogBoxIndirectParam with a larger buffer
    alignas(4) BYTE buf[512] = {};
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(buf);
    dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg->cdit = 3; // 3 controls: label, edit, OK button
    dlg->cx = 150;
    dlg->cy = 50;

    BYTE* p = buf + sizeof(DLGTEMPLATE);
    // menu, class, title (all empty)
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // menu
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // class
    // title: "Go To Line"
    const wchar_t* title = L"Go To Line";
    size_t titleLen = (wcslen(title) + 1) * sizeof(wchar_t);
    memcpy(p, title, titleLen); p += titleLen;

    // Align to DWORD
    p = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~3);

    // Control 1: Static label
    auto* ctrl = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    ctrl->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    ctrl->x = 7; ctrl->y = 7; ctrl->cx = 50; ctrl->cy = 10;
    ctrl->id = 0xFFFF;
    p += sizeof(DLGITEMTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0xFFFF; p += 2;
    *reinterpret_cast<WORD*>(p) = 0x0082; p += 2; // static class
    const wchar_t* label = L"Line:";
    size_t labelLen = (wcslen(label) + 1) * sizeof(wchar_t);
    memcpy(p, label, labelLen); p += labelLen;
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // creation data

    p = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~3);

    // Control 2: Edit box
    ctrl = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    ctrl->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_NUMBER;
    ctrl->x = 60; ctrl->y = 5; ctrl->cx = 80; ctrl->cy = 14;
    ctrl->id = IDC_GOTO_EDIT;
    p += sizeof(DLGITEMTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0xFFFF; p += 2;
    *reinterpret_cast<WORD*>(p) = 0x0081; p += 2; // edit class
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // no text
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // creation data

    p = reinterpret_cast<BYTE*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~3);

    // Control 3: OK button (default)
    ctrl = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    ctrl->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    ctrl->x = 50; ctrl->y = 28; ctrl->cx = 50; ctrl->cy = 14;
    ctrl->id = IDOK;
    p += sizeof(DLGITEMTEMPLATE);
    *reinterpret_cast<WORD*>(p) = 0xFFFF; p += 2;
    *reinterpret_cast<WORD*>(p) = 0x0080; p += 2; // button class
    const wchar_t* okText = L"OK";
    size_t okLen = (wcslen(okText) + 1) * sizeof(wchar_t);
    memcpy(p, okText, okLen); p += okLen;
    *reinterpret_cast<WORD*>(p) = 0; p += 2; // creation data

    INT_PTR result = DialogBoxIndirectW(GetModuleHandleW(nullptr), dlg, hParent, GoToLineDlgProc);
    if (result > 0) {
        int line = static_cast<int>(result) - 1; // 0-based
        int totalLines = static_cast<int>(SendMessageW(g_hRichEdit, EM_GETLINECOUNT, 0, 0));
        if (line < 0) line = 0;
        if (line >= totalLines) line = totalLines - 1;
        int charIdx = static_cast<int>(SendMessageW(g_hRichEdit, EM_LINEINDEX, line, 0));
        SendMessageW(g_hRichEdit, EM_SETSEL, charIdx, charIdx);
        SendMessageW(g_hRichEdit, EM_SCROLLCARET, 0, 0);
    }
}

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
    swprintf_s(status, L"%sLast updated: %s | Size: %.2f KB | Lines shown: %d",
               g_paused ? L"PAUSED | " : L"", timeBuf, sizeKB, lineCount);

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
    g_lastLineCount = static_cast<int>(lines.size());
    UpdateStatusBar(g_lastLineCount, fileSize);
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
        if (wParam == TIMER_ID && !g_paused)
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

    if (argc >= 2 && (wcscmp(argv[1], L"/?") == 0 || wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0)) {
        std::wstring helpMsg = std::wstring(L"ColorTail v") + APP_VERSION + L"\n\n"
                    L"Usage: ColorTail.exe [filepath] [lines]\n\n"
                    L"  filepath   Path to the file to monitor\n"
                    L"  lines      Number of tail lines (default 50)\n\n"
                    L"If no filepath is given, a file open dialog is shown.";
        MessageBoxW(nullptr, helpMsg.c_str(), L"ColorTail", MB_ICONINFORMATION);
        LocalFree(argv);
        return 0;
    }

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
    std::wstring title = std::wstring(L"ColorTail v") + APP_VERSION + L" - " + g_filePath;

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
        // Space toggles pause/resume
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE) {
            g_paused = !g_paused;
            UpdateStatusBar(g_lastLineCount, g_lastSize);
            continue;
        }
        // Ctrl+F: Find
        if (msg.message == WM_KEYDOWN && msg.wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            ShowFindDialog(hWnd);
            continue;
        }
        // F3: Find Next
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F3) {
            FindNext(g_hRichEdit);
            continue;
        }
        // Ctrl+End: Jump to bottom
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_END && (GetKeyState(VK_CONTROL) & 0x8000)) {
            ScrollToBottom(g_hRichEdit);
            continue;
        }
        // Ctrl+G: Go To Line
        if (msg.message == WM_KEYDOWN && msg.wParam == 'G' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            GoToLine(hWnd);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FreeLibrary(g_hRichEditLib);
    return static_cast<int>(msg.wParam);
}
