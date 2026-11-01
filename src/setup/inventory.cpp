#include "reborn/setup_inventory.hpp"
#include "reborn/pkg_intake.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace reborn::setup {
namespace {
constexpr std::uint64_t max_entries = 10000;
constexpr std::uint64_t max_file_bytes = 8ULL * 1024 * 1024 * 1024;
constexpr std::uint64_t max_total_bytes = 64ULL * 1024 * 1024 * 1024;
constexpr unsigned max_depth = 32;
constexpr DWORD block_bytes = 1024 * 1024;

class Cancelled final : public std::runtime_error {
public: Cancelled() : std::runtime_error("Inventory cancelled; no report was saved") {}
};
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw Cancelled(); }
[[noreturn]] void win_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
void nt_check(NTSTATUS status, const char* operation) {
    if (status < 0) {
        std::ostringstream out;
        out << operation << " (status 0x" << std::hex << static_cast<unsigned long>(status) << ')';
        throw std::runtime_error(out.str());
    }
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
};
struct FindHandle {
    HANDLE value;
    explicit FindHandle(HANDLE h) : value(h) {}
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) win_error("Cannot encode filename as UTF-8");
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr) != size)
        win_error("Cannot encode filename as UTF-8");
    return output;
}
std::string json(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string path_text(const std::filesystem::path& value) { return utf8(value.generic_wstring()); }
bool same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool below(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
    auto item = candidate.begin();
    for (auto part = parent.begin(); part != parent.end(); ++part, ++item)
        if (item == candidate.end() || !same(part->wstring(), item->wstring())) return false;
    return true;
}
std::filesystem::path absolute_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("A source folder and report path are required");
    const auto value = std::filesystem::absolute(path).lexically_normal();
    if (value.wstring().starts_with(L"\\\\?\\") || value.wstring().starts_with(L"\\\\.\\"))
        throw std::invalid_argument("Device namespace paths are not accepted");
    for (const auto& part : value.relative_path()) {
        const auto name = part.wstring();
        if (name.find(L':') != std::wstring::npos || (!name.empty() && (name.back() == L'.' || name.back() == L' ')))
            throw std::invalid_argument("Ambiguous names or alternate data streams are not accepted");
    }
    return value;
}
void reject_reparse_ancestors(const std::filesystem::path& value, bool allow_missing_leaf = false) {
    std::filesystem::path current;
    for (const auto& part : value) {
        current /= part;
        if (current == value.root_name()) continue;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (allow_missing_leaf && current == value && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return;
            SetLastError(error); win_error("Cannot inspect path");
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Symbolic links and reparse points are not accepted");
    }
}
std::filesystem::path opened_path(HANDLE handle) {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= buffer.size()) win_error("Cannot verify opened file path");
    std::wstring value(buffer.data(), count);
    if (value.starts_with(L"\\\\?\\UNC\\")) value = L"\\\\" + value.substr(8);
    else if (value.starts_with(L"\\\\?\\")) value = value.substr(4);
    return std::filesystem::path(value).lexically_normal();
}
void verify_path(HANDLE handle, const std::filesystem::path& expected) {
    if (!same(opened_path(handle).wstring(), expected.wstring()))
        throw std::runtime_error("Opened file path differs from requested path");
}
Handle directory_handle(const std::filesystem::path& value) {
    reject_reparse_ancestors(value);
    Handle handle(CreateFileW(value.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) win_error("Cannot open folder");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) win_error("Cannot inspect folder");
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::invalid_argument("Input must be a regular directory without reparse points");
    return handle;
}
BY_HANDLE_FILE_INFORMATION information(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION result{};
    if (!GetFileInformationByHandle(handle, &result)) win_error("Cannot inspect open file");
    return result;
}
std::uint64_t size_of(const BY_HANDLE_FILE_INFORMATION& value) {
    return (static_cast<std::uint64_t>(value.nFileSizeHigh) << 32) | value.nFileSizeLow;
}
bool identity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow && size_of(a) == size_of(b) &&
           a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
           a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}
struct Hasher {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    Hasher() {
        try {
            nt_check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "Open SHA-256 provider");
            DWORD length=0, received=0;
            nt_check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &received, 0), "Read SHA-256 object size");
            if (!length || length > 1024*1024) throw std::runtime_error("Invalid SHA-256 object size");
            object.resize(length);
            nt_check(BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0), "Create SHA-256 hash");
        } catch (...) { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); throw; }
    }
    ~Hasher() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
};
std::string hex_bytes(const unsigned char* bytes, std::size_t count) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(count*2);
    for (std::size_t i=0;i<count;++i) {out+=hex[bytes[i]>>4];out+=hex[bytes[i]&15];}
    return out;
}
std::string signature(const std::vector<unsigned char>& head) {
    auto starts = [&](const unsigned char* bytes, std::size_t length) { return head.size() >= length && std::memcmp(head.data(), bytes, length) == 0; };
    const unsigned char png[]{137,80,78,71,13,10,26,10};
    const unsigned char dds[]{'D','D','S',' '};
    const unsigned char elf[]{127,'E','L','F'};
    const unsigned char mz[]{'M','Z'};
    if (starts(png,sizeof(png))) return "png-signature";
    if (starts(dds,sizeof(dds))) return "dds-signature";
    if (head.size()>=12 && std::memcmp(head.data(),"RIFF",4)==0 && std::memcmp(head.data()+8,"WAVE",4)==0) return "riff-wave-signature";
    if (starts(elf,sizeof(elf))) return "elf-signature-unparsed";
    if (starts(mz,sizeof(mz))) return "mz-signature-unparsed";
    return "unknown";
}
struct Record {
    std::string relative_path;
    std::uint64_t size=0;
    std::string hash, head, hint;
};
Record hash_file(const std::filesystem::path& file, const std::filesystem::path& root,
                 std::uint64_t& bytes_read, std::stop_token stop,
                 const std::function<void(std::uint64_t,std::uint64_t)>& progress, std::uint64_t files,
                 std::uint64_t file_limit=max_file_bytes,std::uint64_t total_limit=max_total_bytes,std::size_t head_limit=64) {
    cancelled(stop);
    reject_reparse_ancestors(file);
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) win_error("Cannot open file for inventory");
    const auto before = information(handle.value);
    if ((before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || GetFileType(handle.value)!=FILE_TYPE_DISK)
        throw std::runtime_error("Skipped non-regular file or reparse point");
    verify_path(handle.value,file);
    const auto size=size_of(before);
    if (bytes_read>total_limit || size>file_limit || size>total_limit-bytes_read) throw std::runtime_error("File exceeds per-file or remaining total read budget");
    Hasher hash;
    std::vector<unsigned char> buffer(block_bytes), head;
    std::uint64_t total=0;
    while (total<size) {
        cancelled(stop);
        DWORD received=0;
        const DWORD wanted=static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(),size-total));
        if (!ReadFile(handle.value,buffer.data(),wanted,&received,nullptr)) win_error("Cannot read source file");
        if (!received) throw std::runtime_error("Source file changed or ended during inventory");
        total+=received; bytes_read+=received;
        const auto head_count=std::min<std::size_t>(head_limit-head.size(),received);
        head.insert(head.end(),buffer.begin(),buffer.begin()+head_count);
        nt_check(BCryptHashData(hash.hash,buffer.data(),received,0),"Hash source bytes");
        if(progress)progress(files,bytes_read);
    }
    cancelled(stop);
    if (!identity(before,information(handle.value))) throw std::runtime_error("Source file changed during inventory");
    verify_path(handle.value,file);
    reject_reparse_ancestors(file);
    // Reopen the path to detect a replaced name as well as mutations of our handle.
    Handle named(CreateFileW(file.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if (named.value==INVALID_HANDLE_VALUE)win_error("Cannot recheck source file");
    if (!identity(before,information(named.value)))throw std::runtime_error("Source path was replaced during inventory");
    verify_path(named.value,file);
    std::array<unsigned char,32> digest{};
    nt_check(BCryptFinishHash(hash.hash,digest.data(),static_cast<ULONG>(digest.size()),0),"Finish SHA-256 hash");
    return {path_text(file.lexically_relative(root)),size,hex_bytes(digest.data(),digest.size()),hex_bytes(head.data(),head.size()),signature(head)};
}
void write_exclusive_report(const std::filesystem::path& destination,const std::string& text,std::stop_token stop) {
    cancelled(stop);
    reject_reparse_ancestors(destination,true);
    static std::atomic<std::uint64_t> sequence{0};
    auto temporary=destination;
    temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(sequence.fetch_add(1));
    Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)win_error("Cannot create inventory temporary report");
    try {
        std::size_t written=0;
        while(written<text.size()) {
            cancelled(stop);
            DWORD count=0;
            const DWORD wanted=static_cast<DWORD>(std::min<std::size_t>(block_bytes,text.size()-written));
            if(!WriteFile(file.value,text.data()+written,wanted,&count,nullptr)||count!=wanted)win_error("Cannot write inventory report");
            written+=count;
        }
        if(!FlushFileBuffers(file.value))win_error("Cannot flush inventory report");
        if(!CloseHandle(file.value)) {file.value=INVALID_HANDLE_VALUE;win_error("Cannot close inventory report");}
        file.value=INVALID_HANDLE_VALUE;
        cancelled(stop);
        reject_reparse_ancestors(destination,true);
        // No REPLACE_EXISTING: another report is never overwritten.
        if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH))win_error("Cannot commit new inventory report");
    } catch(...) {
        if(file.value!=INVALID_HANDLE_VALUE) {CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;}
        DeleteFileW(temporary.c_str());throw;
    }
}
}

InventoryResult inventory_folder(const std::filesystem::path& source,const std::filesystem::path& report,
                                  std::stop_token stop,std::function<void(std::uint64_t,std::uint64_t)> progress) {
    cancelled(stop);
    const auto source_absolute=absolute_path(source);
    const auto report_absolute=absolute_path(report);
    auto source_guard=directory_handle(source_absolute);
    const auto input=opened_path(source_guard.value);
    reject_reparse_ancestors(input);
    if(report_absolute.filename().empty())throw std::invalid_argument("Report must name a new file");
    auto report_anchor=report_absolute.parent_path();
    std::vector<std::filesystem::path> missing_parents;
    while(GetFileAttributesW(report_anchor.c_str())==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)win_error("Cannot inspect report parent");
        if(report_anchor.empty()||report_anchor==report_anchor.root_path())throw std::invalid_argument("Report has no accessible existing parent");
        missing_parents.push_back(report_anchor.filename());report_anchor=report_anchor.parent_path();
    }
    auto report_anchor_guard=directory_handle(report_anchor);
    report_anchor=opened_path(report_anchor_guard.value);
    auto parent=report_anchor;
    for(auto part=missing_parents.rbegin();part!=missing_parents.rend();++part)parent/=*part;
    const auto destination=parent/report_absolute.filename();
    if(below(destination,input))throw std::invalid_argument("Report output must be outside the source folder");
    if(GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES)throw std::invalid_argument("Report already exists; choose a new filename");
    const DWORD destination_error=GetLastError();
    if(destination_error!=ERROR_FILE_NOT_FOUND&&destination_error!=ERROR_PATH_NOT_FOUND)win_error("Cannot inspect report destination");

    InventoryResult result;
    std::vector<Record> records;
    std::vector<std::pair<std::filesystem::path,unsigned>> pending{{input,0}};
    std::uint64_t entries_seen=0,bytes_read=0;
    bool entry_limit=false;
    auto diagnostic=[&](const std::filesystem::path& item,const std::string& message) {
        result.complete=false;
        result.diagnostics.push_back(path_text(item.lexically_relative(input))+": "+message);
    };
    while(!pending.empty()&&!entry_limit) {
        cancelled(stop);
        auto [folder,depth]=std::move(pending.back());pending.pop_back();
        try {
            auto folder_guard=directory_handle(folder);
            verify_path(folder_guard.value,folder);
            WIN32_FIND_DATAW found{};
            FindHandle search(FindFirstFileW((folder/L"*").c_str(),&found));
            if(search.value==INVALID_HANDLE_VALUE) {
                if(GetLastError()==ERROR_FILE_NOT_FOUND)continue;
                win_error("Cannot list folder");
            }
            std::vector<std::wstring> names;
            do {
                cancelled(stop);
                if(std::wcscmp(found.cFileName,L".")==0||std::wcscmp(found.cFileName,L"..")==0)continue;
                if(entries_seen>=max_entries) {diagnostic(folder,"Maximum 10000-entry inventory limit reached");entry_limit=true;break;}
                ++entries_seen;names.emplace_back(found.cFileName);
            }while(FindNextFileW(search.value,&found));
            if(!entry_limit&&GetLastError()!=ERROR_NO_MORE_FILES)win_error("Folder enumeration failed");
            std::sort(names.begin(),names.end());
            for(const auto& name:names) {
                cancelled(stop);
                const auto item=folder/name;
                try {
                    const DWORD attributes=GetFileAttributesW(item.c_str());
                    if(attributes==INVALID_FILE_ATTRIBUTES)win_error("Cannot inspect source entry");
                    if(attributes&FILE_ATTRIBUTE_REPARSE_POINT) {diagnostic(item,"Skipped symbolic link or reparse point");continue;}
                    if(attributes&FILE_ATTRIBUTE_DIRECTORY) {
                        if(depth>=max_depth)diagnostic(item,"Maximum directory depth reached");
                        else pending.emplace_back(item,depth+1);
                    }else {
                        auto record=hash_file(item,input,bytes_read,stop,progress,result.files);
                        result.bytes+=record.size;++result.files;records.push_back(std::move(record));
                        if(progress)progress(result.files,bytes_read);
                    }
                }catch(const Cancelled&){throw;}
                catch(const std::exception& error){diagnostic(item,error.what());}
            }
        }catch(const Cancelled&){throw;}
        catch(const std::exception& error){diagnostic(folder,error.what());}
    }
    cancelled(stop);
    std::sort(records.begin(),records.end(),[](const Record& a,const Record& b){return a.relative_path<b.relative_path;});
    std::sort(result.diagnostics.begin(),result.diagnostics.end());
    std::ostringstream out;
    out << "{\n  \"schema\":\"reborn.setup.inventory.v1\",\n  \"source_root\":"<<json(path_text(input))
        <<",\n  \"complete\":"<<(result.complete?"true":"false")
        <<",\n  \"game_import_supported\":false,\n  \"summary\":\"Read-only inventory. No verified game importer is installed. Signature hints are not asset validation.\",\n"
        <<"  \"limits\":{\"max_entries\":"<<max_entries<<",\"max_depth\":"<<max_depth<<",\"max_file_bytes\":"<<max_file_bytes<<",\"max_total_bytes\":"<<max_total_bytes<<"},\n"
        <<"  \"files_scanned\":"<<result.files<<",\n  \"bytes_hashed\":"<<result.bytes<<",\n  \"bytes_read\":"<<bytes_read<<",\n  \"entries_seen\":"<<entries_seen<<",\n  \"entries\":[";
    bool first=true;
    for(const auto& record:records) {
        if(!first)out<<',';
        first=false;
        out<<"\n    {\"path\":"<<json(record.relative_path)<<",\"size_bytes\":"<<record.size<<",\"sha256\":"<<json(record.hash)
           <<",\"first_bytes_hex\":"<<json(record.head)<<",\"signature_hint\":"<<json(record.hint)<<",\"validated_asset\":false}";
    }
    out<<"\n  ],\n  \"diagnostics\":[";first=true;
    for(const auto& error:result.diagnostics){if(!first)out<<',';first=false;out<<'\n'<<"    "<<json(error);}
    out<<"\n  ]\n}\n";
    // Pinned directory handles deny renaming source and report parent during commit.
    verify_path(source_guard.value,input);
    verify_path(report_anchor_guard.value,report_anchor);
    auto created_parent=report_anchor;
    for(auto part=missing_parents.rbegin();part!=missing_parents.rend();++part) {
        cancelled(stop);created_parent/=*part;
        if(!CreateDirectoryW(created_parent.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)win_error("Cannot create report directory");
        reject_reparse_ancestors(created_parent);
    }
    auto report_parent_guard=directory_handle(destination.parent_path());
    verify_path(report_parent_guard.value,destination.parent_path());
    write_exclusive_report(destination,out.str(),stop);
    return result;
}
PkgResult inspect_pkg(const std::filesystem::path& pkg,const std::filesystem::path& report,
                      std::stop_token stop,std::function<void(std::uint64_t,std::uint64_t)> progress) {
    cancelled(stop);
    const auto input=absolute_path(pkg), output=absolute_path(report);
    if(!same(input.extension().wstring(),L".pkg"))throw std::invalid_argument("Choose a .pkg package file");
    reject_reparse_ancestors(input);
    auto source_parent_guard=directory_handle(input.parent_path());
    verify_path(source_parent_guard.value,input.parent_path());
    if(output.filename().empty())throw std::invalid_argument("Report must name a new JSON file");
    auto anchor=output.parent_path();std::vector<std::filesystem::path> missing;
    while(GetFileAttributesW(anchor.c_str())==INVALID_FILE_ATTRIBUTES){auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)win_error("Cannot inspect report location");if(anchor.empty()||anchor==anchor.root_path())throw std::invalid_argument("Report has no accessible parent");missing.push_back(anchor.filename());anchor=anchor.parent_path();}
    auto anchor_guard=directory_handle(anchor);anchor=opened_path(anchor_guard.value);auto parent=anchor;
    for(auto it=missing.rbegin();it!=missing.rend();++it)parent/=*it;
    const auto destination=parent/output.filename();
    if(below(destination,input.parent_path()))throw std::invalid_argument("Save the package report outside the source package folder");
    if(GetFileAttributesW(destination.c_str())!=INVALID_FILE_ATTRIBUTES)throw std::invalid_argument("Report already exists; choose a new filename");
    constexpr std::uint64_t cap=128ULL*1024*1024*1024;
    // Only outer, publicly documented fields are inspected. No payload keys or extraction.
    // Public references: LibOrbisPkg/PKG/Pkg.cs and PkgReader.cs (links in PKG_SUPPORT.md).
    std::uint64_t read=0;auto record=hash_file(input,input.parent_path(),read,stop,progress,0,cap,cap,0x70);
    if(record.size<0x5a0)throw std::invalid_argument("Package is shorter than the documented 0x5A0 outer header");
    if(!record.head.starts_with("7f434e54"))throw std::invalid_argument("This file does not have the documented CNT package signature");
    PkgResult result;result.bytes=record.size;result.sha256=record.hash;
    auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};bool ascii=true,terminated=false;
    for(std::size_t i=0x40;i<0x70;++i){unsigned c=static_cast<unsigned>((nibble(record.head[i*2])<<4)|nibble(record.head[i*2+1]));if(c==0){terminated=true;continue;}if(terminated||c<32||c>126){ascii=false;break;}result.content_id.push_back(static_cast<char>(c));}
    if(!ascii)result.content_id.clear();
    cancelled(stop);std::ostringstream out;
    out<<"{\n  \"schema\":\"reborn.pkg-source.v1\",\n  \"source_path\":"<<json(path_text(input))
       <<",\n  \"size_bytes\":"<<result.bytes<<",\n  \"sha256\":"<<json(result.sha256)
       <<",\n  \"signature_hint\":\"CNT-package\",\n  \"content_id_hint\":"<<json(result.content_id)
       <<",\n  \"source_registered\":true,\n  \"container_validated\":false,\n  \"content_extracted\":false,\n  \"executable_converted\":false,\n  \"game_import_supported\":false,\n"
       <<"  \"summary\":\"Source metadata registered only. Header signature and minimum size do not prove container validity, platform identity, title identity, authenticity, or game compatibility. No content extraction or game conversion is implemented.\"\n}\n";
    verify_path(source_parent_guard.value,input.parent_path());verify_path(anchor_guard.value,anchor);
    auto created=anchor;for(auto it=missing.rbegin();it!=missing.rend();++it){cancelled(stop);created/=*it;if(!CreateDirectoryW(created.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)win_error("Cannot create report directory");reject_reparse_ancestors(created);}
    auto parent_guard=directory_handle(destination.parent_path());verify_path(parent_guard.value,destination.parent_path());
    write_exclusive_report(destination,out.str(),stop);if(progress)progress(1,result.bytes);return result;
}

}
