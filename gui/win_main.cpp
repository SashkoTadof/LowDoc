#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include "lowdoc/engine.hpp"
#include "../resources/resource.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <iomanip>
#include <sstream>

static HWND g_hwnd = nullptr;
static HWND g_lbl_file = nullptr;
static HWND g_edit_path = nullptr;
static HWND g_btn_browse = nullptr;
static HWND g_grp_results = nullptr;
static HWND g_lbl_status = nullptr;
static HWND g_lbl_before = nullptr;
static HWND g_val_before = nullptr;
static HWND g_lbl_after = nullptr;
static HWND g_val_after = nullptr;
static HWND g_lbl_saved = nullptr;
static HWND g_val_saved = nullptr;
static HWND g_lbl_ratio = nullptr;
static HWND g_val_ratio = nullptr;
static HWND g_chk_context_menu = nullptr;
static HWND g_btn_open_folder = nullptr;
static std::wstring g_last_output_dir;

static bool g_is_dark = false;
static HBRUSH g_bg_brush = nullptr;
static HBRUSH g_edit_brush = nullptr;
static HFONT g_font = nullptr;
static HFONT g_font_bold = nullptr;

struct UiStrings {
    const wchar_t* lbl_file;
    const wchar_t* btn_browse;
    const wchar_t* placeholder;
    const wchar_t* grp_results;
    const wchar_t* status_idle;
    const wchar_t* status_optimizing;
    const wchar_t* status_completed;
    const wchar_t* status_optimal;
    const wchar_t* lbl_before;
    const wchar_t* lbl_after;
    const wchar_t* lbl_saved;
    const wchar_t* lbl_ratio;
    const wchar_t* chk_context_menu;
    const wchar_t* btn_open_folder;
    const wchar_t* ofn_filter;
};

static int g_force_lang = -1;

static bool is_russian_ui() {
    if (g_force_lang != -1) {
        return g_force_lang == 1;
    }
    const wchar_t* env = _wgetenv(L"LOWDOC_LANG");
    if (env) {
        if (_wcsicmp(env, L"en") == 0 || _wcsicmp(env, L"english") == 0) return false;
        if (_wcsicmp(env, L"ru") == 0 || _wcsicmp(env, L"russian") == 0) return true;
    }
    LANGID lang = GetUserDefaultUILanguage();
    return PRIMARYLANGID(lang) == LANG_RUSSIAN;
}

static bool is_dark_mode() {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER,
                             L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                             0, KEY_READ, &hKey);
    if (res != ERROR_SUCCESS) return false;

    DWORD val = 1;
    DWORD size = sizeof(val);
    DWORD type = REG_DWORD;
    res = RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&val), &size);
    RegCloseKey(hKey);

    return (res == ERROR_SUCCESS && val == 0);
}

static UiStrings get_strings(bool ru) {
    if (ru) {
        return {
            L"Файл:",
            L"Обзор...",
            L"Перетащите документ сюда или нажмите «Обзор»",
            L"Параметры оптимизации",
            L"Готов к работе",
            L"Оптимизация документа...",
            L"Оптимизация завершена успешно",
            L"Файл уже максимально сжат без потерь",
            L"Исходный размер:",
            L"После оптимизации:",
            L"Сэкономлено:",
            L"Сжатие:",
            L"Интеграция в контекстное меню Проводника",
            L"Открыть папку",
            L"Поддерживаемые документы (*.docx;*.xlsx;*.pptx;*.pdf;*.odt;*.epub;*.rtf)\0*.docx;*.docm;*.dotx;*.dotm;*.xlsx;*.xlsm;*.pptx;*.pptm;*.pdf;*.odt;*.ods;*.odp;*.epub;*.rtf;*.png;*.jpg;*.jpeg;*.svg\0Все файлы (*.*)\0*.*\0"
        };
    }
    return {
        L"File:",
        L"Browse...",
        L"Drop document here or click Browse...",
        L"Optimization Details",
        L"Ready",
        L"Optimizing document...",
        L"Optimization completed successfully",
        L"File is already optimally compressed",
        L"Original size:",
        L"Optimized size:",
        L"Space saved:",
        L"Reduction:",
        L"Add to Windows Explorer context menu",
        L"Open folder",
        L"Supported Documents (*.docx;*.xlsx;*.pptx;*.pdf;*.odt;*.epub;*.rtf)\0*.docx;*.docm;*.dotx;*.dotm;*.xlsx;*.xlsm;*.pptx;*.pptm;*.pdf;*.odt;*.ods;*.odp;*.epub;*.rtf;*.png;*.jpg;*.jpeg;*.svg\0All Files (*.*)\0*.*\0"
    };
}

static std::wstring get_shell_reg_path(bool cmd = false) {
    std::wstring p = L"Software";
    p += L"\\Classes\\*";
    p += L"\\shell\\LowDoc";
    if (cmd) p += L"\\command";
    return p;
}

static bool is_context_menu_enabled() {
    HKEY hKey = nullptr;
    auto path = get_shell_reg_path();
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &hKey);
    if (res == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

static bool set_context_menu_enabled(bool enable, bool ru) {
    auto cmd_path = get_shell_reg_path(true);
    auto shell_path = get_shell_reg_path(false);

    if (!enable) {
        RegDeleteKeyW(HKEY_CURRENT_USER, cmd_path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, shell_path.c_str());
        return true;
    }

    wchar_t exe_path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    HKEY hKey = nullptr;
    LONG res = RegCreateKeyExW(HKEY_CURRENT_USER, shell_path.c_str(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (res != ERROR_SUCCESS) return false;

    const wchar_t* menu_text = ru ? L"Сжать с помощью LowDoc" : L"Compress with LowDoc";
    RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(menu_text),
                   static_cast<DWORD>((wcslen(menu_text) + 1) * sizeof(wchar_t)));

    std::wstring icon_val = std::wstring(L"\"") + exe_path + L"\",0";
    RegSetValueExW(hKey, L"Icon", 0, REG_SZ, reinterpret_cast<const BYTE*>(icon_val.c_str()),
                   static_cast<DWORD>((icon_val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);

    HKEY hCmdKey = nullptr;
    res = RegCreateKeyExW(HKEY_CURRENT_USER, cmd_path.c_str(), 0, nullptr,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hCmdKey, nullptr);
    if (res != ERROR_SUCCESS) return false;

    std::wstring cmd_val = std::wstring(L"\"") + exe_path + L"\" --context-menu \"%1\"";
    RegSetValueExW(hCmdKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd_val.c_str()),
                   static_cast<DWORD>((cmd_val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hCmdKey);
    return true;
}

static std::wstring format_bytes(size_t bytes, bool ru) {
    std::wostringstream oss;
    const wchar_t* b_unit = ru ? L" Б" : L" B";
    const wchar_t* kb_unit = ru ? L" КБ" : L" KB";
    const wchar_t* mb_unit = ru ? L" МБ" : L" MB";

    if (bytes < 1024) {
        oss << bytes << b_unit;
    } else if (bytes < 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << kb_unit;
    } else {
        oss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << mb_unit;
    }
    return oss.str();
}

static void update_theme_resources(HWND hwnd) {
    g_is_dark = is_dark_mode();

    if (g_bg_brush) DeleteObject(g_bg_brush);
    if (g_edit_brush) DeleteObject(g_edit_brush);

    COLORREF bg_color = g_is_dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
    COLORREF edit_bg_color = g_is_dark ? RGB(45, 45, 45) : RGB(255, 255, 255);

    g_bg_brush = CreateSolidBrush(bg_color);
    g_edit_brush = CreateSolidBrush(edit_bg_color);

    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g_bg_brush));

    BOOL use_dark = g_is_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(hwnd, 19, &use_dark, sizeof(use_dark));

    if (g_chk_context_menu) {
        SetWindowTheme(g_chk_context_menu, g_is_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    if (g_grp_results) {
        SetWindowTheme(g_grp_results, g_is_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }

    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
}

static void process_files(const std::vector<std::wstring>& file_paths) {
    if (file_paths.empty()) return;

    bool ru = is_russian_ui();
    UiStrings s = get_strings(ru);

    SetWindowTextW(g_edit_path, file_paths[0].c_str());
    SetWindowTextW(g_lbl_status, s.status_optimizing);
    SetWindowTextW(g_val_before, L"-");
    SetWindowTextW(g_val_after, L"-");
    SetWindowTextW(g_val_saved, L"-");
    SetWindowTextW(g_val_ratio, L"-");
    ShowWindow(g_btn_open_folder, SW_HIDE);
    UpdateWindow(g_hwnd);

    size_t total_before = 0;
    size_t total_after = 0;
    size_t optimized_count = 0;
    std::wstring last_out_file;

    lowdoc::OptimizationOptions options;

    for (const auto& wpath : file_paths) {
        std::filesystem::path p(wpath);
        auto rep = lowdoc::OptimizationEngine::optimize_file(p, options);
        total_before += rep.original_size;

        if (rep.success) {
            optimized_count++;
            total_after += rep.optimized_size;
            last_out_file = rep.output_file_path.wstring();
        } else {
            total_after += rep.original_size;
        }
    }

    if (!last_out_file.empty()) {
        g_last_output_dir = std::filesystem::path(last_out_file).parent_path().wstring();
    } else if (!file_paths.empty()) {
        g_last_output_dir = std::filesystem::path(file_paths[0]).parent_path().wstring();
    }

    if (optimized_count > 0 && total_before > total_after) {
        size_t saved = total_before - total_after;
        double pct = (1.0 - (static_cast<double>(total_after) / static_cast<double>(total_before))) * 100.0;

        std::wostringstream pct_oss;
        pct_oss << std::fixed << std::setprecision(1) << pct << L"%";

        SetWindowTextW(g_lbl_status, s.status_completed);
        SetWindowTextW(g_val_before, format_bytes(total_before, ru).c_str());
        SetWindowTextW(g_val_after, format_bytes(total_after, ru).c_str());
        SetWindowTextW(g_val_saved, format_bytes(saved, ru).c_str());
        SetWindowTextW(g_val_ratio, pct_oss.str().c_str());
        ShowWindow(g_btn_open_folder, SW_SHOW);
    } else {
        SetWindowTextW(g_lbl_status, s.status_optimal);
        SetWindowTextW(g_val_before, format_bytes(total_before, ru).c_str());
        SetWindowTextW(g_val_after, format_bytes(total_after, ru).c_str());
        SetWindowTextW(g_val_saved, L"0 B");
        SetWindowTextW(g_val_ratio, L"0.0%");
        ShowWindow(g_btn_open_folder, SW_HIDE);
    }
}

static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        bool dark = is_dark_mode();
        COLORREF bg_col = dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
        COLORREF text_col = dark ? RGB(235, 235, 235) : RGB(25, 25, 25);
        SetBkColor(hdc, bg_col);
        SetTextColor(hdc, text_col);
        static HBRUSH hStaticBrush = nullptr;
        if (hStaticBrush) DeleteObject(hStaticBrush);
        hStaticBrush = CreateSolidBrush(bg_col);
        return reinterpret_cast<LRESULT>(hStaticBrush);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void run_progress_dialog(HINSTANCE hInstance, HICON hIcon, const std::vector<std::wstring>& file_paths) {
    if (file_paths.empty()) return;

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    bool ru = is_russian_ui();
    bool dark = is_dark_mode();

    HBRUSH hDlgBrush = CreateSolidBrush(dark ? RGB(32, 32, 32) : RGB(243, 243, 243));
    const wchar_t PROGRESS_WINDOW_CLASS[] = L"LowDocProgressClass";
    WNDCLASSW pwc{};
    pwc.lpfnWndProc = ProgressWndProc;
    pwc.hInstance = hInstance;
    pwc.lpszClassName = PROGRESS_WINDOW_CLASS;
    pwc.hIcon = hIcon;
    pwc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pwc.hbrBackground = hDlgBrush;
    RegisterClassW(&pwc);

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD dwExStyle = WS_EX_TOPMOST;

    RECT rc{0, 0, 420, 92};
    AdjustWindowRectEx(&rc, dwStyle, FALSE, dwExStyle);
    int win_width = rc.right - rc.left;
    int win_height = rc.bottom - rc.top;

    int screen_x = (GetSystemMetrics(SM_CXSCREEN) - win_width) / 2;
    int screen_y = (GetSystemMetrics(SM_CYSCREEN) - win_height) / 2;

    HWND hDlg = CreateWindowExW(
        dwExStyle,
        PROGRESS_WINDOW_CLASS,
        ru ? L"LowDoc - Оптимизация" : L"LowDoc - Optimizing",
        dwStyle,
        screen_x, screen_y, win_width, win_height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hDlg) {
        DeleteObject(hDlgBrush);
        return;
    }

    if (hIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }

    BOOL use_dark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hDlg, 20, &use_dark, sizeof(use_dark));
    DwmSetWindowAttribute(hDlg, 19, &use_dark, sizeof(use_dark));

    HFONT dlg_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HWND hLbl = CreateWindowExW(
        0, L"STATIC",
        ru ? L"Оптимизация документа..." : L"Optimizing document...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        24, 18, 372, 20,
        hDlg, nullptr, hInstance, nullptr
    );
    SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(dlg_font), TRUE);

    HWND hProgress = CreateWindowExW(
        0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
        24, 46, 372, 16,
        hDlg, nullptr, hInstance, nullptr
    );
    SendMessageW(hProgress, PBM_SETMARQUEE, TRUE, 30);
    SetWindowTheme(hProgress, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    size_t total_before = 0;
    size_t total_after = 0;
    size_t optimized_count = 0;

    std::atomic<bool> done{false};
    std::thread worker([&]() {
        lowdoc::OptimizationOptions options;
        for (const auto& wpath : file_paths) {
            std::filesystem::path p(wpath);
            auto rep = lowdoc::OptimizationEngine::optimize_file(p, options);
            total_before += rep.original_size;
            if (rep.success) {
                optimized_count++;
                total_after += rep.optimized_size;
            } else {
                total_after += rep.original_size;
            }
        }
        done = true;
    });

    MSG msg{};
    while (!done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(15);
    }

    if (worker.joinable()) worker.join();

    SendMessageW(hProgress, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(hProgress, PBM_SETPOS, 100, 0);

    std::wstring result_text;
    if (optimized_count > 0 && total_before > total_after) {
        size_t saved = total_before - total_after;
        double pct = (1.0 - (static_cast<double>(total_after) / static_cast<double>(total_before))) * 100.0;
        std::wostringstream oss;
        if (ru) {
            oss << L"Успешно сжато! Очищено: " << format_bytes(saved, true) << L" (" << std::fixed << std::setprecision(1) << pct << L"%)";
        } else {
            oss << L"Successfully optimized! Saved: " << format_bytes(saved, false) << L" (" << std::fixed << std::setprecision(1) << pct << L"%)";
        }
        result_text = oss.str();
    } else {
        result_text = ru ? L"Файл уже максимально оптимизирован" : L"File is already optimally compressed";
    }

    SetWindowTextW(hLbl, result_text.c_str());

    DWORD start_time = GetTickCount();
    while (GetTickCount() - start_time < 2200) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_LBUTTONDOWN || msg.message == WM_KEYDOWN) {
                start_time = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    DestroyWindow(hDlg);
    DeleteObject(hDlgBrush);
    if (dlg_font) DeleteObject(dlg_font);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            bool ru = is_russian_ui();
            UiStrings s = get_strings(ru);

            g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_font_bold = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            g_lbl_file = CreateWindowW(L"STATIC", s.lbl_file,
                                       WS_CHILD | WS_VISIBLE,
                                       20, 22, 45, 20, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_lbl_file, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_edit_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", s.placeholder,
                                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                          70, 18, 300, 26, hwnd, (HMENU)201, nullptr, nullptr);
            SendMessageW(g_edit_path, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_btn_browse = CreateWindowW(L"BUTTON", s.btn_browse,
                                         WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                         380, 18, 80, 26, hwnd, (HMENU)101, nullptr, nullptr);
            SendMessageW(g_btn_browse, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_grp_results = CreateWindowW(L"STATIC", s.grp_results,
                                          WS_CHILD | WS_VISIBLE,
                                          20, 60, 440, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_grp_results, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);

            g_lbl_status = CreateWindowW(L"STATIC", s.status_idle,
                                         WS_CHILD | WS_VISIBLE,
                                         20, 84, 440, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_lbl_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_lbl_before = CreateWindowW(L"STATIC", s.lbl_before,
                                         WS_CHILD | WS_VISIBLE,
                                         20, 110, 150, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_lbl_before, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_val_before = CreateWindowW(L"STATIC", L"-",
                                         WS_CHILD | WS_VISIBLE,
                                         170, 110, 290, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_val_before, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_lbl_after = CreateWindowW(L"STATIC", s.lbl_after,
                                        WS_CHILD | WS_VISIBLE,
                                        20, 134, 150, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_lbl_after, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_val_after = CreateWindowW(L"STATIC", L"-",
                                        WS_CHILD | WS_VISIBLE,
                                        170, 134, 290, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_val_after, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_lbl_saved = CreateWindowW(L"STATIC", s.lbl_saved,
                                        WS_CHILD | WS_VISIBLE,
                                        20, 158, 150, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_lbl_saved, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_val_saved = CreateWindowW(L"STATIC", L"-",
                                        WS_CHILD | WS_VISIBLE,
                                        170, 158, 290, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_val_saved, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);

            g_lbl_ratio = CreateWindowW(L"STATIC", s.lbl_ratio,
                                        WS_CHILD | WS_VISIBLE,
                                        20, 182, 150, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_lbl_ratio, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            g_val_ratio = CreateWindowW(L"STATIC", L"-",
                                        WS_CHILD | WS_VISIBLE,
                                        170, 182, 290, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_val_ratio, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);

            g_chk_context_menu = CreateWindowW(L"BUTTON", s.chk_context_menu,
                                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                              20, 222, 320, 24, hwnd, (HMENU)103, nullptr, nullptr);
            SendMessageW(g_chk_context_menu, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            if (is_context_menu_enabled()) {
                SendMessageW(g_chk_context_menu, BM_SETCHECK, BST_CHECKED, 0);
            }

            g_btn_open_folder = CreateWindowW(L"BUTTON", s.btn_open_folder,
                                             WS_CHILD | BS_OWNERDRAW,
                                             350, 221, 110, 26, hwnd, (HMENU)102, nullptr, nullptr);
            SendMessageW(g_btn_open_folder, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);

            DragAcceptFiles(hwnd, TRUE);
            update_theme_resources(hwnd);
            break;
        }

        case WM_SETTINGCHANGE: {
            update_theme_resources(hwnd);
            break;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis->CtlType == ODT_BUTTON) {
                bool pressed = (dis->itemState & ODS_SELECTED);
                bool focused = (dis->itemState & ODS_FOCUS);

                COLORREF bg = g_is_dark
                    ? (pressed ? RGB(40, 40, 40) : (focused ? RGB(60, 60, 60) : RGB(50, 50, 50)))
                    : (pressed ? RGB(225, 225, 225) : (focused ? RGB(238, 238, 238) : RGB(250, 250, 250)));
                COLORREF border = g_is_dark
                    ? (focused ? RGB(100, 150, 240) : RGB(75, 75, 75))
                    : (focused ? RGB(0, 120, 215) : RGB(190, 190, 190));
                COLORREF text_color = g_is_dark ? RGB(245, 245, 245) : RGB(20, 20, 20);

                HBRUSH hBrush = CreateSolidBrush(bg);
                HPEN hPen = CreatePen(PS_SOLID, 1, border);
                HGDIOBJ oldBrush = SelectObject(dis->hDC, hBrush);
                HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);

                RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom, 4, 4);

                SelectObject(dis->hDC, oldBrush);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(hBrush);
                DeleteObject(hPen);

                wchar_t text[128] = {0};
                GetWindowTextW(dis->hwndItem, text, 128);

                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, text_color);
                HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0));
                if (hFont) SelectObject(dis->hDC, hFont);

                RECT textRc = dis->rcItem;
                if (pressed) {
                    OffsetRect(&textRc, 0, 1);
                }
                DrawTextW(dis->hDC, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == 101) {
                bool ru = is_russian_ui();
                UiStrings s = get_strings(ru);

                wchar_t szFile[MAX_PATH * 8] = {0};
                OPENFILENAMEW ofn = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                ofn.lpstrFilter = s.ofn_filter;
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

                if (GetOpenFileNameW(&ofn)) {
                    std::vector<std::wstring> files = { szFile };
                    process_files(files);
                }
            } else if (wmId == 102) {
                if (!g_last_output_dir.empty()) {
                    ShellExecuteW(nullptr, L"open", g_last_output_dir.c_str(), nullptr, nullptr, SW_SHOWDEFAULT);
                }
            } else if (wmId == 103) {
                LRESULT state = SendMessageW(g_chk_context_menu, BM_GETCHECK, 0, 0);
                set_context_menu_enabled(state == BST_CHECKED, is_russian_ui());
            }
            break;
        }

        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> dropped_files;

            for (UINT i = 0; i < count; ++i) {
                wchar_t filePath[MAX_PATH] = {0};
                if (DragQueryFileW(hDrop, i, filePath, MAX_PATH)) {
                    dropped_files.push_back(filePath);
                }
            }
            DragFinish(hDrop);

            process_files(dropped_files);
            break;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            COLORREF edit_bg = g_is_dark ? RGB(45, 45, 45) : RGB(255, 255, 255);
            COLORREF edit_tx = g_is_dark ? RGB(240, 240, 240) : RGB(20, 20, 20);
            SetBkColor(hdc, edit_bg);
            SetTextColor(hdc, edit_tx);
            return reinterpret_cast<LRESULT>(g_edit_brush);
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND hCtrl = reinterpret_cast<HWND>(lParam);

            COLORREF bg_col = g_is_dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
            COLORREF text_col = g_is_dark ? RGB(235, 235, 235) : RGB(25, 25, 25);

            if (hCtrl == g_val_saved || hCtrl == g_val_ratio) {
                text_col = g_is_dark ? RGB(85, 225, 135) : RGB(15, 135, 55);
            } else if (hCtrl == g_lbl_status) {
                text_col = g_is_dark ? RGB(160, 160, 160) : RGB(100, 100, 100);
            } else if (hCtrl == g_chk_context_menu) {
                text_col = g_is_dark ? RGB(215, 215, 215) : RGB(40, 40, 40);
            } else if (hCtrl == g_lbl_before || hCtrl == g_lbl_after || hCtrl == g_lbl_saved || hCtrl == g_lbl_ratio) {
                text_col = g_is_dark ? RGB(180, 180, 180) : RGB(85, 85, 85);
            }

            SetTextColor(hdc, text_col);
            SetBkColor(hdc, bg_col);
            return reinterpret_cast<LRESULT>(g_bg_brush);
        }

        case WM_DESTROY:
            if (g_bg_brush) {
                DeleteObject(g_bg_brush);
                g_bg_brush = nullptr;
            }
            if (g_edit_brush) {
                DeleteObject(g_edit_brush);
                g_edit_brush = nullptr;
            }
            if (g_font) {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            if (g_font_bold) {
                DeleteObject(g_font_bold);
                g_font_bold = nullptr;
            }
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool is_context_mode = false;
    std::vector<std::wstring> initial_files;

    if (argv && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--context-menu" || arg == L"-c") {
                is_context_mode = true;
            } else if (arg == L"--en" || arg == L"--english") {
                g_force_lang = 0;
            } else if (arg == L"--ru" || arg == L"--russian") {
                g_force_lang = 1;
            } else if ((arg == L"--lang" || arg == L"-l") && i + 1 < argc) {
                std::wstring val = argv[++i];
                if (val == L"en" || val == L"english") g_force_lang = 0;
                else if (val == L"ru" || val == L"russian") g_force_lang = 1;
            } else {
                initial_files.push_back(arg);
            }
        }
    }
    if (argv) LocalFree(argv);

    if (is_context_mode) {
        if (!initial_files.empty()) {
            run_progress_dialog(hInstance, hAppIcon, initial_files);
        }
        return 0;
    }

    const wchar_t CLASS_NAME[] = L"LowDocWindowClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = hAppIcon;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(is_dark_mode() ? RGB(32, 32, 32) : RGB(243, 243, 243));

    RegisterClassW(&wc);

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, 480, 265};
    AdjustWindowRectEx(&rc, dwStyle, FALSE, 0);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    int screen_x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int screen_y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"LowDoc",
        dwStyle,
        screen_x, screen_y, width, height,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hwnd) return 0;

    if (hAppIcon) {
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    if (!initial_files.empty()) {
        process_files(initial_files);
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
