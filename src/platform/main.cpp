#include "reborn/core.hpp"
#include "reborn/renderer.hpp"
#include "reborn/audio.hpp"
#include "reborn/input.hpp"
#include "reborn/crash.hpp"
#include "reborn/paths.hpp"
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Options {
    bool warp=false, validation=false, no_audio=false, no_dialogs=false;
    bool resize_smoke=false, fail_init=false, borderless=false;
    unsigned smoke_frames=0, width=0, height=0;
    std::filesystem::path data_dir;
    bool explicit_data_dir=false;
    std::wstring crash_test;
};
std::filesystem::path executable_dir() {
    std::vector<wchar_t> buffer(32768);
    DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if (!n || n >= buffer.size()) throw std::runtime_error("Cannot locate executable directory");
    return std::filesystem::path(std::wstring(buffer.data(),n)).parent_path();
}
unsigned number(const std::wstring& s, unsigned low, unsigned high) {
    unsigned n=0;
    if (s.empty()) throw std::invalid_argument("Missing numeric argument");
    for (wchar_t c:s) {
        if(c<L'0'||c>L'9'||n>high/10) throw std::invalid_argument("Invalid numeric argument");
        n=n*10+static_cast<unsigned>(c-L'0');
        if(n>high) throw std::invalid_argument("Numeric argument exceeds supported limit");
    }
    if(n<low) throw std::invalid_argument("Numeric argument is below supported limit");
    return n;
}
Options options() {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv) throw std::runtime_error("Cannot parse command line");
    std::unique_ptr<void,decltype(&LocalFree)> guard(argv,LocalFree);
    Options o; o.data_dir=reborn::default_data_dir();
    for(int i=1;i<argc;++i) {
        const std::wstring arg=argv[i];
        auto value=[&]() -> std::wstring {if(++i>=argc)throw std::invalid_argument("Missing command-line value");return argv[i];};
        if(arg==L"--warp")o.warp=true;
        else if(arg==L"--validation")o.validation=true;
        else if(arg==L"--no-audio")o.no_audio=true;
        else if(arg==L"--no-dialogs")o.no_dialogs=true;
        else if(arg==L"--resize-smoke")o.resize_smoke=true;
        else if(arg==L"--fail-init")o.fail_init=true;
        else if(arg==L"--borderless")o.borderless=true;
        else if(arg==L"--smoke-frames")o.smoke_frames=number(value(),1,1000000);
        else if(arg==L"--width")o.width=number(value(),320,7680);
        else if(arg==L"--height")o.height=number(value(),240,4320);
        else if(arg==L"--data-dir"){o.data_dir=std::filesystem::absolute(value());o.explicit_data_dir=true;}
        else if(arg==L"--crash-test") {
            o.crash_test=value();
            if(o.crash_test!=L"access"&&o.crash_test!=L"terminate")throw std::invalid_argument("--crash-test accepts access or terminate");
        }
        else throw std::invalid_argument("Unknown option; see docs/BUILD.md for supported switches");
    }
    if(o.resize_smoke && o.smoke_frames<40)throw std::invalid_argument("--resize-smoke requires --smoke-frames >=40");
    return o;
}
struct WindowState {
    bool running=true, focused=true, minimized=false, resize=false;
    bool tone=false, toggle_vsync=false, toggle_borderless=false;
    unsigned width=1280,height=720;
};
LRESULT CALLBACK window_proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) noexcept {
    auto* state=reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        state=static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(!state)return DefWindowProcW(hwnd,message,wparam,lparam);
    switch(message) {
    case WM_GETMINMAXINFO: {
        auto* limits=reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMaxTrackSize={16384,16384};
        RECT minimum{0,0,320,240};
        AdjustWindowRectEx(&minimum,WS_OVERLAPPEDWINDOW,FALSE,0);
        limits->ptMinTrackSize={minimum.right-minimum.left,minimum.bottom-minimum.top};
        return 0;
    }
    case WM_CLOSE: state->running=false; return 0;
    case WM_DESTROY: state->running=false; return 0;
    case WM_SIZE:
        state->width=LOWORD(lparam);state->height=HIWORD(lparam);
        state->minimized=wparam==SIZE_MINIMIZED || !state->width || !state->height;
        state->resize=true;return 0;
    case WM_ACTIVATEAPP: state->focused=wparam!=0;return 0;
    case WM_KEYDOWN:
        if(lparam & (1LL<<30))return 0;
        if(wparam==VK_ESCAPE)state->running=false;
        if(wparam==VK_F1)state->tone=true;
        if(wparam==VK_F2)state->toggle_vsync=true;
        if(wparam==VK_F3)state->toggle_borderless=true;
        return 0;
    case WM_DPICHANGED: {
        const auto* r=reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;
    }
    default:return DefWindowProcW(hwnd,message,wparam,lparam);
    }
}
struct NativeWindow {
    HWND hwnd=nullptr;
    ~NativeWindow(){if(hwnd)DestroyWindow(hwnd);}
};
struct ComSession {
    bool initialized=false;
    ComSession(){const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr))throw std::runtime_error("COM initialization failed");initialized=true;}
    ~ComSession(){if(initialized)CoUninitialize();}
};
void size_client(HWND hwnd,unsigned width,unsigned height) {
    RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE));
    if(!AdjustWindowRectEx(&rect,style,FALSE,0))throw std::runtime_error("AdjustWindowRectEx failed");
    if(!SetWindowPos(hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))throw std::runtime_error("Window resize failed");
}
void borderless(HWND hwnd,bool enabled,RECT& previous) {
    if(enabled) {
        if(!GetWindowRect(hwnd,&previous))throw std::runtime_error("Cannot read window bounds");
        MONITORINFO monitor{sizeof(MONITORINFO),{},{},0};
        if(!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))throw std::runtime_error("Cannot read display bounds");
        SetWindowLongPtrW(hwnd,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        const auto& r=monitor.rcMonitor;
        if(!SetWindowPos(hwnd,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_FRAMECHANGED|SWP_NOACTIVATE))throw std::runtime_error("Borderless transition failed");
    } else {
        SetWindowLongPtrW(hwnd,GWL_STYLE,WS_OVERLAPPEDWINDOW|WS_VISIBLE);
        if(!SetWindowPos(hwnd,nullptr,previous.left,previous.top,previous.right-previous.left,previous.bottom-previous.top,SWP_FRAMECHANGED|SWP_NOZORDER|SWP_NOACTIVATE))throw std::runtime_error("Windowed transition failed");
    }
}
int run(HINSTANCE instance,const Options& o,reborn::Logger& log) {
    ComSession com;
    log.write("info","startup","Native Windows x64 foundation. No original game data or gameplay is implemented.");
    {
        // Surface earlier crashes on the next launch instead of letting them go unnoticed.
        std::error_code ignored;
        unsigned previous=0;std::string newest;
        for(const auto& entry:std::filesystem::directory_iterator(o.data_dir/L"crashes",ignored)) {
            if(entry.path().extension()==L".json"){++previous;const auto name=entry.path().filename().u8string();if(std::string(name.begin(),name.end())>newest)newest=std::string(name.begin(),name.end());}
        }
        if(previous)log.write("warning","previous_crash_reports",std::to_string(previous)+" report(s) in crashes/; newest "+newest);
    }
    if(!o.crash_test.empty()){log.write("warning","crash_test","Deliberate crash requested for reporter verification");reborn::trigger_test_crash(o.crash_test);}
    if(o.fail_init)throw std::runtime_error("Requested initialization failure for regression testing");
    auto config_path=o.data_dir/L"config"/L"settings.ini";
    auto settings=reborn::load_settings(config_path,log);
    bool settings_dirty=!std::filesystem::exists(config_path);
    if(o.width)settings.width=o.width;
    if(o.height)settings.height=o.height;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WindowState state;state.width=settings.width;state.height=settings.height;
    WNDCLASSEXW klass{};klass.cbSize=sizeof(klass);klass.lpfnWndProc=window_proc;
    klass.hInstance=instance;klass.lpszClassName=L"JStarsRebornNativeFoundation";
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassExW(&klass))throw std::runtime_error("RegisterClassExW failed");
    NativeWindow window;
    window.hwnd=CreateWindowExW(0,klass.lpszClassName,L"J-Stars Reborn | Native foundation | F1 tone  F2 VSync  F3 borderless  Esc quit",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,640,480,nullptr,nullptr,instance,&state);
    if(!window.hwnd)throw std::runtime_error("CreateWindowExW failed");
    size_client(window.hwnd,settings.width,settings.height);
    reborn::Renderer renderer;
    renderer.initialize(window.hwnd,state.width,state.height,o.warp,o.validation);
    log.write("info","graphics_ready",renderer.adapter_name());
    log.write("info","graphics_dimensions",std::to_string(state.width)+"x"+std::to_string(state.height));
    reborn::AudioSystem audio;
    if(!o.no_audio) {
        try {audio.initialize(settings.master_volume);log.write("info","audio_ready","XAudio2 mastering and source voices initialized; F1 plays a procedural diagnostic tone");}
        catch(const std::exception& e){log.write("warning","audio_unavailable",e.what());}
    } else log.write("info","audio_disabled","Requested by command line");
    reborn::InputSystem input;
    std::array<bool,4> last_pads{};
    reborn::FixedStep scheduler(settings.fixed_hz);
    ShowWindow(window.hwnd,SW_SHOW);
    state.resize=false;
    bool fullscreen=false;RECT previous{};
    if(o.borderless){borderless(window.hwnd,true,previous);fullscreen=true;log.write("info","display_mode","borderless");}
    auto previous_time=Clock::now();
    auto title_time=previous_time;
    std::vector<double> frame_times;frame_times.reserve(60000);
    unsigned frames=0;uint64_t ticks=0;
    while(state.running) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)state.running=false;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(!state.running)break;
        input.poll(state.focused);
        const auto pads=input.pads();
        for(size_t i=0;i<pads.size();++i)if(pads[i].connected!=last_pads[i]) {
            log.write("info","controller",std::string("XInput slot ")+std::to_string(i)+(pads[i].connected?" connected":" disconnected"));last_pads[i]=pads[i].connected;
        }
        if(state.minimized){previous_time=Clock::now();Sleep(10);continue;}
        if(state.toggle_vsync){settings.vsync=!settings.vsync;settings_dirty=true;state.toggle_vsync=false;log.write("info","vsync",settings.vsync?"enabled":"disabled");}
        if(state.toggle_borderless){fullscreen=!fullscreen;borderless(window.hwnd,fullscreen,previous);state.toggle_borderless=false;log.write("info","display_mode",fullscreen?"borderless":"windowed");}
        if(state.tone){try{audio.play_test_tone();log.write("info","test_tone",audio.available()?"submitted":"audio unavailable");}catch(const std::exception& e){log.write("warning","audio_playback_failed",e.what());}state.tone=false;}
        if(state.resize) {
            renderer.resize(state.width,state.height);state.resize=false;
            log.write("info","resize",std::to_string(state.width)+"x"+std::to_string(state.height));
            if(!fullscreen && !o.resize_smoke && state.width>=320 && state.width<=7680 && state.height>=240 && state.height<=4320) {
                if(settings.width!=state.width || settings.height!=state.height) {settings.width=state.width;settings.height=state.height;settings_dirty=true;}
            }
        }
        const auto frame_start=Clock::now();
        const double elapsed=std::chrono::duration<double>(frame_start-previous_time).count();previous_time=frame_start;
        const auto step=scheduler.advance(elapsed);ticks+=step.steps;
        // Scheduling is exercised only: no combat, physics or animation behavior is invented here.
        if(step.dropped_seconds>0)log.write("warning","simulation_backlog_dropped",std::to_string(step.dropped_seconds));
        renderer.render(settings.vsync);
        const double frame_ms=std::chrono::duration<double,std::milli>(Clock::now()-frame_start).count();
        if(frame_times.size()<60000)frame_times.push_back(frame_ms);
        ++frames;
        if(o.resize_smoke) {
            if(frames==10)size_client(window.hwnd,800,600);
            if(frames==20)size_client(window.hwnd,1920,1080);
            if(frames==30)size_client(window.hwnd,settings.width,settings.height);
        }
        if(Clock::now()-title_time>std::chrono::seconds(1)) {
            std::wostringstream title;title<<L"J-Stars Reborn | FOUNDATION ONLY | "<<state.width<<L"x"<<state.height<<L" | frame "<<frames<<L" | F1 tone / F2 VSync / F3 display / Esc";
            SetWindowTextW(window.hwnd,title.str().c_str());title_time=Clock::now();
        }
        if(o.smoke_frames && frames>=o.smoke_frames)state.running=false;
    }
    input.stop_vibration();
    renderer.wait_idle();
    renderer.shutdown();
    if(settings_dirty)reborn::save_settings(config_path,settings);
    if(!frame_times.empty()) {
        std::sort(frame_times.begin(),frame_times.end());
        const auto percentile=[&](double p){return frame_times[static_cast<size_t>(p*static_cast<double>(frame_times.size()-1))];};
        std::ostringstream metrics;metrics<<"clear-only,frames="<<frames<<",samples="<<frame_times.size()<<",mean_ms="<<std::accumulate(frame_times.begin(),frame_times.end(),0.0)/static_cast<double>(frame_times.size())<<",p95_ms="<<percentile(.95)<<",p99_ms="<<percentile(.99)<<",scheduler_ticks="<<ticks;
        log.write("info","frame_summary",metrics.str());
    }
    log.write("info","shutdown","Clean shutdown completed");
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    std::unique_ptr<reborn::Logger> log;
    const bool quiet=std::wstring(GetCommandLineW()).find(L"--no-dialogs")!=std::wstring::npos;
    try {
        auto o=options();
        std::filesystem::create_directories(o.data_dir/L"config");
        std::filesystem::create_directories(o.data_dir/L"logs");
        log=std::make_unique<reborn::Logger>(o.data_dir/L"logs"/L"runtime.jsonl");
        reborn::install_crash_reporter(o.data_dir/L"crashes");
        log->write("info","data_dir",std::string(o.explicit_data_dir?"explicit: ":"per-user: ")+[&]{auto u=o.data_dir.u8string();return std::string(u.begin(),u.end());}());
        if(!o.explicit_data_dir) {
            // 0.2.1 and earlier stored settings beside the executable. Copy them once into the
            // per-user location; the original is left untouched. Old logs/crash reports stay put.
            try {
                const auto result=reborn::migrate_legacy_settings(executable_dir()/L"config"/L"settings.ini",o.data_dir/L"config"/L"settings.ini");
                log->write(result==reborn::MigrationResult::Migrated?"warning":"info","settings_migration",reborn::to_string(result));
            } catch(const std::exception& e) {
                log->write("warning","settings_migration_failed",e.what());
            }
        }
        if(quiet)SetErrorMode(GetErrorMode()|SEM_NOGPFAULTERRORBOX);
        return run(instance,o,*log);
    } catch(const std::exception& e) {
        try {if(log)log->write("error","fatal",e.what());}catch(...){}
        OutputDebugStringA(e.what());
        if(!quiet)MessageBoxA(nullptr,e.what(),"J-Stars Reborn foundation - startup/runtime failure",MB_OK|MB_ICONERROR);
        return 1;
    }
}
