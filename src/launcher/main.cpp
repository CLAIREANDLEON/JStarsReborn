// JStarsReborn launcher (JStarsReborn.exe): chooses and checks the game folder, prepares the boot image, starts the
// game runtime (runtime\jstars-runtime.exe) for a normal play session and explains how it ended. Win32 + common
// controls only. GPL-2.0-or-later.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "launcher_core.hpp"
#include "online.hpp"
#include "saves.hpp"
#include <fstream>
#include <iterator>
#include <cstring>
#include "reborn/paths.hpp"
#include "reborn/sha256.hpp"

using namespace reborn::launcher;
namespace prep = reborn::prep;

namespace {

// ---- constants ---------------------------------------------------------------------------------------------------------
constexpr wchar_t kWindowClass[] = L"JStarsRebornLauncher";
constexpr wchar_t kGameWindowClass[] = L"JStarsRebornGameWindow"; // created by the runtime (gpu/jgpu_window_win32.cpp)
constexpr wchar_t kMutexName[] = L"Local\\JStarsRebornLauncher.SingleInstance";
constexpr UINT WM_APP_CHECKED = WM_APP + 1, WM_APP_PREP_PROGRESS = WM_APP + 2, WM_APP_PREP_DONE = WM_APP + 3,
               WM_APP_GAME_EXITED = WM_APP + 4, WM_APP_RUNTIME_INFO = WM_APP + 5, WM_APP_ONLINE = WM_APP + 6;
constexpr UINT_PTR TIMER_STOP = 1, TIMER_POLL = 2;
constexpr COLORREF kSidebar = RGB(30, 32, 40), kSidebarHover = RGB(44, 47, 58), kSidebarSel = RGB(52, 56, 70),
                   kAccent = RGB(226, 72, 40), kAccentDark = RGB(186, 52, 24), kContentBg = RGB(246, 247, 249),
                   kText = RGB(28, 30, 36), kMuted = RGB(96, 100, 112), kGood = RGB(24, 128, 64), kBad = RGB(186, 36, 36);

enum Page : int { PagePlay, PageFolder, PageSettings, PageOnline, PageControls, PageSaves, PageHelp, PageCount };
constexpr int kPages = PageCount;
const wchar_t* kPageNames[PageCount] = {L"Play", L"Game folder", L"Settings", L"Online", L"Controls", L"Saves", L"Troubleshooting"};

enum Id {
    IdNavFirst = 100, // + page
    IdPlay = 200, IdStop, IdCancelPrep, IdOpenSessionLogs, IdPrepProgress, IdStatus, IdLastHead, IdLastDetail, IdPrepStep, IdPlayHint,
    IdFolderEdit = 300, IdBrowse, IdRecheck, IdFolderDetails, IdPrepState, IdPrepareNow, IdFolderNote, IdFolderLabel,
    IdWindowSize = 400, IdSound, IdVolume, IdVolumeLabel, IdKeyboard, IdConfirm, IdPlayerName, IdSettingsNote, IdLblWindow, IdLblVolume,
    IdLblConfirm, IdLblName, IdFrameRate60,
    IdOnline = 450, IdOnlineIntro, IdUpnp, IdLblMyCode, IdMyCode, IdFindCode, IdCopyCode, IdLblPeers, IdLobbyPeers, IdOnlineStatus, IdOnlineNote,
    IdControlsList = 500, IdControlsNote,
    IdSavesInfo = 550, IdSaveImportFile, IdSaveImportFolder, IdSaveUnlock, IdSaveBackupList, IdSaveRestore, IdSaveOpenBackups, IdSavesStatus, IdSavesNote,
    IdOpenLatest = 600, IdOpenLogs, IdReport, IdDeletePrepared, IdOpenData, IdLicenses, IdHelpInfo, IdHelpText,
};

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
fs::path exe_dir() {
    std::wstring s(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
    s.resize(n);
    return fs::path(s).parent_path();
}
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}
// Command-line quoting that CommandLineToArgvW reverses exactly.
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++bs; }
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; }
        else { r.append(bs, L'\\'); r += a[i]; }
    }
    return r + L"\"";
}

// ---- application state -----------------------------------------------------------------------------------------------
struct LastExit {
    bool valid = false;
    ExitInfo info;
    fs::path session;
    std::string when;
};

struct App {
    HINSTANCE inst{};
    HWND hwnd{};
    UINT dpi = 96;
    HFONT font{}, font_bold{}, font_title{}, font_small{}, font_nav{}, font_button{}, font_app{};
    HBRUSH content_brush{}, sidebar_brush{};
    std::vector<std::pair<HWND, int>> controls; // control, page (-1 = always)
    int page = PagePlay;
    int hover_nav = -1;

    fs::path exe, data_root, settings_file, sessions_root, user_dir, logs_dir, runtime_exe;
    Settings settings;
    std::mutex log_mutex;

    // game folder check (worker thread)
    GameCheck game;
    bool checking = false;
    std::jthread check_thread;
    std::string prep_state_text;
    bool prepared = false;

    // preparation
    std::jthread prep_thread;
    bool preparing = false;
    bool launch_after_prep = false;
    std::mutex prep_mutex;
    double prep_fraction = 0;
    std::string prep_step;

    // runtime info
    std::string runtime_version = "(checking)";
    std::string runtime_sha256;
    bool vulkan_present = true;

    // session
    HANDLE proc{};
    DWORD pid = 0;
    std::jthread wait_thread;
    bool running = false, stop_requested = false, forced = false, quit_after_exit = false;
    fs::path session_dir;
    std::string session_start;
    ULONGLONG session_tick = 0;
    std::wstring session_cmdline;
    LastExit last;
    bool foreign_game = false; // a game window exists that this launcher did not start

    // online (peer-to-peer; see online.hpp)
    std::jthread online_thread;
    bool online_busy = false, ports_mapped = false;
    online::Igd igd;
    std::string my_code, online_status = "Online play is off.";

    // artwork (the user's own sce_sys/icon0.png, decoded locally)
    HBITMAP icon_bmp{};
    int icon_w = 0, icon_h = 0;
} g;
void refresh_online_page();
void refresh_saves_page();
void start_online_setup();

int S(int v) { return MulDiv(v, static_cast<int>(g.dpi), 96); }

void log_line(const std::string& text) {
    std::scoped_lock lk{g.log_mutex};
    std::error_code ec;
    fs::create_directories(g.logs_dir, ec);
    const fs::path f = g.logs_dir / "launcher.log";
    if (fs::exists(f, ec) && fs::file_size(f, ec) > (1u << 20)) fs::rename(f, g.logs_dir / "launcher.old.log", ec);
    std::ofstream o(f, std::ios::app | std::ios::binary);
    o << utc_stamp(false) << " " << text << "\n";
}

HWND ctl(int id) { return GetDlgItem(g.hwnd, id); }
void set_text(int id, const std::wstring& t) { SetWindowTextW(ctl(id), t.c_str()); }
std::wstring get_text(int id) {
    HWND h = ctl(id);
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<std::size_t>(n));
    return s;
}
void show(int id, bool on) { ShowWindow(ctl(id), on ? SW_SHOW : SW_HIDE); }
void enable(int id, bool on) { EnableWindow(ctl(id), on ? TRUE : FALSE); }

HWND add(int page, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, g.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g.inst, nullptr);
    g.controls.emplace_back(h, page);
    return h;
}

void save_settings_now() {
    try {
        save_settings(g.settings_file, g.settings);
    } catch (const std::exception& e) {
        log_line(std::string("settings: save failed: ") + e.what());
        MessageBoxW(g.hwnd, (L"Your settings could not be saved:\n" + W(e.what())).c_str(), L"JStarsReborn", MB_ICONWARNING);
    }
}

// ---- fonts and layout ----------------------------------------------------------------------------------------------------
HFONT make_font(int pt, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-MulDiv(pt, static_cast<int>(g.dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}
void make_fonts() {
    for (HFONT f : {g.font, g.font_bold, g.font_title, g.font_small, g.font_nav, g.font_button, g.font_app})
        if (f) DeleteObject(f);
    g.font = make_font(10, FW_NORMAL);
    g.font_bold = make_font(10, FW_SEMIBOLD);
    g.font_title = make_font(20, FW_SEMIBOLD, L"Segoe UI Semibold");
    g.font_small = make_font(9, FW_NORMAL);
    g.font_nav = make_font(11, FW_NORMAL);
    g.font_button = make_font(16, FW_SEMIBOLD, L"Segoe UI Semibold");
    g.font_app = make_font(13, FW_SEMIBOLD, L"Segoe UI Semibold");
    for (auto& [h, page] : g.controls) {
        (void)page;
        const int id = GetDlgCtrlID(h);
        HFONT f = g.font;
        if (id == IdLastHead) f = g.font_bold;
        if (id == IdFolderNote || id == IdSettingsNote || id == IdControlsNote || id == IdPlayHint || id == IdPrepStep) f = g.font_small;
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }
}

constexpr int kSidebarW = 210, kPad = 32, kHeaderH = 96;

void place(int id, int x, int y, int w, int h) { SetWindowPos(ctl(id), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); }

void layout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int W_ = rc.right, H_ = rc.bottom;
    for (int p = 0; p < PageCount; ++p) place(IdNavFirst + p, 0, S(96) + p * S(48), S(kSidebarW), S(48));
    const int x0 = S(kSidebarW) + S(kPad), cw = std::max(S(300), W_ - x0 - S(kPad));
    int y = S(kHeaderH) + S(8);
    // Play
    {
        const int art = g.icon_bmp ? S(128) + S(24) : 0;
        place(IdStatus, x0 + art, S(96), cw - art, S(52));
        int py = S(180);
        place(IdPlay, x0, py, S(240), S(64));
        place(IdStop, x0 + S(256), py + S(14), S(150), S(36));
        place(IdPrepProgress, x0, py + S(84), std::min(cw, S(520)), S(10));
        place(IdPrepStep, x0, py + S(100), std::min(cw, S(400)), S(22));
        place(IdCancelPrep, x0 + std::min(cw, S(520)) - S(110), py + S(98), S(110), S(30));
        place(IdLastHead, x0, py + S(150), cw, S(24));
        place(IdLastDetail, x0, py + S(176), cw, S(64));
        place(IdOpenSessionLogs, x0, py + S(244), S(180), S(32));
        place(IdPlayHint, x0, H_ - S(48), cw, S(36));
    }
    // Game folder
    {
        place(IdFolderLabel, x0, y, cw, S(22));
        place(IdFolderEdit, x0, y + S(26), cw - S(250), S(28));
        place(IdBrowse, x0 + cw - S(240), y + S(25), S(116), S(30));
        place(IdRecheck, x0 + cw - S(116), y + S(25), S(116), S(30));
        place(IdFolderDetails, x0, y + S(68), cw, std::max(S(120), H_ - y - S(68) - S(140)));
        place(IdPrepState, x0, H_ - S(126), cw - S(170), S(44));
        place(IdPrepareNow, x0 + cw - S(160), H_ - S(124), S(160), S(32));
        place(IdFolderNote, x0, H_ - S(72), cw, S(48));
    }
    // Settings
    {
        const int lw = S(170), fx = x0 + lw;
        int sy = y + S(4);
        place(IdLblWindow, x0, sy + S(4), lw, S(24)); place(IdWindowSize, fx, sy, S(220), S(300)); sy += S(48);
        place(IdSound, x0, sy, S(400), S(26)); sy += S(38);
        place(IdLblVolume, x0, sy + S(4), lw, S(24)); place(IdVolume, fx, sy, S(260), S(32)); place(IdVolumeLabel, fx + S(268), sy + S(4), S(60), S(24)); sy += S(48);
        place(IdKeyboard, x0, sy, cw, S(26)); sy += S(42);
        place(IdFrameRate60, x0, sy, cw, S(26)); sy += S(42);
        place(IdLblConfirm, x0, sy + S(4), lw, S(24)); place(IdConfirm, fx, sy, S(260), S(300)); sy += S(48);
        place(IdLblName, x0, sy + S(4), lw, S(24)); place(IdPlayerName, fx, sy, S(260), S(28)); sy += S(52);
        place(IdSettingsNote, x0, sy, cw, S(72));
    }
    // Online
    {
        const int lw = S(190), fx = x0 + lw;
        int sy = y + S(4);
        place(IdOnline, x0, sy, cw, S(26)); sy += S(32);
        place(IdOnlineIntro, x0, sy, cw, S(54)); sy += S(60);
        place(IdUpnp, x0, sy, cw, S(26)); sy += S(38);
        place(IdLblMyCode, x0, sy + S(6), lw, S(24)); place(IdMyCode, fx, sy, S(220), S(34));
        place(IdFindCode, fx + S(230), sy, S(170), S(34)); place(IdCopyCode, fx + S(410), sy, S(90), S(34)); sy += S(48);
        place(IdLblPeers, x0, sy + S(4), lw, S(24)); place(IdLobbyPeers, fx, sy, std::max(S(220), cw - lw), S(28)); sy += S(42);
        place(IdOnlineStatus, x0, sy, cw, S(84)); sy += S(90);
        place(IdOnlineNote, x0, sy, cw, std::max(S(60), H_ - sy - S(16)));
    }
    // Controls
    {
        place(IdControlsList, x0, y, cw, std::max(S(200), H_ - y - S(118)));
        place(IdControlsNote, x0, H_ - S(108), cw, S(96));
        HWND lv = ctl(IdControlsList);
        const int c0 = S(170), c1 = S(200);
        ListView_SetColumnWidth(lv, 0, c0);
        ListView_SetColumnWidth(lv, 1, c1);
        ListView_SetColumnWidth(lv, 2, std::max(S(160), cw - c0 - c1 - S(24)));
    }
    // Saves
    {
        int sy = y + S(4);
        place(IdSavesInfo, x0, sy, cw, S(66)); sy += S(74);
        place(IdSaveImportFile, x0, sy, S(250), S(34)); place(IdSaveImportFolder, x0 + S(262), sy, S(250), S(34)); sy += S(46);
        place(IdSaveUnlock, x0, sy, S(250), S(34)); sy += S(54);
        place(IdSaveBackupList, x0, sy, std::min(cw - S(262), S(420)), S(300));
        place(IdSaveRestore, x0 + std::min(cw - S(262), S(420)) + S(12), sy, S(250), S(34)); sy += S(46);
        place(IdSaveOpenBackups, x0, sy, S(250), S(34)); sy += S(50);
        place(IdSavesStatus, x0, sy, cw, S(70)); sy += S(78);
        place(IdSavesNote, x0, sy, cw, std::max(S(60), H_ - sy - S(16)));
    }
    // Troubleshooting
    {
        int hy = y + S(4);
        place(IdOpenLatest, x0, hy, S(250), S(34)); place(IdOpenLogs, x0 + S(262), hy, S(200), S(34)); hy += S(46);
        place(IdReport, x0, hy, S(250), S(34)); place(IdOpenData, x0 + S(262), hy, S(200), S(34)); hy += S(46);
        place(IdDeletePrepared, x0, hy, S(250), S(34)); place(IdLicenses, x0 + S(262), hy, S(200), S(34)); hy += S(56);
        place(IdHelpText, x0, hy, cw, std::max(S(120), H_ - hy - S(24)));
    }
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

void show_page(int p) {
    g.page = p;
    for (auto& [h, page] : g.controls) ShowWindow(h, page < 0 || page == p ? SW_SHOW : SW_HIDE);
    // state-dependent visibility on the Play page
    if (p == PagePlay) {
        show(IdStop, g.running);
        show(IdPrepProgress, g.preparing);
        show(IdPrepStep, g.preparing);
        show(IdCancelPrep, g.preparing);
        show(IdLastHead, g.last.valid);
        show(IdLastDetail, g.last.valid);
        show(IdOpenSessionLogs, g.last.valid);
    }
    if (p == PageSaves) refresh_saves_page();
    for (int i = 0; i < PageCount; ++i) InvalidateRect(ctl(IdNavFirst + i), nullptr, TRUE);
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

// ---- state → UI -------------------------------------------------------------------------------------------------------------
bool can_play() {
    return g.game.ok() && !g.checking && !g.preparing && !g.running && !g.foreign_game && fs::exists(g.runtime_exe) && g.vulkan_present;
}

void refresh_play_page() {
    std::wstring status;
    if (!fs::exists(g.runtime_exe)) status = L"The game runtime is missing from this installation (runtime\\jstars-runtime.exe). Please reinstall JStarsReborn.";
    else if (!g.vulkan_present) status = L"Vulkan was not found. JStarsReborn needs a graphics driver with Vulkan 1.3 support. Update your graphics driver.";
    else if (g.checking) status = L"Checking your game folder\u2026";
    else if (g.running) status = L"Playing. Close the game window to end the session.";
    else if (g.preparing) status = L"Preparing the game for first launch\u2026";
    else if (g.foreign_game) status = L"A game session started earlier is still running. Close it before starting another one.";
    else if (!g.game.ok()) status = W(g.game.summary);
    else status = g.prepared ? L"Ready to play." : L"Ready. The game will be prepared once when you press Play (a few seconds).";
    set_text(IdStatus, status);
    set_text(IdPlay, g.running ? L"Playing\u2026" : g.preparing ? L"Preparing\u2026" : L"Play");
    enable(IdPlay, can_play());
    if (g.page == PagePlay) {
        show(IdStop, g.running);
        show(IdPrepProgress, g.preparing);
        show(IdPrepStep, g.preparing);
        show(IdCancelPrep, g.preparing);
        show(IdLastHead, g.last.valid && !g.running && !g.preparing);
        show(IdLastDetail, g.last.valid && !g.running && !g.preparing);
        show(IdOpenSessionLogs, g.last.valid && !g.running && !g.preparing);
    }
    if (g.last.valid) {
        set_text(IdLastHead, W((g.last.info.normal ? "Last session: " : "Last session ended with a problem: ") + g.last.info.headline));
        set_text(IdLastDetail, W((g.last.info.detail.empty() ? "" : g.last.info.detail + "\r\n") + "Ended " + g.last.when + "."));
    }
    InvalidateRect(ctl(IdPlay), nullptr, TRUE);
    RECT r{S(kSidebarW), 0, 10000, S(176)};
    InvalidateRect(g.hwnd, &r, TRUE);
}

void refresh_folder_page() {
    set_text(IdFolderEdit, g.settings.game_folder.empty() ? L"" : W(g.settings.game_folder));
    std::wstring d;
    if (g.checking) d = L"Checking\u2026";
    else if (g.game.status == GameStatus::NotChosen) d = L"No game folder chosen yet. Click Browse and select the folder that contains eboot.bin and sce_sys (for example E:\\CUSA01593).";
    else {
        for (const auto& line : g.game.details) d += W(line) + L"\r\n";
        if (d.empty()) d = W(g.game.summary);
    }
    set_text(IdFolderDetails, d);
    set_text(IdPrepState, g.game.ok() ? W("Prepared data: " + g.prep_state_text) : L"Prepared data: available after a valid game folder is chosen.");
    enable(IdPrepareNow, g.game.ok() && !g.preparing && !g.running && !g.checking);
    enable(IdBrowse, !g.running && !g.preparing);
    enable(IdRecheck, !g.running && !g.preparing && !g.checking);
}

void refresh_all() {
    refresh_play_page();
    refresh_folder_page();
    enable(IdDeletePrepared, g.game.ok() && !g.running && !g.preparing);
}

// ---- artwork: the user's icon0.png, decoded with WIC -----------------------------------------------------------------------
void load_icon(const fs::path& png) {
    if (g.icon_bmp) { DeleteObject(g.icon_bmp); g.icon_bmp = nullptr; }
    if (png.empty()) return;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w && h && w <= 4096 && h <= 4096) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = static_cast<LONG>(w);
            bi.bmiHeader.biHeight = -static_cast<LONG>(h);
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE*>(bits)))) {
                g.icon_bmp = bmp;
                g.icon_w = static_cast<int>(w);
                g.icon_h = static_cast<int>(h);
            } else if (bmp) {
                DeleteObject(bmp);
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    f->Release();
}

// ---- game folder check ---------------------------------------------------------------------------------------------------------
struct CheckResult { GameCheck game; bool prepared; std::string prep_text; };

void start_check() {
    if (g.checking) return;
    g.checking = true;
    refresh_all();
    const fs::path folder = from_u8(g.settings.game_folder);
    const fs::path data_root = g.data_root;
    HWND hwnd = g.hwnd;
    g.check_thread = std::jthread([folder, data_root, hwnd](std::stop_token st) {
        auto* r = new CheckResult;
        try {
            r->game = check_game_folder(folder, st);
            if (r->game.ok()) {
                std::string why;
                r->prepared = prepared_valid(prepared_dir(data_root, r->game), r->game.inputs, &why);
                r->prep_text = r->prepared ? "ready (made from exactly these game files)." : why + "; it is created when you press Play.";
            }
        } catch (const std::exception& e) {
            r->game.status = GameStatus::Invalid;
            r->game.summary = "The game folder could not be checked.";
            r->game.details.push_back(std::string("Problem: ") + e.what());
        }
        if (!PostMessageW(hwnd, WM_APP_CHECKED, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

// ---- preparation ------------------------------------------------------------------------------------------------------------------
void start_prepare(bool then_launch) {
    if (!g.game.ok() || g.preparing || g.running) return;
    g.preparing = true;
    g.launch_after_prep = then_launch;
    g.prep_fraction = 0;
    g.prep_step = "Starting";
    SendMessageW(ctl(IdPrepProgress), PBM_SETPOS, 0, 0);
    refresh_all();
    show_page(g.page);
    const GameCheck game = g.game;
    const fs::path dir = prepared_dir(g.data_root, game);
    HWND hwnd = g.hwnd;
    log_line("prepare: start -> " + u8(dir));
    g.prep_thread = std::jthread([game, dir, hwnd](std::stop_token st) {
        std::string* err = nullptr;
        try {
            prepare_game(game, dir, [hwnd](double f, const std::string& step) {
                {
                    std::scoped_lock lk{g.prep_mutex};
                    g.prep_fraction = f;
                    g.prep_step = step;
                }
                PostMessageW(hwnd, WM_APP_PREP_PROGRESS, 0, 0);
            }, st);
        } catch (const std::exception& e) {
            err = new std::string(e.what());
        }
        if (!PostMessageW(hwnd, WM_APP_PREP_DONE, 0, reinterpret_cast<LPARAM>(err))) delete err;
    });
}

// ---- runtime info (version, hash, Vulkan presence) -------------------------------------------------------------------------------------
struct RuntimeInfo { std::string version, sha256; bool vulkan; };
void start_runtime_info() {
    const fs::path exe = g.runtime_exe;
    HWND hwnd = g.hwnd;
    std::thread([exe, hwnd] {
        auto* r = new RuntimeInfo{"(not found)", "", true};
        HMODULE vk = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        r->vulkan = vk != nullptr;
        if (vk) FreeLibrary(vk);
        if (fs::exists(exe)) {
            // version: run "jstars-runtime.exe --version" without a window and read its output
            SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
            HANDLE rd = nullptr, wr = nullptr;
            if (CreatePipe(&rd, &wr, &sa, 0)) {
                SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW si{};
                si.cb = sizeof si;
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = wr;
                si.hStdError = wr;
                si.hStdInput = nullptr;
                PROCESS_INFORMATION pi{};
                std::wstring cmd = quote_arg(exe.wstring()) + L" --version";
                if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &si, &pi)) {
                    CloseHandle(wr);
                    wr = nullptr;
                    std::string out;
                    char buf[256];
                    DWORD n = 0;
                    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
                    WaitForSingleObject(pi.hProcess, 5000);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
                    if (!out.empty()) r->version = out;
                }
                if (wr) CloseHandle(wr);
                CloseHandle(rd);
            }
            std::ifstream f(exe, std::ios::binary);
            reborn::PortableSha256 h;
            std::vector<char> b(1 << 20);
            while (f) { f.read(b.data(), static_cast<std::streamsize>(b.size())); if (f.gcount() > 0) h.update(b.data(), static_cast<std::size_t>(f.gcount())); }
            r->sha256 = reborn::PortableSha256::to_hex(h.finish());
        }
        if (!PostMessageW(hwnd, WM_APP_RUNTIME_INFO, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// ---- sessions ------------------------------------------------------------------------------------------------------------------------
HWND find_game_window(DWORD pid) {
    struct Ctx { DWORD pid; HWND found; } ctx{pid, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64];
        if (!GetClassNameW(h, cls, 64) || wcscmp(cls, kGameWindowClass) != 0) return TRUE;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (c->pid == 0 || p == c->pid) { c->found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::string session_json(bool finished, DWORD code, const ExitInfo* info) {
    std::ostringstream o;
    o << "{\n"
      << "  \"launcher_version\": \"" << launcher_version << "\",\n"
      << "  \"runtime_version\": \"" << json_escape(g.runtime_version) << "\",\n"
      << "  \"runtime_sha256\": \"" << g.runtime_sha256 << "\",\n"
      << "  \"game\": {\"title_id\": \"" << json_escape(g.game.title_id) << "\", \"version\": \"" << json_escape(g.game.app_ver)
      << "\", \"supported_build\": \"" << json_escape(g.game.build ? g.game.build->label : "") << "\"},\n"
      << "  \"settings\": {\"window\": \"" << g.settings.window_width << "x" << g.settings.window_height << "\", \"sound\": "
      << (g.settings.sound ? "true" : "false") << ", \"volume\": " << g.settings.volume << ", \"keyboard\": " << (g.settings.keyboard ? "true" : "false")
      << ", \"confirm_button\": \"" << g.settings.confirm_button << "\", \"online\": " << (g.settings.online ? "true" : "false") << "},\n"
      << "  \"mode\": \"normal play: live input (controller/keyboard), no scripted input, no time limit, no capture\",\n"
      << "  \"command_line\": \"" << json_escape(U(g.session_cmdline)) << "\",\n"
      << "  \"started_utc\": \"" << g.session_start << "\",\n";
    if (finished) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(code));
        o << "  \"ended_utc\": \"" << utc_stamp(false) << "\",\n"
          << "  \"duration_seconds\": " << (GetTickCount64() - g.session_tick) / 1000 << ",\n"
          << "  \"exit_code\": " << code << ",\n  \"exit_code_hex\": \"" << hex << "\",\n"
          << "  \"ended_by_launcher\": " << (g.forced ? "true" : "false") << ",\n"
          << "  \"result\": \"" << (info && info->normal ? "normal" : "problem") << "\",\n"
          << "  \"explanation\": \"" << json_escape(info ? (info->detail.empty() ? info->headline : info->headline + " " + info->detail) : "") << "\"\n";
    } else {
        o << "  \"status\": \"running\"\n";
    }
    o << "}\n";
    return o.str();
}
void write_session_json(bool finished, DWORD code, const ExitInfo* info) {
    std::ofstream f(g.session_dir / "session.json", std::ios::binary | std::ios::trunc);
    f << session_json(finished, code, info);
}

std::vector<wchar_t> environment_block() {
    // The current environment minus any JSTARS_* developer variables, plus the settings the runtime reads.
    std::vector<std::wstring> vars;
    if (LPWCH env = GetEnvironmentStringsW()) {
        for (LPWCH p = env; *p; p += wcslen(p) + 1) {
            std::wstring v = p;
            std::wstring up = v.substr(0, 7);
            CharUpperBuffW(up.data(), static_cast<DWORD>(up.size()));
            if (up == L"JSTARS_" || up == L"JGPU_TR") continue;
            vars.push_back(v);
        }
        FreeEnvironmentStringsW(env);
    }
    vars.push_back(L"JSTARS_ENTER_BUTTON=" + W(g.settings.confirm_button));
    vars.push_back(L"JSTARS_USER_NAME=" + W(sanitize_player_name(g.settings.player_name)));
    std::vector<wchar_t> block;
    for (const auto& v : vars) { block.insert(block.end(), v.begin(), v.end()); block.push_back(0); }
    block.push_back(0);
    return block;
}

void launch_game() {
    if (!can_play()) return;
    if (HWND other = find_game_window(0)) {
        (void)other;
        g.foreign_game = true;
        refresh_all();
        return;
    }
    const fs::path prepared = prepared_dir(g.data_root, g.game);
    std::string why;
    if (!prepared_valid(prepared, g.game.inputs, &why)) { start_prepare(true); return; }
    std::error_code ec;
    fs::create_directories(g.sessions_root, ec);
    prune_sessions(g.sessions_root, 30);
    g.session_dir = g.sessions_root / utc_stamp(true);
    for (int i = 2; fs::exists(g.session_dir, ec); ++i) g.session_dir = g.sessions_root / (utc_stamp(true) + "-" + std::to_string(i));
    fs::create_directories(g.session_dir, ec);
    fs::create_directories(g.user_dir, ec);
    if (ec) {
        MessageBoxW(g.hwnd, (L"Could not create the session folder:\n" + g.session_dir.wstring()).c_str(), L"JStarsReborn", MB_ICONERROR);
        return;
    }
    // Normal play: live input, no scripted input, no time limit, no capture, no diagnostics switches.
    std::wstring cmd = quote_arg(g.runtime_exe.wstring());
    cmd += L" --boot " + quote_arg((prepared / "boot-linked.bin").wstring());
    cmd += L" --game-root " + quote_arg(g.game.folder.wstring());
    cmd += L" --user-dir " + quote_arg(g.user_dir.wstring());
    cmd += L" --log game.log --gpu-dir gpu --capture-dir captures";
    cmd += L" --window " + std::to_wstring(g.settings.window_width) + L"x" + std::to_wstring(g.settings.window_height);
    if (g.settings.sound) cmd += L" --volume " + std::to_wstring(g.settings.volume);
    else cmd += L" --no-audio";
    if (!g.settings.keyboard) cmd += L" --no-keyboard";
    if (g.settings.frame_rate == 60) cmd += L" --frame-rate 60"; // experimental; the runtime checks the game build first
    // Experimental online test: real sockets and the LAN room directory. The online probe makes a console online function that
    // is not implemented yet answer "signed out" (the game shows its communication error) instead of stopping the game.
    if (g.settings.online) {
        cmd += L" --online --trace-np --experiment online-probe";
        const std::string peers = online::peers_to_lobby_arg(g.settings.lobby_peers);
        if (!peers.empty()) cmd += L" --lobby-peer " + W(peers);
        if (g.settings.upnp && !g.ports_mapped && !g.online_busy) start_online_setup();
    }
    g.session_cmdline = cmd;

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW((g.session_dir / "stdout.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileW((g.session_dir / "stderr.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    HANDLE inherit[2] = {out, err};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    auto env = environment_block();
    std::wstring cmd_mut = cmd;
    g.session_start = utc_stamp(false);
    g.session_tick = GetTickCount64();
    const BOOL ok = CreateProcessW(g.runtime_exe.c_str(), cmd_mut.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, env.data(),
                                   g.session_dir.c_str(), &si.StartupInfo, &pi);
    const DWORD le = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(out);
    CloseHandle(err);
    if (!ok) {
        log_line("launch failed: error " + std::to_string(le));
        wchar_t msg[512] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, le, 0, msg, 512, nullptr);
        MessageBoxW(g.hwnd, (L"The game could not be started:\n" + std::wstring(msg)).c_str(), L"JStarsReborn", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    g.proc = pi.hProcess;
    g.pid = pi.dwProcessId;
    g.running = true;
    g.stop_requested = g.forced = false;
    write_session_json(false, 0, nullptr);
    log_line("session start " + u8(g.session_dir) + " pid " + std::to_string(g.pid) + " : " + U(cmd));
    HANDLE proc = g.proc;
    HWND hwnd = g.hwnd;
    g.wait_thread = std::jthread([proc, hwnd] {
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        PostMessageW(hwnd, WM_APP_GAME_EXITED, code, 0);
    });
    refresh_all();
    show_page(PagePlay);
}

void request_stop() {
    if (!g.running) return;
    HWND gw = find_game_window(g.pid);
    if (gw) {
        g.stop_requested = true;
        PostMessageW(gw, WM_CLOSE, 0, 0);
        SetTimer(g.hwnd, TIMER_STOP, 10000, nullptr);
        log_line("stop: WM_CLOSE sent to the game window");
        return;
    }
    if (MessageBoxW(g.hwnd, L"The game has no window to close yet (it may still be starting or not responding).\n\nEnd the game now? Unsaved progress will be lost.",
                    L"End the game?", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
        g.forced = true;
        TerminateProcess(g.proc, 0xE0000001);
        log_line("stop: ended by the player (no game window)");
    }
}

void on_game_exited(DWORD code) {
    KillTimer(g.hwnd, TIMER_STOP);
    if (g.wait_thread.joinable()) g.wait_thread.join();
    if (g.proc) { CloseHandle(g.proc); g.proc = nullptr; }
    g.running = false;
    const ExitInfo info = describe_exit(code, g.session_dir / "game.log", g.forced);
    write_session_json(true, code, &info);
    g.last = {true, info, g.session_dir, "at " + utc_stamp(false).substr(11, 5) + " UTC (exit code " + std::to_string(code) + ")"};
    log_line("session end exit=" + std::to_string(code) + (g.forced ? " (ended by the player)" : "") + " : " + info.headline);
    refresh_all();
    show_page(PagePlay);
    if (g.quit_after_exit) { DestroyWindow(g.hwnd); return; }
    SetForegroundWindow(g.hwnd);
    if (!info.normal && !g.forced) {
        const std::wstring msg = W(info.headline + "\n\n" + info.detail + "\n\nOpen the session logs?");
        if (MessageBoxW(g.hwnd, msg.c_str(), L"JStarsReborn", MB_YESNO | MB_ICONWARNING) == IDYES)
            ShellExecuteW(g.hwnd, L"open", g.session_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- diagnostic report --------------------------------------------------------------------------------------------------------------
std::vector<std::string> privacy_secrets() {
    std::vector<std::string> s;
    const fs::path profile = known_folder(FOLDERID_Profile);
    if (!profile.empty()) { s.push_back(u8(profile)); s.push_back(U(profile.wstring())); }
    wchar_t name[256];
    DWORD n = 256;
    if (GetUserNameW(name, &n) && n > 1) s.push_back(U(name));
    n = 256;
    if (GetComputerNameW(name, &n) && n > 0) s.push_back(U(name));
    std::sort(s.begin(), s.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    return s;
}

std::string os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) fn(&v);
    return "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " + std::to_string(v.dwBuildNumber);
}

// Separate remembered folders for the two file dialogs (otherwise Windows offers the last folder of either dialog).
constexpr GUID kReportDialogGuid = {0x6a1f3c52, 0x9d4e, 0x4b7a, {0x8e, 0x21, 0x5c, 0x3b, 0x77, 0x0d, 0x91, 0xa4}};
constexpr GUID kFolderDialogGuid = {0x2c8e7b10, 0x41f5, 0x4e9d, {0xa3, 0x6c, 0x0f, 0x52, 0xd8, 0x19, 0x6e, 0x3b}};

// True when `p` is the game folder or inside it (case-insensitive, whole path components).
bool inside_game_folder(const fs::path& p) {
    if (g.settings.game_folder.empty()) return false;
    std::error_code ec;
    fs::path game = fs::weakly_canonical(from_u8(g.settings.game_folder), ec);
    if (ec) game = from_u8(g.settings.game_folder);
    fs::path target = fs::weakly_canonical(p, ec);
    if (ec) target = p;
    std::wstring a = game.wstring(), b = target.wstring();
    while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/')) a.pop_back();
    if (b.size() < a.size() || _wcsnicmp(a.c_str(), b.c_str(), a.size()) != 0) return false;
    return b.size() == a.size() || b[a.size()] == L'\\' || b[a.size()] == L'/';
}

void create_report() {
    const std::wstring name = L"JStarsReborn-report-" + W(utc_stamp(true)) + L".zip";
    fs::path target;
    for (;;) {
        IFileSaveDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        dlg->SetClientGuid(kReportDialogGuid);
        COMDLG_FILTERSPEC spec{L"ZIP archive", L"*.zip"};
        dlg->SetFileTypes(1, &spec);
        dlg->SetDefaultExtension(L"zip");
        dlg->SetFileName(target.empty() ? name.c_str() : target.filename().c_str());
        dlg->SetTitle(L"Save diagnostic report");
        IShellItem* docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&docs)))) {
            dlg->SetDefaultFolder(docs);  // used when this dialog has no remembered folder yet
            if (!target.empty()) dlg->SetFolder(docs);  // after a refused location
            docs->Release();
        }
        fs::path chosen;
        if (SUCCEEDED(dlg->Show(g.hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { chosen = p; CoTaskMemFree(p); }
                item->Release();
            }
        }
        dlg->Release();
        if (chosen.empty()) return;
        if (!inside_game_folder(chosen)) { target = chosen; break; }
        target = chosen;
        MessageBoxW(g.hwnd, L"The report can't be saved inside your game folder: that folder is only ever read.\n\nPlease choose another place, "
                            L"for example Documents or Desktop.", L"JStarsReborn", MB_ICONINFORMATION);
    }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    try {
        const auto secrets = privacy_secrets();
        auto clean = [&](const std::string& s) { return redact(s, secrets); };
        ZipWriter zip(target);
        std::ostringstream r;
        r << "JStarsReborn diagnostic report\n"
          << "created: " << utc_stamp(false) << "\n"
          << "launcher: " << launcher_version << "\nruntime: " << g.runtime_version << " sha256 " << g.runtime_sha256 << "\n"
          << "os: " << os_version() << "\nvulkan loader present: " << (g.vulkan_present ? "yes" : "no") << "\n\n"
          << "game folder: " << g.settings.game_folder << "\nstatus: " << g.game.summary << "\n";
        for (const auto& d : g.game.details) r << "  " << d << "\n";
        r << "prepared data: " << g.prep_state_text << "\n\nsettings: window " << g.settings.window_width << "x" << g.settings.window_height
          << ", sound " << g.settings.sound << ", volume " << g.settings.volume << ", keyboard " << g.settings.keyboard << ", confirm "
          << g.settings.confirm_button << "\n\n"
          << "Contents: launcher log, and for the newest three sessions: session.json, game.log, stdout.txt, stderr.txt.\n"
          << "Not included: game files, prepared data, saves, GPU command dumps, captures. Your user and computer name are replaced by <redacted>.\n"
          << "This report is not uploaded anywhere.\n";
        if (g.last.valid) r << "\nlast session: " << g.last.info.headline << " " << g.last.info.detail << "\n";
        zip.add("report.txt", clean(r.str()));
        zip.add("launcher.log", clean(read_text(g.logs_dir / "launcher.log", 1u << 20)));
        std::vector<fs::path> sessions;
        std::error_code ec;
        for (fs::directory_iterator it(g.sessions_root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec)) sessions.push_back(it->path());
        std::sort(sessions.begin(), sessions.end());
        if (sessions.size() > 3) sessions.erase(sessions.begin(), sessions.end() - 3);
        for (const auto& s : sessions) {
            const std::string base = "sessions/" + u8(s.filename()) + "/";
            const std::string game_log = read_text(s / "game.log", 8u << 20);
            for (const char* f : {"session.json", "game.log", "stdout.txt", "stderr.txt"}) {
                if (!fs::exists(s / f, ec)) continue;
                std::string text = std::string(f) == "game.log" ? game_log : read_text(s / f, 8u << 20);
                // The runtime writes its log to both game.log and stderr; keep one copy unless they differ.
                if (std::string(f) == "stderr.txt" && !game_log.empty() && text == game_log) text = "(identical to game.log)\n";
                zip.add(base + f, clean(text));
            }
        }
        zip.finish();
        SetCursor(old);
        log_line("report written: " + u8(target));
        if (MessageBoxW(g.hwnd, (L"The diagnostic report was saved:\n" + target.wstring() + L"\n\nIt is not uploaded anywhere. Show it in Explorer?").c_str(),
                        L"JStarsReborn", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            const std::wstring args = L"/select," + quote_arg(target.wstring());
            ShellExecuteW(g.hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    } catch (const std::exception& e) {
        SetCursor(old);
        MessageBoxW(g.hwnd, (L"The report could not be written:\n" + W(e.what())).c_str(), L"JStarsReborn", MB_ICONERROR);
    }
}

// ---- folder picker -------------------------------------------------------------------------------------------------------------------
void browse_folder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetClientGuid(kFolderDialogGuid);
    dlg->SetTitle(L"Choose the game folder (the one that contains eboot.bin and sce_sys)");
    if (!g.settings.game_folder.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(from_u8(g.settings.game_folder).wstring().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    fs::path chosen;
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { chosen = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (chosen.empty()) return;
    g.settings.game_folder = u8(chosen);
    save_settings_now();
    log_line("game folder chosen: " + g.settings.game_folder);
    start_check();
}

// ---- painting -------------------------------------------------------------------------------------------------------------------------
void draw_text(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT flags) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t.c_str(), -1, &r, flags);
    SelectObject(dc, old);
}

void paint(HDC dc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    RECT side{0, 0, S(kSidebarW), rc.bottom};
    FillRect(dc, &side, g.sidebar_brush);
    RECT content{S(kSidebarW), 0, rc.right, rc.bottom};
    FillRect(dc, &content, g.content_brush);
    draw_text(dc, L"JStarsReborn", RECT{S(22), S(24), S(kSidebarW), S(54)}, g.font_app, RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE);
    draw_text(dc, W(std::string("Launcher ") + launcher_version), RECT{S(22), S(54), S(kSidebarW), S(76)}, g.font_small, RGB(160, 165, 180), DT_LEFT | DT_SINGLELINE);
    draw_text(dc, L"Unofficial PC port. Not affiliated with the game's publisher.", RECT{S(16), rc.bottom - S(64), S(kSidebarW) - S(12), rc.bottom - S(12)},
              g.font_small, RGB(140, 145, 160), DT_LEFT | DT_WORDBREAK);
    const int x0 = S(kSidebarW) + S(kPad);
    if (g.page == PagePlay) {
        int tx = x0;
        if (g.icon_bmp) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, g.icon_bmp);
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            SetStretchBltMode(dc, HALFTONE);
            AlphaBlend(dc, x0, S(28), S(128), S(128) * g.icon_h / std::max(1, g.icon_w), mem, 0, 0, g.icon_w, g.icon_h, bf);
            SelectObject(mem, old);
            DeleteDC(mem);
            tx += S(128) + S(24);
        }
        std::wstring title = g.game.title.empty() ? L"J-Stars Victory VS+" : W(g.game.title);
        draw_text(dc, title, RECT{tx, S(26), rc.right - S(kPad), S(66)}, g.font_title, kText, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring sub = g.settings.game_folder.empty() ? L"No game folder selected"
                           : g.game.title_id.empty()    ? (g.checking ? L"Checking the game folder\u2026" : L"The chosen game folder needs attention (see Game folder)")
                                                        : W(g.game.title_id + "  \u00b7  Version " + g.game.app_ver);
        if (g.game.ok()) sub += L"  \u00b7  Supported build";
        draw_text(dc, sub, RECT{tx, S(64), rc.right - S(kPad), S(86)}, g.font, kMuted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        draw_text(dc, kPageNames[g.page], RECT{x0, S(30), rc.right - S(kPad), S(76)}, g.font_title, kText, DT_LEFT | DT_SINGLELINE);
    }
}

void draw_nav(const DRAWITEMSTRUCT* d) {
    const int p = static_cast<int>(d->CtlID) - IdNavFirst;
    const bool sel = p == g.page, hot = p == g.hover_nav;
    HBRUSH b = CreateSolidBrush(sel ? kSidebarSel : hot ? kSidebarHover : kSidebar);
    FillRect(d->hDC, &d->rcItem, b);
    DeleteObject(b);
    if (sel) {
        RECT bar{d->rcItem.left, d->rcItem.top + S(10), d->rcItem.left + S(4), d->rcItem.bottom - S(10)};
        HBRUSH a = CreateSolidBrush(kAccent);
        FillRect(d->hDC, &bar, a);
        DeleteObject(a);
    }
    RECT t = d->rcItem;
    t.left += S(22);
    draw_text(d->hDC, kPageNames[p], t, sel ? g.font_bold : g.font_nav, sel ? RGB(255, 255, 255) : RGB(205, 208, 218), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (d->itemState & ODS_FOCUS) {
        RECT f = d->rcItem;
        InflateRect(&f, -S(3), -S(3));
        HBRUSH fb = CreateSolidBrush(RGB(120, 160, 255));
        FrameRect(d->hDC, &f, fb);
        DeleteObject(fb);
    }
}

void draw_play(const DRAWITEMSTRUCT* d) {
    const bool disabled = d->itemState & ODS_DISABLED, pressed = d->itemState & ODS_SELECTED;
    FillRect(d->hDC, &d->rcItem, g.content_brush);
    HBRUSH b = CreateSolidBrush(disabled ? RGB(178, 182, 192) : pressed ? kAccentDark : kAccent);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, pen);
    RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, S(12), S(12));
    SelectObject(d->hDC, ob);
    SelectObject(d->hDC, op);
    DeleteObject(b);
    DeleteObject(pen);
    draw_text(d->hDC, get_text(IdPlay), d->rcItem, g.font_button, disabled ? RGB(60, 63, 72) : RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d->itemState & ODS_FOCUS) {
        RECT f = d->rcItem;
        InflateRect(&f, -S(4), -S(4));
        DrawFocusRect(d->hDC, &f);
    }
}

void draw_secondary(const DRAWITEMSTRUCT* d) {
    const bool pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
    FillRect(d->hDC, &d->rcItem, g.content_brush);
    HBRUSH b = CreateSolidBrush(disabled ? RGB(232, 234, 239) : pressed ? RGB(222, 226, 235) : RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), disabled ? RGB(190, 194, 204) : RGB(110, 115, 130));
    HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, pen);
    RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, S(8), S(8));
    SelectObject(d->hDC, ob);
    SelectObject(d->hDC, op);
    DeleteObject(b);
    DeleteObject(pen);
    wchar_t t[96];
    GetWindowTextW(d->hwndItem, t, 96);
    draw_text(d->hDC, t, d->rcItem, g.font_bold, disabled ? RGB(120, 124, 136) : kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d->itemState & ODS_FOCUS) {
        RECT f = d->rcItem;
        InflateRect(&f, -S(3), -S(3));
        DrawFocusRect(d->hDC, &f);
    }
}

// ---- controls ---------------------------------------------------------------------------------------------------------------------------
void build_controls() {
    for (int p = 0; p < PageCount; ++p) add(-1, L"BUTTON", kPageNames[p], BS_OWNERDRAW | WS_TABSTOP | WS_VISIBLE, IdNavFirst + p);
    // Play
    add(PagePlay, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdStatus);
    add(PagePlay, L"BUTTON", L"Play", BS_OWNERDRAW | WS_TABSTOP, IdPlay);
    add(PagePlay, L"BUTTON", L"Stop game", BS_OWNERDRAW | WS_TABSTOP, IdStop);
    add(PagePlay, PROGRESS_CLASSW, L"", PBS_SMOOTH, IdPrepProgress);
    SendMessageW(ctl(IdPrepProgress), PBM_SETRANGE32, 0, 1000);
    add(PagePlay, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdPrepStep);
    add(PagePlay, L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP, IdCancelPrep);
    add(PagePlay, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdLastHead);
    add(PagePlay, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdLastDetail);
    add(PagePlay, L"BUTTON", L"Open session logs", BS_OWNERDRAW | WS_TABSTOP, IdOpenSessionLogs);
    add(PagePlay, L"STATIC",
        L"Controls: an Xbox-compatible controller, or the keyboard (see Controls). Input reaches the game only while its window is active. "
        L"Close the game window to end a session.", SS_LEFT | SS_NOPREFIX, IdPlayHint);
    // Game folder
    add(PageFolder, L"STATIC", L"Folder of your own game dump (contains eboot.bin, sce_sys and sce_module):", SS_LEFT | SS_NOPREFIX, IdFolderLabel);
    add(PageFolder, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, IdFolderEdit, WS_EX_CLIENTEDGE);
    add(PageFolder, L"BUTTON", L"Browse\u2026", BS_OWNERDRAW | WS_TABSTOP, IdBrowse);
    add(PageFolder, L"BUTTON", L"Check again", BS_OWNERDRAW | WS_TABSTOP, IdRecheck);
    add(PageFolder, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, IdFolderDetails, WS_EX_CLIENTEDGE);
    add(PageFolder, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdPrepState);
    add(PageFolder, L"BUTTON", L"Prepare now", BS_OWNERDRAW | WS_TABSTOP, IdPrepareNow);
    add(PageFolder, L"STATIC",
        L"Your game folder is only read, never changed. Prepared data, saves, settings and logs are stored in your user profile "
        L"(%LOCALAPPDATA%\\JStarsReborn).", SS_LEFT | SS_NOPREFIX, IdFolderNote);
    // Settings
    add(PageSettings, L"STATIC", L"Window size", SS_LEFT, IdLblWindow);
    add(PageSettings, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdWindowSize);
    for (auto [w, h] : window_sizes()) {
        const std::wstring t = std::to_wstring(w) + L" \u00d7 " + std::to_wstring(h);
        SendMessageW(ctl(IdWindowSize), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.c_str()));
    }
    add(PageSettings, L"BUTTON", L"Play game sound", BS_AUTOCHECKBOX | WS_TABSTOP, IdSound);
    add(PageSettings, L"STATIC", L"Volume", SS_LEFT, IdLblVolume);
    add(PageSettings, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IdVolume);
    SendMessageW(ctl(IdVolume), TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(ctl(IdVolume), TBM_SETPAGESIZE, 0, 10);
    add(PageSettings, L"STATIC", L"", SS_LEFT, IdVolumeLabel);
    add(PageSettings, L"BUTTON", L"Use the keyboard as a controller when no controller is connected", BS_AUTOCHECKBOX | WS_TABSTOP, IdKeyboard);
    add(PageSettings, L"BUTTON", L"60 fps (experimental: battle speed is correct; some animations may still be off)", BS_AUTOCHECKBOX | WS_TABSTOP, IdFrameRate60);
    add(PageSettings, L"STATIC", L"Confirm button", SS_LEFT, IdLblConfirm);
    add(PageSettings, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdConfirm);
    SendMessageW(ctl(IdConfirm), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cross confirms (Western layout)"));
    SendMessageW(ctl(IdConfirm), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Circle confirms (Japanese layout)"));
    add(PageSettings, L"STATIC", L"Player name", SS_LEFT, IdLblName);
    add(PageSettings, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdPlayerName, WS_EX_CLIENTEDGE);
    SendMessageW(ctl(IdPlayerName), EM_SETLIMITTEXT, 16, 0);
    add(PageSettings, L"STATIC",
        L"Settings are saved at once and apply the next time you press Play. The player name is what the game sees as your console user name. "
        L"Graphics options beyond the window size are not offered because the runtime has none yet. Online play is on the Online page.", SS_LEFT | SS_NOPREFIX, IdSettingsNote);
    // Online
    add(PageOnline, L"BUTTON", L"Enable online play (experimental, still in testing)", BS_AUTOCHECKBOX | WS_TABSTOP, IdOnline);
    add(PageOnline, L"STATIC",
        L"Online play is peer-to-peer: your game and your friend's game talk to each other directly. There is no JStarsReborn server and "
        L"nothing contacts PlayStation Network. Players on the same network find each other's rooms automatically.", SS_LEFT | SS_NOPREFIX, IdOnlineIntro);
    add(PageOnline, L"BUTTON", L"Open my ports on the router automatically (UPnP) so players on the internet can join me", BS_AUTOCHECKBOX | WS_TABSTOP, IdUpnp);
    add(PageOnline, L"STATIC", L"Your room code", SS_LEFT, IdLblMyCode);
    add(PageOnline, L"EDIT", L"", ES_READONLY | ES_CENTER | WS_TABSTOP, IdMyCode, WS_EX_CLIENTEDGE);
    add(PageOnline, L"BUTTON", L"Get my room code", BS_OWNERDRAW | WS_TABSTOP, IdFindCode);
    add(PageOnline, L"BUTTON", L"Copy", BS_OWNERDRAW | WS_TABSTOP, IdCopyCode);
    add(PageOnline, L"STATIC", L"Friend's room code", SS_LEFT, IdLblPeers);
    add(PageOnline, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IdLobbyPeers, WS_EX_CLIENTEDGE);
    SendMessageW(ctl(IdLobbyPeers), EM_SETLIMITTEXT, 200, 0);
    add(PageOnline, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdOnlineStatus);
    add(PageOnline, L"STATIC",
        L"How to play over the internet: the host gets a room code here and sends it to the other player, who types it into \"Friend's room "
        L"code\" (several codes can be separated by commas). Both press Play and go to Online Battle > Friendly Match: the host creates a room, "
        L"the other player uses Search for a Room. If the router has no UPnP, forward UDP ports 47391 and 3658 to this PC by hand and give your "
        L"public address instead of a code. Still in testing: joining a room is not finished yet, so a match cannot start yet.",
        SS_LEFT | SS_NOPREFIX, IdOnlineNote);
    // Controls
    HWND lv = add(PageControls, WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOSORTHEADER | LVS_SINGLESEL | WS_TABSTOP, IdControlsList, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[3] = {L"Game (PS4) button", L"Controller (Xbox layout)", L"Keyboard"};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = 150;
        ListView_InsertColumn(lv, i, &c);
    }
    // The real mapping of runtime/jstars_os.c (os_pad_poll).
    const wchar_t* rows[][3] = {
        {L"Cross (\u00d7)", L"A", L"Enter or Space"},
        {L"Circle (\u25cb)", L"B", L"Backspace"},
        {L"Square (\u25a1)", L"X", L"F"},
        {L"Triangle (\u25b3)", L"Y", L"R"},
        {L"L1", L"Left bumper (LB)", L"Q"},
        {L"R1", L"Right bumper (RB)", L"E"},
        {L"L2", L"Left trigger (LT)", L"Left Shift"},
        {L"R2", L"Right trigger (RT)", L"Left Ctrl"},
        {L"L3 (left stick press)", L"Left stick press", L"V"},
        {L"R3 (right stick press)", L"Right stick press", L"B"},
        {L"OPTIONS", L"Menu (Start)", L"Esc or P"},
        {L"Touch pad button", L"View (Back)", L"T"},
        {L"Directional buttons", L"D-pad", L"Arrow keys"},
        {L"Left stick", L"Left stick", L"W A S D"},
        {L"Right stick", L"Right stick", L"I J K L, or numpad 8 4 5 6"},
    };
    for (int i = 0; i < static_cast<int>(std::size(rows)); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = const_cast<wchar_t*>(rows[i][0]);
        ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, i, 1, const_cast<wchar_t*>(rows[i][1]));
        ListView_SetItemText(lv, i, 2, const_cast<wchar_t*>(rows[i][2]));
    }
    add(PageControls, L"STATIC",
        L"Controllers: Xbox-compatible (XInput) controllers work directly; PlayStation controllers work through Steam Input or DS4Windows "
        L"(XInput mode). The first connected controller is used; unplugging and reconnecting it is picked up within half a second. When no "
        L"controller is connected, the keyboard is used (if enabled in Settings). Input reaches the game only while the game window is "
        L"active. Which button confirms in menus is set on the Settings page. These mappings are fixed in this version.",
        SS_LEFT | SS_NOPREFIX, IdControlsNote);
    // Saves
    add(PageSaves, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdSavesInfo);
    add(PageSaves, L"BUTTON", L"Import a save file\u2026", BS_OWNERDRAW | WS_TABSTOP, IdSaveImportFile);
    add(PageSaves, L"BUTTON", L"Import from a folder\u2026", BS_OWNERDRAW | WS_TABSTOP, IdSaveImportFolder);
    add(PageSaves, L"BUTTON", L"Unlock every character", BS_OWNERDRAW | WS_TABSTOP, IdSaveUnlock);
    add(PageSaves, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdSaveBackupList);
    add(PageSaves, L"BUTTON", L"Restore this backup", BS_OWNERDRAW | WS_TABSTOP, IdSaveRestore);
    add(PageSaves, L"BUTTON", L"Open backups folder", BS_OWNERDRAW | WS_TABSTOP, IdSaveOpenBackups);
    add(PageSaves, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IdSavesStatus);
    add(PageSaves, L"STATIC",
        L"Import: choose a SAVE-DATA.DAT, or a folder that contains one (another JStarsReborn install, a shadPS4 user folder, or a "
        L"decrypted PS4 save). Encrypted PS4 saves must be decrypted first (for example with Apollo Save Tool). Every change makes a backup "
        L"of your current save first. Unlock every character uses the save format documented by CLAIREANDLEON (tools/save_editor): all "
        L"characters unlocked and owned, J-Points 999,999. The game must be closed for any of this.",
        SS_LEFT | SS_NOPREFIX, IdSavesNote);
    // Troubleshooting
    add(PageHelp, L"BUTTON", L"Open latest session logs", BS_OWNERDRAW | WS_TABSTOP, IdOpenLatest);
    add(PageHelp, L"BUTTON", L"Open log folder", BS_OWNERDRAW | WS_TABSTOP, IdOpenLogs);
    add(PageHelp, L"BUTTON", L"Create diagnostic report\u2026", BS_OWNERDRAW | WS_TABSTOP, IdReport);
    add(PageHelp, L"BUTTON", L"Open data folder", BS_OWNERDRAW | WS_TABSTOP, IdOpenData);
    add(PageHelp, L"BUTTON", L"Delete prepared data", BS_OWNERDRAW | WS_TABSTOP, IdDeletePrepared);
    add(PageHelp, L"BUTTON", L"Licenses and notices", BS_OWNERDRAW | WS_TABSTOP, IdLicenses);
    add(PageHelp, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, IdHelpText, WS_EX_CLIENTEDGE);
}

void refresh_help_text() {
    std::wostringstream t;
    t << L"Every play session gets its own log folder (game.log, session.json with the versions, settings, start/end time and how it "
         L"ended). The newest 30 sessions are kept.\r\n\r\n"
      << L"The diagnostic report is a ZIP file you save yourself; it is never uploaded. It contains the launcher log and the logs of the newest "
         L"three sessions, with your user and computer names replaced. It never contains game files, prepared data, saves or captures.\r\n\r\n"
      << L"Delete prepared data if the game fails to start after you changed the game folder; it is rebuilt on the next Play.\r\n\r\n"
      << L"Known limits of this version: movies are skipped; trophies are not available; online is an experimental local-network test (Settings); only CUSA01593 version 01.02 "
         L"is supported; only the title screen, menus, Free Battle and the J-Adventure map have been exercised.\r\n\r\n"
      << L"Launcher " << W(launcher_version) << L"\r\nRuntime: " << W(g.runtime_version) << L"\r\nRuntime SHA-256: " << W(g.runtime_sha256)
      << L"\r\nData folder: " << g.data_root.wstring() << L"\r\nSaves: " << g.user_dir.wstring();
    set_text(IdHelpText, t.str());
}

void load_settings_into_ui() {
    int sel = 0;
    const auto& sizes = window_sizes();
    for (std::size_t i = 0; i < sizes.size(); ++i)
        if (sizes[i].first == g.settings.window_width && sizes[i].second == g.settings.window_height) sel = static_cast<int>(i);
    SendMessageW(ctl(IdWindowSize), CB_SETCURSEL, sel, 0);
    SendMessageW(ctl(IdSound), BM_SETCHECK, g.settings.sound ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ctl(IdVolume), TBM_SETPOS, TRUE, g.settings.volume);
    set_text(IdVolumeLabel, std::to_wstring(g.settings.volume) + L" %");
    enable(IdVolume, g.settings.sound);
    SendMessageW(ctl(IdKeyboard), BM_SETCHECK, g.settings.keyboard ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ctl(IdFrameRate60), BM_SETCHECK, g.settings.frame_rate == 60 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ctl(IdConfirm), CB_SETCURSEL, g.settings.confirm_button == "circle" ? 1 : 0, 0);
    set_text(IdPlayerName, W(g.settings.player_name));
    refresh_online_page();
}

// ---- first run: data folder, backups of earlier data -----------------------------------------------------------------------------------
void init_data() {
    g.data_root = reborn::default_data_dir();
    g.settings_file = g.data_root / "launcher.ini";
    g.sessions_root = g.data_root / "sessions";
    g.user_dir = g.data_root / "user";
    g.logs_dir = g.data_root / "logs";
    std::error_code ec;
    const bool first_run = !fs::exists(g.settings_file, ec);
    if (first_run && fs::exists(g.data_root, ec)) {
        // Data from an earlier version (0.2.2 foundation: config\settings.ini, logs, crashes). Back it up before writing anything.
        const fs::path backup = g.data_root / ("backup-before-launcher-" + std::string(launcher_version) + "-" + utc_stamp(true));
        bool any = false;
        for (fs::directory_iterator it(g.data_root, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().wstring();
            if (name.rfind(L"backup-", 0) == 0) continue;
            fs::create_directories(backup, ec);
            fs::copy(it->path(), backup / it->path().filename(), fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            any = true;
        }
        if (any) log_line("first run: earlier data backed up to " + u8(backup) + " (originals left in place)");
    }
    std::vector<std::string> notes;
    g.settings = load_settings(g.settings_file, &notes);
    for (const auto& n : notes) log_line(n);
    if (first_run) {
        // window size from the 0.2.2 settings, when it is one of the offered sizes (the old volume controlled a test tone, not the game)
        const std::string legacy = read_text(g.data_root / "config" / "settings.ini", 1 << 16);
        int w = 0, h = 0;
        std::istringstream in(legacy);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("width=", 0) == 0) w = std::atoi(line.c_str() + 6);
            if (line.rfind("height=", 0) == 0) h = std::atoi(line.c_str() + 7);
        }
        for (auto [sw, sh] : window_sizes())
            if (sw == w && sh == h) { g.settings.window_width = w; g.settings.window_height = h; log_line("first run: window size imported from 0.2.2 settings"); }
        // default game folder: E:\CUSA01593 only if it exists and validates
        if (g.settings.game_folder.empty()) {
            const fs::path def = L"E:\\CUSA01593";
            if (fs::is_directory(def, ec) && check_game_folder(def).ok()) {
                g.settings.game_folder = u8(def);
                log_line("first run: default game folder E:\\CUSA01593 validated and selected");
            }
        }
        save_settings_now();
    }
}

// ---- window procedure -----------------------------------------------------------------------------------------------------------------
// ---- online page ----------------------------------------------------------------------------------------------------------
struct OnlineResult { std::string status, code; bool mapped = false; online::Igd igd; };

// ---- Saves page (saves.hpp) --------------------------------------------------------------------------------------------------------
fs::path backups_root() { return g.data_root / "save-backups"; }
static std::vector<fs::path> g_save_backups;
void refresh_saves_page() {
    std::wstring info;
    std::vector<std::uint8_t> b;
    {
        std::ifstream f(saves::save_file(g.user_dir), std::ios::binary);
        if (f) b.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (b.empty()) info = L"No save yet. Start the game once and let it create its save (choose Yes when it asks), then close it.";
    else {
        const saves::Info in = saves::inspect(b);
        if (!in.valid) info = L"Your current save has a problem: " + W(in.problem) + L". You can restore a backup below.";
        else info = L"Current save: " + std::to_wstring(in.jpoints) + L" J-Points, " + std::to_wstring(in.owned) + L" of " +
                    std::to_wstring(saves::kOwned) + L" battle characters owned" + (in.all_unlocked ? L" (every character unlocked)." : L".");
    }
    if (g.running) info += L"\r\nClose the game to import, unlock or restore.";
    set_text(IdSavesInfo, info);
    for (int id : {IdSaveImportFile, IdSaveImportFolder, IdSaveUnlock, IdSaveRestore}) enable(id, !g.running);
    g_save_backups = saves::list_backups(backups_root());
    HWND cb = ctl(IdSaveBackupList);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (const auto& p : g_save_backups) SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.filename().wstring().c_str()));
    if (g_save_backups.empty()) SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(no backups yet)"));
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
    enable(IdSaveRestore, !g.running && !g_save_backups.empty());
}
// File (folder=false) or folder picker; empty when cancelled.
fs::path pick_path(bool folder, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dlg->SetTitle(title);
    if (!folder) {
        const COMDLG_FILTERSPEC filters[] = {{L"J-Stars save (SAVE-DATA.DAT)", L"SAVE-DATA.DAT;*.DAT"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filters);
    }
    fs::path chosen;
    if (SUCCEEDED(dlg->Show(g.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { chosen = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    return chosen;
}
void saves_report(const saves::Result& r) {
    set_text(IdSavesStatus, W(r.message));
    log_line(std::string("saves: ") + (r.ok ? "ok: " : "not done: ") + r.message);
    refresh_saves_page();
}
bool saves_game_closed() {
    if (!g.running) return true;
    set_text(IdSavesStatus, L"Close the game first: it would overwrite the save.");
    return false;
}
void saves_import(bool folder) {
    if (!saves_game_closed()) return;
    const fs::path chosen = pick_path(folder, folder ? L"Choose a folder that contains a J-Stars save (SAVE-DATA.DAT)" : L"Choose a J-Stars save (SAVE-DATA.DAT)");
    if (chosen.empty()) return;
    std::string why;
    const fs::path src = saves::find_importable(chosen, &why);
    if (src.empty()) { saves_report({false, "Not imported: " + why + ".", {}}); return; }
    const std::wstring q = L"Replace your current save with\r\n" + src.wstring() + L" ?\r\n\r\nYour current save is backed up first and can be restored on this page.";
    if (MessageBoxW(g.hwnd, q.c_str(), L"Import save", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    saves_report(saves::import_save(src, g.user_dir, backups_root()));
}

void refresh_online_page() {
    if (g.settings.online && g.online_status == "Online play is off.") g.online_status = "Online play is on. Press Play, then Online Battle > Friendly Match.";
    SendMessageW(ctl(IdOnline), BM_SETCHECK, g.settings.online ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ctl(IdUpnp), BM_SETCHECK, g.settings.upnp ? BST_CHECKED : BST_UNCHECKED, 0);
    if (GetFocus() != ctl(IdLobbyPeers)) set_text(IdLobbyPeers, W(g.settings.lobby_peers));
    set_text(IdMyCode, W(g.my_code));
    set_text(IdOnlineStatus, W(g.online_busy ? std::string("Asking the router to open UDP ports 47391 and 3658 for this PC\u2026") : g.online_status));
    for (int id : {IdUpnp, IdFindCode, IdLobbyPeers, IdLblPeers, IdLblMyCode, IdMyCode}) enable(id, g.settings.online);
    enable(IdFindCode, g.settings.online && !g.online_busy);
    enable(IdCopyCode, g.settings.online && !g.my_code.empty());
}

void copy_to_clipboard(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

// Finds the router, opens the two UDP ports (if UPnP is on) and works out the room code, on a worker thread.
void start_online_setup() {
    if (g.online_busy) return;
    g.online_busy = true;
    refresh_online_page();
    const bool upnp = g.settings.upnp;
    HWND hwnd = g.hwnd;
    if (g.online_thread.joinable()) g.online_thread.join();
    g.online_thread = std::jthread([upnp, hwnd] {
        auto* r = new OnlineResult;
        std::string log;
        const std::string lan = online::local_ipv4();
        if (lan.empty()) r->status = "This PC has no network connection.";
        else if (!upnp) r->status = "UPnP is off. Forward UDP ports 47391 and 3658 to " + lan + " on your router, and give the other player your public address.";
        else if (!online::igd_find(&r->igd, &log)) r->status = "Could not open the ports automatically: " + log + "Forward UDP ports 47391 and 3658 to " + lan + " by hand, or turn on UPnP in the router.";
        else {
            const bool a = online::igd_add_udp_mapping(r->igd, online::kDirectoryPort, lan, "JStarsReborn rooms", &log);
            const bool b = online::igd_add_udp_mapping(r->igd, online::kGamePort, lan, "JStarsReborn game", &log);
            r->mapped = a || b;
            std::string pub;
            if (!online::igd_external_ip(r->igd, &pub, &log)) r->status = "Ports opened, but the router did not tell its public address. " + log;
            else {
                uint32_t ip = 0; uint16_t port = 0;
                online::decode_peer(pub, &ip, &port);
                const bool priv = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == 0x191;   // 10/8, 172.16/12, 192.168/16, 100.64/10
                r->code = online::encode_room_code(ip, online::kDirectoryPort);
                if (!(a && b)) r->status = "Only part of the ports could be opened: " + log;
                else if (priv) r->status = "Ports opened, but your router is itself behind another network (" + pub + "): players on the internet may not reach you. Players on your network can.";
                else r->status = "Ready: UDP ports 47391 and 3658 are open for this PC (" + lan + "). Send your room code to the other player.";
            }
        }
        if (!PostMessageW(hwnd, WM_APP_ONLINE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
}

void on_command(int id, int code) {
    if (id == IDOK) { // Enter: IsDialogMessage sends IDOK; press the focused button if there is one
        HWND f = GetFocus();
        wchar_t cls[16] = {};
        if (f && IsChild(g.hwnd, f) && GetClassNameW(f, cls, 16) && !_wcsicmp(cls, L"Button") && IsWindowEnabled(f) &&
            (GetWindowLongW(f, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
            SendMessageW(f, BM_CLICK, 0, 0);
        return;
    }
    if (id == IDCANCEL) return;
    if (id >= IdNavFirst && id < IdNavFirst + kPages) { show_page(id - IdNavFirst); return; }
    switch (id) {
    case IdPlay: launch_game(); break;
    case IdStop: request_stop(); break;
    case IdCancelPrep: g.prep_thread.request_stop(); break;
    case IdBrowse: browse_folder(); break;
    case IdRecheck: start_check(); break;
    case IdPrepareNow: start_prepare(false); break;
    case IdOpenSessionLogs:
        if (g.last.valid) ShellExecuteW(g.hwnd, L"open", g.last.session.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IdOpenLatest: {
        auto s = latest_session(g.sessions_root);
        if (s) ShellExecuteW(g.hwnd, L"open", s->c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else MessageBoxW(g.hwnd, L"There are no session logs yet. They are created when you play.", L"JStarsReborn", MB_ICONINFORMATION);
        break;
    }
    case IdOpenLogs: {
        std::error_code ec;
        fs::create_directories(g.sessions_root, ec);
        ShellExecuteW(g.hwnd, L"open", g.sessions_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case IdOpenData: ShellExecuteW(g.hwnd, L"open", g.data_root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IdSaveImportFile: saves_import(false); break;
    case IdSaveImportFolder: saves_import(true); break;
    case IdSaveUnlock:
        if (saves_game_closed() &&
            MessageBoxW(g.hwnd, L"Unlock every character and set J-Points to 999,999 in your save?\r\n\r\nYour current save is backed up first and can be restored on this page.",
                        L"Unlock every character", MB_YESNO | MB_ICONQUESTION) == IDYES)
            saves_report(saves::unlock_all_characters(g.user_dir, backups_root()));
        break;
    case IdSaveRestore: {
        if (!saves_game_closed()) break;
        const int sel = (int)SendMessageW(ctl(IdSaveBackupList), CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)g_save_backups.size()) break;
        const std::wstring q = L"Restore the backup " + g_save_backups[sel].filename().wstring() + L" ?\r\n\r\nYour current save is backed up first.";
        if (MessageBoxW(g.hwnd, q.c_str(), L"Restore save", MB_YESNO | MB_ICONQUESTION) == IDYES)
            saves_report(saves::restore(g_save_backups[sel], g.user_dir, backups_root()));
        break;
    }
    case IdSaveOpenBackups: {
        std::error_code ec;
        fs::create_directories(backups_root(), ec);
        ShellExecuteW(g.hwnd, L"open", backups_root().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case IdLicenses: ShellExecuteW(g.hwnd, L"open", (g.exe.parent_path() / "LICENSES").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IdReport: create_report(); break;
    case IdDeletePrepared: {
        if (!g.game.ok()) break;
        if (MessageBoxW(g.hwnd, L"Delete the prepared data? It is created again (a few seconds) the next time you press Play. Saves and settings are not affected.",
                        L"JStarsReborn", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
        std::error_code ec;
        fs::remove_all(prepared_dir(g.data_root, g.game), ec);
        log_line("prepared data deleted by the player");
        start_check();
        break;
    }
    case IdWindowSize:
        if (code == CBN_SELCHANGE) {
            const int i = static_cast<int>(SendMessageW(ctl(IdWindowSize), CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < static_cast<int>(window_sizes().size())) {
                g.settings.window_width = window_sizes()[static_cast<std::size_t>(i)].first;
                g.settings.window_height = window_sizes()[static_cast<std::size_t>(i)].second;
                save_settings_now();
            }
        }
        break;
    case IdSound:
        g.settings.sound = SendMessageW(ctl(IdSound), BM_GETCHECK, 0, 0) == BST_CHECKED;
        enable(IdVolume, g.settings.sound);
        save_settings_now();
        break;
    case IdKeyboard:
        g.settings.keyboard = SendMessageW(ctl(IdKeyboard), BM_GETCHECK, 0, 0) == BST_CHECKED;
        save_settings_now();
        break;
    case IdFrameRate60:
        g.settings.frame_rate = SendMessageW(ctl(IdFrameRate60), BM_GETCHECK, 0, 0) == BST_CHECKED ? 60 : 30;
        save_settings_now();
        break;
    case IdOnline:
        g.settings.online = SendMessageW(ctl(IdOnline), BM_GETCHECK, 0, 0) == BST_CHECKED;
        g.online_status = g.settings.online ? "Online play is on. Press Play, then Online Battle > Friendly Match." : "Online play is off.";
        save_settings_now();
        refresh_online_page();
        break;
    case IdUpnp:
        g.settings.upnp = SendMessageW(ctl(IdUpnp), BM_GETCHECK, 0, 0) == BST_CHECKED;
        save_settings_now();
        refresh_online_page();
        break;
    case IdFindCode: start_online_setup(); break;
    case IdCopyCode: copy_to_clipboard(W(g.my_code)); break;
    case IdLobbyPeers:
        if (code == EN_KILLFOCUS) {
            g.settings.lobby_peers = sanitize_lobby_peers(U(get_text(IdLobbyPeers)));
            set_text(IdLobbyPeers, W(g.settings.lobby_peers));
            std::string bad;
            online::peers_to_lobby_arg(g.settings.lobby_peers, &bad);
            if (!bad.empty()) g.online_status = "Not a room code or address (ignored): " + bad;
            save_settings_now();
            refresh_online_page();
        }
        break;
    case IdConfirm:
        if (code == CBN_SELCHANGE) {
            g.settings.confirm_button = SendMessageW(ctl(IdConfirm), CB_GETCURSEL, 0, 0) == 1 ? "circle" : "cross";
            save_settings_now();
        }
        break;
    case IdPlayerName:
        if (code == EN_KILLFOCUS) {
            g.settings.player_name = sanitize_player_name(U(get_text(IdPlayerName)));
            set_text(IdPlayerName, W(g.settings.player_name));
            save_settings_now();
        }
        break;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        g.content_brush = CreateSolidBrush(kContentBg);
        g.sidebar_brush = CreateSolidBrush(kSidebar);
        build_controls();
        make_fonts();
        load_settings_into_ui();
        refresh_help_text();
        layout();
        show_page(PagePlay);
        refresh_all();
        start_runtime_info();
        start_check();
        SetTimer(hwnd, TIMER_POLL, 2000, nullptr);
        // the most recent session's result, if any (e.g. after reopening the launcher)
        if (auto s = latest_session(g.sessions_root)) {
            const std::string j = read_text(*s / "session.json", 1 << 16);
            const auto p = j.find("\"exit_code\": ");
            if (p != std::string::npos) {
                const DWORD code = static_cast<DWORD>(std::strtoul(j.c_str() + p + 13, nullptr, 10));
                g.last = {true, describe_exit(code, *s / "game.log", j.find("\"ended_by_launcher\": true") != std::string::npos), *s,
                          "in an earlier launcher session (exit code " + std::to_string(code) + ")"};
            }
        }
        refresh_all();
        return 0;
    }
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        make_fonts();
        layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* m = reinterpret_cast<MINMAXINFO*>(lp);
        m->ptMinTrackSize = {S(900), S(620)};
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == IdFolderEdit || id == IdFolderDetails || id == IdHelpText) {
            SetBkColor(dc, RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        SetBkColor(dc, kContentBg);
        COLORREF c = kText;
        if (id == IdFolderNote || id == IdSettingsNote || id == IdControlsNote || id == IdPlayHint || id == IdPrepStep) c = kMuted;
        if (id == IdLastHead) c = g.last.info.normal ? kGood : kBad;
        if (id == IdStatus) c = g.game.ok() || g.running || g.preparing || g.checking || g.game.status == GameStatus::NotChosen ? kText : kBad;
        SetTextColor(dc, c);
        return reinterpret_cast<LRESULT>(g.content_brush);
    }
    case WM_DRAWITEM: {
        const auto* d = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (d->CtlID >= IdNavFirst && d->CtlID < static_cast<UINT>(IdNavFirst + kPages)) draw_nav(d);
        else if (d->CtlID == IdPlay) draw_play(d);
        else if (d->CtlType == ODT_BUTTON) draw_secondary(d);
        return TRUE;
    }
    case WM_SETCURSOR:
        if (reinterpret_cast<HWND>(wp) != hwnd) {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(wp));
            int hover = id >= IdNavFirst && id < IdNavFirst + kPages ? id - IdNavFirst : -1;
            if (hover != g.hover_nav) {
                const int old = g.hover_nav;
                g.hover_nav = hover;
                if (old >= 0) InvalidateRect(ctl(IdNavFirst + old), nullptr, FALSE);
                if (hover >= 0) InvalidateRect(ctl(IdNavFirst + hover), nullptr, FALSE);
            }
            if (hover >= 0 || id == IdPlay) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        }
        break;
    case WM_COMMAND: on_command(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp) == ctl(IdVolume)) {
            g.settings.volume = static_cast<int>(SendMessageW(ctl(IdVolume), TBM_GETPOS, 0, 0));
            set_text(IdVolumeLabel, std::to_wstring(g.settings.volume) + L" %");
            if (LOWORD(wp) == TB_ENDTRACK || LOWORD(wp) == TB_THUMBPOSITION) save_settings_now();
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_STOP) {
            KillTimer(hwnd, TIMER_STOP);
            if (g.running && MessageBoxW(hwnd, L"The game has not closed after 10 seconds.\n\nEnd it now? Unsaved progress will be lost.", L"Game not responding",
                                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES && g.running) {
                g.forced = true;
                TerminateProcess(g.proc, 0xE0000001);
                log_line("stop: ended by the player after the close request timed out");
            }
        } else if (wp == TIMER_POLL) {
            // a game window this launcher did not start (e.g. from before the launcher was reopened) blocks a second session
            const bool foreign = !g.running && find_game_window(0) != nullptr;
            if (foreign != g.foreign_game) { g.foreign_game = foreign; refresh_all(); }
        }
        return 0;
    case WM_APP_CHECKED: {
        std::unique_ptr<CheckResult> r(reinterpret_cast<CheckResult*>(lp));
        if (g.check_thread.joinable()) g.check_thread.join();
        g.checking = false;
        g.game = std::move(r->game);
        g.prepared = r->prepared;
        g.prep_state_text = r->prep_text;
        load_icon(g.game.ok() ? g.game.icon : fs::path());
        log_line("game check: " + g.game.summary + " [" + g.settings.game_folder + "]");
        layout();
        refresh_all();
        return 0;
    }
    case WM_APP_PREP_PROGRESS: {
        double f;
        std::string step;
        {
            std::scoped_lock lk{g.prep_mutex};
            f = g.prep_fraction;
            step = g.prep_step;
        }
        SendMessageW(ctl(IdPrepProgress), PBM_SETPOS, static_cast<WPARAM>(f * 1000), 0);
        set_text(IdPrepStep, W(step + "\u2026"));
        return 0;
    }
    case WM_APP_PREP_DONE: {
        std::unique_ptr<std::string> err(reinterpret_cast<std::string*>(lp));
        if (g.prep_thread.joinable()) g.prep_thread.join();
        g.preparing = false;
        if (err) {
            log_line("prepare: failed: " + *err);
            g.prepared = false;
            if (*err != "cancelled")
                MessageBoxW(hwnd, (L"The game could not be prepared:\n\n" + W(*err) + L"\n\nCheck the game folder page; your game folder was not changed.").c_str(),
                            L"JStarsReborn", MB_ICONERROR);
            g.prep_state_text = *err == "cancelled" ? "preparation was cancelled; it starts again when you press Play." : "failed: " + *err;
            refresh_all();
            show_page(g.page);
            return 0;
        }
        log_line("prepare: done");
        g.prepared = true;
        g.prep_state_text = "ready (made from exactly these game files).";
        refresh_all();
        show_page(g.page);
        if (g.launch_after_prep) launch_game();
        return 0;
    }
    case WM_APP_RUNTIME_INFO: {
        std::unique_ptr<RuntimeInfo> r(reinterpret_cast<RuntimeInfo*>(lp));
        g.runtime_version = r->version;
        g.runtime_sha256 = r->sha256;
        g.vulkan_present = r->vulkan;
        log_line("runtime: " + r->version + " sha256 " + r->sha256 + (r->vulkan ? "" : " (Vulkan loader not found)"));
        refresh_help_text();
        refresh_all();
        return 0;
    }
    case WM_APP_GAME_EXITED: on_game_exited(static_cast<DWORD>(wp)); return 0;
    case WM_APP_ONLINE: {
        std::unique_ptr<OnlineResult> r(reinterpret_cast<OnlineResult*>(lp));
        g.online_busy = false;
        g.online_status = r->status;
        if (r->mapped) { g.ports_mapped = true; g.igd = r->igd; }
        if (!r->code.empty()) g.my_code = r->code;
        log_line("online: " + r->status);
        refresh_online_page();
        return 0;
    }
    case WM_CLOSE:
        if (g.running) {
            if (MessageBoxW(hwnd, L"The game is still running.\n\nClose the game and the launcher? (The game is asked to close normally.)", L"JStarsReborn",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                g.quit_after_exit = true;
                request_stop();
            }
            return 0;
        }
        if (g.preparing) g.prep_thread.request_stop();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        if (g.prep_thread.joinable()) { g.prep_thread.request_stop(); g.prep_thread.join(); }
        if (g.check_thread.joinable()) { g.check_thread.request_stop(); g.check_thread.join(); }
        if (g.online_thread.joinable()) g.online_thread.join();
        if (g.ports_mapped) {   // close the ports this launcher opened
            std::string ignored;
            online::igd_delete_udp_mapping(g.igd, online::kDirectoryPort, &ignored);
            online::igd_delete_udp_mapping(g.igd, online::kGamePort, &ignored);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK nav_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wp == VK_DOWN || wp == VK_UP)) {
        const int p = GetDlgCtrlID(h) - IdNavFirst;
        const int n = (p + (wp == VK_DOWN ? 1 : PageCount - 1)) % PageCount;
        SetFocus(ctl(IdNavFirst + n));
        show_page(n);
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        const int old = g.hover_nav;
        g.hover_nav = -1;
        if (old >= 0) InvalidateRect(ctl(IdNavFirst + old), nullptr, FALSE);
    }
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show_cmd) {
    // One launcher at a time: a second start brings the first one forward.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) {
            // A launcher from another folder (for example an older version) is open: say so, rather than silently
            // bringing that one forward, so the player does not end up playing with the other version.
            wchar_t mine[MAX_PATH * 2] = L"", theirs[MAX_PATH * 2] = L"";
            GetModuleFileNameW(nullptr, mine, MAX_PATH * 2);
            DWORD pid = 0, n = MAX_PATH * 2;
            GetWindowThreadProcessId(other, &pid);
            if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                if (!QueryFullProcessImageNameW(proc, 0, theirs, &n)) theirs[0] = 0;
                CloseHandle(proc);
            }
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
            if (theirs[0] && lstrcmpiW(mine, theirs) != 0) {
                const std::wstring msg = std::wstring(L"Another JStarsReborn launcher is already open:\n") + theirs +
                    L"\n\nOnly one launcher can run at a time. Close that one, then start this one (JStarsReborn " + W(launcher_version) + L") again.";
                MessageBoxW(other, msg.c_str(), L"JStarsReborn", MB_ICONINFORMATION | MB_SETFOREGROUND);
            }
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = inst;
    g.exe = exe_dir() / "JStarsReborn.exe";
    g.runtime_exe = exe_dir() / "runtime" / "jstars-runtime.exe";
    try {
        init_data();
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, (L"JStarsReborn cannot use its data folder:\n" + W(e.what())).c_str(), L"JStarsReborn", MB_ICONERROR);
        return 1;
    }
    log_line(std::string("launcher ") + launcher_version + " started from " + u8(exe_dir()));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"JStarsReborn", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(980, static_cast<int>(dpi), 96), MulDiv(660, static_cast<int>(dpi), 96), nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    for (int p = 0; p < PageCount; ++p) SetWindowSubclass(ctl(IdNavFirst + p), nav_subclass, 1, 0);
    ShowWindow(hwnd, show_cmd);
    UpdateWindow(hwnd);
    SetFocus(ctl(IdPlay));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
