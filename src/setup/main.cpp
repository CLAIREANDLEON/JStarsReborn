#include <windows.h>

#include "reborn/pkg_intake.hpp"
#include "reborn/game_validate.hpp"
#include "reborn/paths.hpp"
#include "reborn/setup_install.hpp"
#include "reborn/setup_inventory.hpp"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <optional>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>

namespace {
namespace fsp = std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr int id_source = 110, id_destination = 111, id_pkg = 120,
              id_folder = 121, id_inspect = 122, id_report = 123,
              id_parent = 124, id_install = 125, id_launch = 126,
              id_cancel = 127, id_status = 130, id_details = 131,
              id_progress = 132;
struct Options {
  fsp::path package, source, destination, report, versions;
  std::wstring mode;
  unsigned smoke_ms = 0;
};
fsp::path executable_folder() {
  std::wstring s(32768, L'\0');
  auto n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
  if (!n || n >= s.size())
    throw std::runtime_error("Cannot locate setup files");
  s.resize(n);
  return fsp::path(s).parent_path();
}
std::wstring wide(const std::string &text) {
  if (text.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), nullptr, 0);
  if (!n)
    return L"The operation failed; the diagnostic text could not be decoded.";
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), out.data(), n);
  return out;
}
Options parse() {
  Options o;
  o.package = executable_folder();
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv)
    throw std::runtime_error("Cannot read setup arguments");
  std::unique_ptr<void, decltype(&LocalFree)> memory(argv, LocalFree);
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto value = [&]() {
      if (++i >= argc)
        throw std::runtime_error("Missing setup option value");
      return std::wstring(argv[i]);
    };
    if (a == L"--package")
      o.package = fsp::absolute(value());
    else if (a == L"--source")
      o.source = fsp::absolute(value());
    else if (a == L"--destination")
      o.destination = fsp::absolute(value());
    else if (a == L"--report")
      o.report = fsp::absolute(value());
    else if (a == L"--inspect" || a == L"--inspect-pkg") {
      o.mode = a;
      o.source = fsp::absolute(value());
    } else if (a == L"--validate-game") {
      o.mode = a;
      o.source = fsp::absolute(value());
    } else if (a == L"--versions") {
      o.versions = fsp::absolute(value());
    } else if (a == L"--install") {
      o.mode = a;
      o.destination = fsp::absolute(value());
    } else if (a == L"--ui-smoke-ms") {
      auto v = value();
      if (v.empty() || v.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error("Invalid UI smoke duration");
      auto n = std::stoul(v);
      if (n < 100 || n > 10000)
        throw std::runtime_error("UI smoke duration must be 100..10000 ms");
      o.smoke_ms = static_cast<unsigned>(n);
    } else
      throw std::runtime_error("Unknown setup option");
  }
  return o;
}
std::wstring control_text(HWND h) {
  int n = GetWindowTextLengthW(h);
  std::wstring s(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(static_cast<size_t>(n));
  return s;
}
bool within(const fsp::path &child, const fsp::path &parent) {
  auto c = fsp::weakly_canonical(child).lexically_normal();
  auto p = fsp::weakly_canonical(parent).lexically_normal();
  auto ci = c.begin();
  for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci) {
    if (ci == c.end() || CompareStringOrdinal(ci->c_str(), -1, pi->c_str(), -1,
                                              TRUE) != CSTR_EQUAL)
      return false;
  }
  return true;
}
std::optional<fsp::path> choose(HWND owner, bool folder, const wchar_t *title) {
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    throw std::runtime_error("Windows file picker is unavailable");
  DWORD flags = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
  if (folder)
    flags |= FOS_PICKFOLDERS;
  else
    flags |= FOS_FILEMUSTEXIST;
  dialog->SetOptions(flags);
  dialog->SetTitle(title);
  if (!folder) {
    COMDLG_FILTERSPEC filter[]{{L"PlayStation package (*.pkg)", L"*.pkg"}};
    dialog->SetFileTypes(1, filter);
  }
  auto hr = dialog->Show(owner);
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    return std::nullopt;
  if (FAILED(hr))
    throw std::runtime_error("File picker failed");
  ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item)))
    throw std::runtime_error("Cannot read selected path");
  PWSTR raw = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
    throw std::runtime_error("Selected item is not a local path");
  fsp::path selected(raw);
  CoTaskMemFree(raw);
  return selected;
}
struct Outcome {
  bool ok = false, inspection = false, partial = false;
  std::wstring text;
  fsp::path report, executable;
};
struct App {
  HWND hwnd = nullptr;
  Options options;
  std::vector<HWND> children;
  HFONT normal = nullptr, title = nullptr, bold = nullptr;
  HBRUSH body = nullptr, card = nullptr, header = nullptr;
  std::jthread worker;
  std::atomic<uint64_t> files{0}, bytes{0};
  std::atomic<bool> done{false};
  std::mutex mutex;
  Outcome outcome;
  bool busy = false, close_requested = false;
  uint64_t auto_close = 0;
  fsp::path last_report, installed;
  ~App() {
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
    for (auto font : {normal, title, bold})
      if (font)
        DeleteObject(font);
    for (auto brush : {body, card, header})
      if (brush)
        DeleteObject(brush);
  }
  HWND get(int id) const { return GetDlgItem(hwnd, id); }
  int unit(int n) const {
    return MulDiv(n, static_cast<int>(GetDpiForWindow(hwnd)), 96);
  }
  void text(int id, const std::wstring &s) {
    SetWindowTextW(get(id), s.c_str());
  }
  void enable() {
    for (int id : {id_source, id_destination, id_pkg, id_folder, id_inspect,
                   id_parent, id_install})
      EnableWindow(get(id), !busy);
    EnableWindow(get(id_inspect),
                 !busy && !control_text(get(id_source)).empty());
    EnableWindow(get(id_install),
                 !busy && !control_text(get(id_destination)).empty());
    EnableWindow(get(id_report), !busy && !last_report.empty());
    EnableWindow(get(id_launch), !busy && !installed.empty());
    EnableWindow(get(id_cancel), busy && !close_requested);
  }
  HWND add(int id, const wchar_t *type, const wchar_t *label, DWORD style = 0) {
    auto h = CreateWindowExW(
        type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, label,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (!h)
      throw std::runtime_error("Cannot create setup control");
    children.push_back(h);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(normal), TRUE);
    return h;
  }
  void fonts() {
    for (auto font : {normal, title, bold})
      if (font)
        DeleteObject(font);
    auto make = [&](int size, int weight) {
      return CreateFontW(-unit(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
    };
    normal = make(15, FW_NORMAL);
    title = make(29, FW_SEMIBOLD);
    bold = make(17, FW_SEMIBOLD);
    for (auto h : children)
      SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(normal), TRUE);
    for (int id : {201, 210, 220, id_status})
      if (get(id))
        SendMessageW(get(id), WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
    if (get(200))
      SendMessageW(get(200), WM_SETFONT, reinterpret_cast<WPARAM>(title), TRUE);
  }
  void create() {
    body = CreateSolidBrush(RGB(244, 247, 251));
    card = CreateSolidBrush(RGB(255, 255, 255));
    header = CreateSolidBrush(RGB(20, 34, 54));
    fonts();
    add(200, L"STATIC", L"J-STARS REBORN");
    add(201, L"STATIC", L"Native PC setup");
    add(202, L"STATIC", L"FOUNDATION  /  0.2", SS_RIGHT);
    add(203, L"STATIC",
        L"Choose a source package. Install the native runtime.");
    add(204, L"STATIC",
        L"Game conversion is unavailable: no verified PS4 game importer is "
        L"included.");
    add(210, L"STATIC", L"01   Import a source");
    add(211, L"STATIC",
        L"Register a PKG file or inspect an extracted folder.\r\nSource files "
        L"are read without changing them.");
    add(212, L"STATIC", L"PKG file or extracted-data folder");
    add(id_source, L"EDIT", options.source.c_str(),
        ES_AUTOHSCROLL | WS_TABSTOP);
    SendMessageW(get(id_source), EM_SETLIMITTEXT, 30000, 0);
    SendMessageW(get(id_source), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Select your local .pkg file"));
    add(id_pkg, L"BUTTON", L"Choose PKG...", BS_PUSHBUTTON | WS_TABSTOP);
    add(id_folder, L"BUTTON", L"Choose folder...", BS_PUSHBUTTON | WS_TABSTOP);
    add(id_inspect, L"BUTTON", L"Import source", BS_PUSHBUTTON | WS_TABSTOP);
    add(id_report, L"BUTTON", L"Open report", BS_PUSHBUTTON | WS_TABSTOP);
    add(213, L"STATIC",
        L"Package registration does not extract game content\r\nor convert "
        L"console code to a PC game.");
    add(220, L"STATIC", L"02   Install the native runtime");
    add(221, L"STATIC",
        L"Install the current diagnostic application.\r\nCharacters, battles "
        L"and online play are not included.");
    add(222, L"STATIC", L"New installation folder");
    if (options.destination.empty())
      options.destination = options.package.parent_path() / L"JStarsReborn-PC";
    add(id_destination, L"EDIT", options.destination.c_str(),
        ES_AUTOHSCROLL | WS_TABSTOP);
    SendMessageW(get(id_destination), EM_SETLIMITTEXT, 30000, 0);
    add(id_parent, L"BUTTON", L"Choose parent...", BS_PUSHBUTTON | WS_TABSTOP);
    add(id_install, L"BUTTON", L"Install foundation",
        BS_PUSHBUTTON | WS_TABSTOP);
    add(id_launch, L"BUTTON", L"Launch installed", BS_PUSHBUTTON | WS_TABSTOP);
    add(223, L"STATIC",
        L"Uses a new folder and checks each copied file.\r\nExisting "
        L"installations are never overwritten.");
    add(id_status, L"STATIC", L"Ready");
    add(id_progress, PROGRESS_CLASSW, L"", PBS_MARQUEE);
    add(id_cancel, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP);
    add(id_details, L"EDIT",
        L"Start with Choose PKG to register a local package. The foundation "
        L"can also be installed without game data.",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
    fonts();
    enable();
    SetTimer(hwnd, 1, 100, nullptr);
    if (options.smoke_ms)
      auto_close = GetTickCount64() + options.smoke_ms;
  }
  void layout() {
    RECT r{};
    GetClientRect(hwnd, &r);
    int w = MulDiv(r.right, 96, static_cast<int>(GetDpiForWindow(hwnd))),
        h = MulDiv(r.bottom, 96, static_cast<int>(GetDpiForWindow(hwnd)));
    int cw = (w - 88) / 2, rx = 56 + cw;
    auto at = [&](int id, int x, int y, int width, int height) {
      SetWindowPos(get(id), nullptr, unit(x), unit(y), unit(width),
                   unit(height), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    at(200, 32, 22, w - 350, 38);
    at(201, 34, 65, w - 200, 26);
    at(202, w - 295, 33, 260, 26);
    at(203, 32, 125, w - 64, 27);
    at(204, 32, 158, w - 64, 26);
    at(210, 48, 217, cw - 32, 28);
    at(211, 48, 258, cw - 32, 48);
    at(212, 48, 320, cw - 32, 22);
    at(id_source, 48, 346, cw - 32, 30);
    int bw = (cw - 48) / 3;
    at(id_pkg, 48, 388, bw, 34);
    at(id_folder, 56 + bw, 388, bw, 34);
    at(id_inspect, 64 + bw * 2, 388, bw, 34);
    at(id_report, 48, 434, bw, 31);
    at(213, 48, 480, cw - 32, 44);
    at(220, rx + 16, 217, cw - 32, 28);
    at(221, rx + 16, 258, cw - 32, 48);
    at(222, rx + 16, 320, cw - 32, 22);
    at(id_destination, rx + 16, 346, cw - 32, 30);
    at(id_parent, rx + 16, 388, bw, 34);
    at(id_install, rx + 24 + bw, 388, bw, 34);
    at(id_launch, rx + 32 + bw * 2, 388, bw, 34);
    at(223, rx + 16, 480, cw - 32, 44);
    at(id_status, 48, 578, w - 200, 25);
    at(id_progress, 48, 613, w - 230, 14);
    at(id_cancel, w - 160, 604, 112, 32);
    at(id_details, 48, 651, w - 96, std::max(56, h - 674));
    InvalidateRect(hwnd, nullptr, TRUE);
  }
  void paint() {
    PAINTSTRUCT ps{};
    auto dc = BeginPaint(hwnd, &ps);
    RECT r{};
    GetClientRect(hwnd, &r);
    FillRect(dc, &r, body);
    RECT head{0, 0, r.right, unit(106)};
    FillRect(dc, &head, header);
    int w = MulDiv(r.right, 96, static_cast<int>(GetDpiForWindow(hwnd))),
        cw = (w - 88) / 2;
    auto pen = CreatePen(PS_SOLID, 1, RGB(220, 226, 235));
    auto oldPen = SelectObject(dc, pen);
    auto oldBrush = SelectObject(dc, card);
    RoundRect(dc, unit(32), unit(200), unit(32 + cw), unit(544), unit(12),
              unit(12));
    RoundRect(dc, unit(56 + cw), unit(200), unit(w - 32), unit(544), unit(12),
              unit(12));
    RoundRect(dc, unit(32), unit(562), unit(w - 32), r.bottom - unit(20),
              unit(12), unit(12));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    EndPaint(hwnd, &ps);
  }
  void status(const std::wstring &heading, const std::wstring &detail) {
    text(id_status, heading);
    text(id_details, detail);
  }
  void start(bool inspect) {
    if (busy)
      return;
    auto src = control_text(get(id_source));
    auto dst = control_text(get(id_destination));
    if (inspect && src.empty())
      throw std::runtime_error(
          "Choose a PKG file or extracted-data folder first");
    if (!inspect && dst.empty())
      throw std::runtime_error("Choose a new installation folder first");
    if (!inspect && !src.empty()) {
      fsp::path s = fsp::absolute(src), d = fsp::absolute(dst);
      if (fsp::is_regular_file(s))
        s = s.parent_path();
      if (within(d, s) || within(s, d))
        throw std::runtime_error("Choose an installation folder separate from "
                                 "the selected game-data folder");
    }
    if (inspect)
      last_report.clear();
    else
      installed.clear();
    files = 0;
    bytes = 0;
    done = false;
    busy = true;
    enable();
    SendMessageW(get(id_progress), PBM_SETMARQUEE, TRUE, 30);
    status(inspect ? L"Inspecting source..." : L"Installing foundation...",
           L"The operation can be cancelled. The original source files are not "
           L"modified.");
    auto package = options.package;
    worker = std::jthread([this, inspect, src, dst,
                           package](std::stop_token stop) {
      Outcome result;
      result.inspection = inspect;
      try {
        auto progress = [&](uint64_t f, uint64_t b) {
          files = f;
          bytes = b;
        };
        if (inspect) {
          // Per-user location: the setup folder may be read-only or shared.
          auto report = reborn::default_data_dir() / L"reports" /
                        (L"source-" + std::to_wstring(GetCurrentProcessId()) +
                         L"-" + std::to_wstring(GetTickCount64()) + L".json");
          if (fsp::is_regular_file(src)) {
            auto pkg = reborn::setup::inspect_pkg(src, report, stop, progress);
            result.text = L"PKG source registered. No game content was "
                          L"extracted or converted.\r\n" +
                          std::to_wstring(pkg.bytes) + L" bytes inspected.\r\n";
            if (!pkg.content_id.empty())
              result.text += L"Content ID: " + wide(pkg.content_id) + L"\r\n";
          } else {
            auto inventory =
                reborn::setup::inventory_folder(src, report, stop, progress);
            result.partial = !inventory.complete;
            result.text = std::to_wstring(inventory.files) +
                          L" files inventoried. " +
                          std::to_wstring(inventory.diagnostics.size()) +
                          L" diagnostics.\r\nNo compatible game importer is "
                          L"implemented.\r\n";
            for (size_t i = 0;
                 i < std::min<size_t>(inventory.diagnostics.size(), 4); ++i)
              result.text += wide(inventory.diagnostics[i]) + L"\r\n";
          }
          result.report = report;
          result.text += L"Report: " + report.wstring();
        } else {
          auto installed_result = reborn::setup::install_foundation(
              package, fsp::absolute(dst), stop, progress);
          result.executable = installed_result.executable;
          result.text = L"Installed " +
                        std::to_wstring(installed_result.files) +
                        L" verified files.\r\n" + result.executable.wstring() +
                        L"\r\nLaunch opens the native diagnostic window, not a "
                        L"playable game.";
        }
        result.ok = true;
      } catch (const std::exception &e) {
        result.text = wide(e.what());
      } catch (...) {
        result.text = L"Unexpected setup failure.";
      }
      {
        std::lock_guard lock(mutex);
        outcome = std::move(result);
      }
      done.store(true, std::memory_order_release);
    });
  }
  void poll() {
    if (busy) {
      text(id_status, L"Working  ·  " + std::to_wstring(files.load()) +
                          L" files  ·  " +
                          std::to_wstring(bytes.load() / 1024) + L" KiB");
      if (done.load(std::memory_order_acquire)) {
        worker.join();
        Outcome result;
        {
          std::lock_guard lock(mutex);
          result = std::move(outcome);
        }
        busy = false;
        SendMessageW(get(id_progress), PBM_SETMARQUEE, FALSE, 0);
        SendMessageW(get(id_progress), PBM_SETPOS, 0, 0);
        if (result.ok) {
          last_report = result.inspection ? result.report : last_report;
          if (!result.inspection)
            installed = result.executable;
          status(result.inspection
                     ? (result.partial
                            ? L"Inspection incomplete — review diagnostics"
                            : L"Source registered — conversion unavailable")
                     : L"Foundation installed",
                 result.text);
        } else
          status(L"Operation stopped", result.text);
        enable();
        if (close_requested) {
          DestroyWindow(hwnd);
          return;
        }
      }
    }
    if (auto_close && GetTickCount64() >= auto_close) {
      auto_close = 0;
      SendMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }
  void command(int id, int event) {
    if ((id == id_source || id == id_destination) && event == EN_CHANGE) {
      if (id == id_source)
        last_report.clear();
      else
        installed.clear();
      enable();
      return;
    }
    if (event != BN_CLICKED)
      return;
    if (id == id_pkg || id == id_folder) {
      auto p = choose(hwnd, id == id_folder,
                      id == id_folder ? L"Choose extracted game-data folder"
                                      : L"Choose a locally supplied PKG file");
      if (p)
        text(id_source, p->wstring());
    } else if (id == id_parent) {
      auto p = choose(hwnd, true,
                      L"Choose the parent for a new installation folder");
      if (p)
        text(id_destination, (*p / L"JStarsReborn-PC").wstring());
    } else if (id == id_inspect)
      start(true);
    else if (id == id_install)
      start(false);
    else if (id == id_cancel) {
      worker.request_stop();
      status(L"Cancelling...",
             L"Waiting for the current bounded operation to stop safely.");
      EnableWindow(get(id_cancel), FALSE);
    } else if (id == id_report && !last_report.empty()) {
      auto result = reinterpret_cast<INT_PTR>(
          ShellExecuteW(hwnd, L"open", last_report.c_str(), nullptr,
                        last_report.parent_path().c_str(), SW_SHOWNORMAL));
      if (result <= 32)
        throw std::runtime_error("No application could open the JSON report; "
                                 "its full path is shown below");
    } else if (id == id_launch && !installed.empty()) {
      std::wstring command = L"\"" + installed.wstring() + L"\"";
      STARTUPINFOW info{};
      info.cb = sizeof(info);
      PROCESS_INFORMATION process{};
      if (!CreateProcessW(installed.c_str(), command.data(), nullptr, nullptr,
                          FALSE, 0, nullptr, installed.parent_path().c_str(),
                          &info, &process))
        throw std::runtime_error("Cannot launch the installed foundation");
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      status(L"Foundation launched",
             L"The native diagnostic window is running. It is not a playable "
             L"game port.");
    }
  }
};
LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
  auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    app = static_cast<App *>(
        reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
    app->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (!app)
    return DefWindowProcW(hwnd, msg, wp, lp);
  try {
    switch (msg) {
    case WM_CREATE:
      app->create();
      return 0;
    case WM_SIZE:
      app->layout();
      return 0;
    case WM_PAINT:
      app->paint();
      return 0;
    case WM_COMMAND:
      app->command(LOWORD(wp), HIWORD(wp));
      return 0;
    case WM_TIMER:
      app->poll();
      return 0;
    case WM_GETMINMAXINFO: {
      auto *limits = reinterpret_cast<MINMAXINFO *>(lp);
      limits->ptMinTrackSize = {app->unit(980), app->unit(810)};
      return 0;
    }
    case WM_DPICHANGED: {
      auto *r = reinterpret_cast<RECT *>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      app->fonts();
      app->layout();
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      auto dc = reinterpret_cast<HDC>(wp);
      int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, (id >= 200 && id <= 202)
                           ? RGB(244, 248, 255)
                           : (id == 204 ? RGB(136, 76, 12) : RGB(29, 43, 63)));
      if ((id >= 200 && id <= 202))
        return reinterpret_cast<LRESULT>(app->header);
      if (id == 203 || id == 204)
        return reinterpret_cast<LRESULT>(app->body);
      return reinterpret_cast<LRESULT>(app->card);
    }
    case WM_CLOSE:
      if (app->busy) {
        app->close_requested = true;
        app->worker.request_stop();
        app->enable();
        app->status(L"Closing safely...",
                    L"Cancelling the current operation before closing.");
      } else
        DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, 1);
      PostQuitMessage(0);
      return 0;
    }
  } catch (const std::exception &e) {
    if (app->get(id_details))
      app->status(L"Unable to continue", wide(e.what()));
    else
      return -1;
  } catch (...) {
    if (app->get(id_details))
      app->status(L"Unable to continue", L"Unexpected setup error.");
    else
      return -1;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
int show_setup(HINSTANCE instance, Options o) {
  auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr))
    throw std::runtime_error("Cannot initialize Windows file dialogs");
  struct Com {
    ~Com() { CoUninitialize(); }
  } com;
  INITCOMMONCONTROLSEX controls{sizeof(controls),
                                ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
  if (!InitCommonControlsEx(&controls))
    throw std::runtime_error("Cannot initialize Windows controls");
  App app;
  app.options = std::move(o);
  WNDCLASSEXW klass{};
  klass.cbSize = sizeof(klass);
  klass.lpfnWndProc = procedure;
  klass.hInstance = instance;
  klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  klass.lpszClassName = L"JStarsRebornSetup";
  if (!RegisterClassExW(&klass))
    throw std::runtime_error("Cannot register setup window");
  HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, klass.lpszClassName,
                           L"J-Stars Reborn Setup", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1040, 850, nullptr,
                           nullptr, instance, &app);
  if (!h)
    throw std::runtime_error("Cannot create setup window");
  ShowWindow(h, SW_SHOW);
  UpdateWindow(h);
  MSG msg{};
  int result;
  while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (result < 0)
    throw std::runtime_error("Windows message loop failed");
  return 0;
}
// Read-only validation of an already-decrypted dump folder. Writes a new JSON report
// (never overwrites) outside the game folder. Exit 0 = structurally valid; this never
// means playable, because no runtime stage executes game content yet.
int validate_game_cli(const Options &o) {
  if (o.report.empty())
    throw std::runtime_error("--validate-game requires --report outside the game folder");
  if (within(o.report, o.source))
    throw std::runtime_error("The report must be outside the game folder");
  const auto manifest_path =
      o.versions.empty() ? o.package / L"supported-versions.tsv" : o.versions;
  std::ifstream manifest_stream(manifest_path, std::ios::binary);
  if (!manifest_stream)
    throw std::runtime_error("Cannot read the supported-version manifest (supported-versions.tsv)");
  std::string manifest_text((std::istreambuf_iterator<char>(manifest_stream)),
                            std::istreambuf_iterator<char>());
  const auto supported = reborn::setup::parse_supported_versions(manifest_text);
  const auto result = reborn::setup::validate_game_folder(o.source, supported);
  const auto json = reborn::setup::validation_json(result);
  HANDLE out = CreateFileW(o.report.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot create a new validation report (it must not already exist)");
  DWORD written = 0;
  const BOOL wrote = WriteFile(out, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) &&
                     written == json.size() && FlushFileBuffers(out);
  CloseHandle(out);
  if (!wrote) {
    DeleteFileW(o.report.c_str());
    throw std::runtime_error("Cannot write the validation report");
  }
  std::printf("Game folder %s. TITLE_ID=%s APP_VER=%s executable=%s build=%s. "
              "Not playable: this build has no graphics path; the execution runtime is still in development.\n",
              result.ok() ? "structurally valid" : "has problems",
              result.title_id.empty() ? "?" : result.title_id.c_str(),
              result.app_ver.empty() ? "?" : result.app_ver.c_str(),
              result.executable == reborn::setup::ExecutableKind::Elf ? "elf"
              : result.executable == reborn::setup::ExecutableKind::Ps4Self ? "self"
              : result.executable == reborn::setup::ExecutableKind::Unknown ? "unknown" : "missing",
              result.match == reborn::setup::SupportMatch::Verified ? "verified"
              : result.match == reborn::setup::SupportMatch::Untested ? "untested" : "unknown");
  return result.ok() ? 0 : 1;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  try {
    auto o = parse();
    if (o.mode == L"--inspect" || o.mode == L"--inspect-pkg") {
      if (o.report.empty())
        throw std::runtime_error(
            "Inspection requires --report outside the source");
      if (o.mode == L"--inspect-pkg") {
        auto result = reborn::setup::inspect_pkg(o.source, o.report);
        std::printf(
            "PKG source registered: %llu bytes. Game conversion unavailable.\n",
            static_cast<unsigned long long>(result.bytes));
        return 0;
      }
      auto result = reborn::setup::inventory_folder(o.source, o.report);
      std::printf("Inventory: %llu files. Game import unavailable.\n",
                  static_cast<unsigned long long>(result.files));
      return result.complete ? 0 : 1;
    }
    if (o.mode == L"--validate-game")
      return validate_game_cli(o);
    if (o.mode == L"--install") {
      auto result = reborn::setup::install_foundation(o.package, o.destination);
      std::printf("Foundation installed: %llu files.\n",
                  static_cast<unsigned long long>(result.files));
      return 0;
    }
    return show_setup(instance, std::move(o));
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Setup failed: %s\n", e.what());
    OutputDebugStringA(e.what());
    return 1;
  }
}
