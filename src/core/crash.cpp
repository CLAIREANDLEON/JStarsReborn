#include "reborn/crash.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <stdexcept>

namespace reborn {
namespace {
// Everything the filter needs is preallocated: the heap may be corrupt at crash time.
constexpr DWORD terminate_code = 0xE0524E54; // 'RNT': std::terminate was called
wchar_t g_directory[1024];
std::atomic<bool> g_reporting{false};

struct Text {
    char data[32768];
    std::size_t used = 0;
    void add(const char* s) noexcept { while (*s && used + 1 < sizeof(data)) data[used++] = *s++; data[used] = 0; }
    void hex(std::uint64_t value) noexcept {
        char buffer[24];
        // JSON has no hex literal: emit a quoted "0x..." string.
        std::snprintf(buffer, sizeof(buffer), "\"0x%llx\"", static_cast<unsigned long long>(value));
        add(buffer);
    }
    void number(std::uint64_t value) noexcept {
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
        add(buffer);
    }
    // JSON string from a wide path; escapes quotes, backslashes and controls.
    void wide_string(const wchar_t* s) noexcept {
        char utf8[2048];
        const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, sizeof(utf8), nullptr, nullptr);
        add("\"");
        if (n > 0) {
            for (const char* p = utf8; *p; ++p) {
                const auto c = static_cast<unsigned char>(*p);
                if (c == '"' || c == '\\') { char e[3] = {'\\', static_cast<char>(c), 0}; add(e); }
                else if (c < 0x20) { char e[8]; std::snprintf(e, sizeof(e), "\\u%04x", c); add(e); }
                else { char e[2] = {static_cast<char>(c), 0}; add(e); }
            }
        }
        add("\"");
    }
};
Text g_text;

const char* exception_name(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case terminate_code: return "STD_TERMINATE";
    default: return "UNKNOWN_EXCEPTION";
    }
}

void add_location(Text& t, DWORD64 address) noexcept {
    HMODULE module = nullptr;
    t.add("{\"address\":");
    t.hex(address);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) && module) {
        wchar_t name[MAX_PATH];
        const DWORD n = GetModuleFileNameW(module, name, MAX_PATH);
        const wchar_t* base = name;
        for (DWORD i = 0; i < n; ++i) if (name[i] == L'\\' || name[i] == L'/') base = name + i + 1;
        t.add(",\"module\":");
        t.wide_string(n ? base : L"?");
        t.add(",\"rva\":");
        t.hex(address - reinterpret_cast<DWORD64>(module));
    }
    t.add("}");
}

bool write_file(const wchar_t* path, const void* data, DWORD size) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, size, &written, nullptr) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

DWORD write_minidump(const wchar_t* path, EXCEPTION_POINTERS* info) noexcept {
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbghelp) return GetLastError() ? GetLastError() : ERROR_MOD_NOT_FOUND;
    auto write = reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    if (!write) return ERROR_PROC_NOT_FOUND;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file,
                          static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), &exception, nullptr, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) DeleteFileW(path);
    return ok ? ERROR_SUCCESS : (error ? error : ERROR_GEN_FAILURE);
}

LONG WINAPI filter(EXCEPTION_POINTERS* info) noexcept {
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : ERROR_GEN_FAILURE;
    if (g_reporting.exchange(true)) TerminateProcess(GetCurrentProcess(), code);

    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t stem[1200];
    std::swprintf(stem, sizeof(stem) / sizeof(stem[0]), L"%ls\\crash-%04u%02u%02uT%02u%02u%02u.%03uZ-%lu", g_directory,
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                  GetCurrentProcessId());
    wchar_t json_path[1300], dump_path[1300];
    std::swprintf(json_path, 1300, L"%ls.json", stem);
    std::swprintf(dump_path, 1300, L"%ls.dmp", stem);
    const DWORD dump_status = info ? write_minidump(dump_path, info) : ERROR_INVALID_PARAMETER;

    Text& t = g_text;
    t.used = 0;
    char time[40];
    std::snprintf(time, sizeof(time), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond, now.wMilliseconds);
    t.add("{\n  \"schema\":\"reborn.crash.v1\",\n  \"time\":\"");
    t.add(time);
    t.add("\",\n  \"process_id\":");
    t.number(GetCurrentProcessId());
    t.add(",\n  \"thread_id\":");
    t.number(GetCurrentThreadId());
    t.add(",\n  \"exception_code\":");
    t.hex(code);
    t.add(",\n  \"exception_name\":\"");
    t.add(exception_name(code));
    t.add("\"");
    if (info && info->ExceptionRecord) {
        const auto* record = info->ExceptionRecord;
        t.add(",\n  \"fault\":");
        add_location(t, reinterpret_cast<DWORD64>(record->ExceptionAddress));
        if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            const auto operation = record->ExceptionInformation[0];
            t.add(",\n  \"access\":\"");
            t.add(operation == 0 ? "read" : operation == 1 ? "write" : operation == 8 ? "execute" : "unknown");
            t.add("\",\n  \"target_address\":");
            t.hex(record->ExceptionInformation[1]);
        }
    }
    t.add(",\n  \"stack\":[");
    if (info && info->ContextRecord) {
#if defined(_M_X64) || defined(__x86_64__)
        CONTEXT context = *info->ContextRecord;
        for (int frame = 0; frame < 64 && context.Rip; ++frame) {
            if (frame) t.add(",");
            t.add("\n    ");
            add_location(t, context.Rip);
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
            if (function) {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
            } else {
                // Leaf function: the return address is at the top of the stack.
                if (!context.Rsp) break;
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
        }
#endif
    }
    t.add("\n  ],\n  \"minidump\":");
    if (dump_status == ERROR_SUCCESS) t.wide_string(dump_path);
    else { t.add("null,\n  \"minidump_error\":"); t.number(dump_status); }
    t.add(",\n  \"note\":\"Symbolize frames with the matching build's map/PDB using module + rva.\"\n}\n");
    write_file(json_path, t.data, static_cast<DWORD>(t.used));

    if (!(GetErrorMode() & SEM_NOGPFAULTERRORBOX)) {
        // Visible, not silent: tell the user where the report went.
        wchar_t message[1600];
        std::swprintf(message, 1600, L"J-Stars Reborn crashed (%hs).\nA report was saved to:\n%ls", exception_name(code), json_path);
        MessageBoxW(nullptr, message, L"J-Stars Reborn crash", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    }
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void on_terminate() noexcept {
    RaiseException(terminate_code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), terminate_code);
    for (;;) {}
}
}

void install_crash_reporter(const std::filesystem::path& crash_directory) {
    std::filesystem::create_directories(crash_directory);
    const auto& native = crash_directory.native();
    if (native.size() >= sizeof(g_directory) / sizeof(g_directory[0]))
        throw std::runtime_error("Crash report directory path is too long");
    std::wmemcpy(g_directory, native.c_str(), native.size() + 1);
    SetUnhandledExceptionFilter(filter);
    std::set_terminate(on_terminate);
}

[[noreturn]] void trigger_test_crash(std::wstring_view kind) {
    if (kind == L"terminate") std::terminate();
    volatile int* null_pointer = nullptr;
    *null_pointer = 0x52454242;
    std::terminate(); // unreachable
}
}
