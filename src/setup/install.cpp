#include "reborn/setup_install.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace reborn::setup {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t maximum_total = 100ULL * 1024 * 1024;
constexpr size_t maximum_files = 256;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
[[noreturn]] void windows_error(const char* what) {
    throw std::runtime_error(std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
void cancelled(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Installation cancelled; no destination was committed");
}
bool equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool below(const fs::path& child, const fs::path& parent) {
    auto a=child.begin(), b=parent.begin();
    for (;b!=parent.end();++a,++b) if(a==child.end() || !equal(a->native(),b->native())) return false;
    return true;
}
fs::path normalized_absolute(const fs::path& value) {
    if(value.empty())throw std::invalid_argument("Empty path");
    auto result=fs::absolute(value).lexically_normal();
    if(result.filename().empty()) result=result.parent_path();
    return result.make_preferred();
}
void check_ancestors(const fs::path& value, bool must_exist) {
    fs::path current;
    for(const auto& component:value) {
        current/=component;
        if(current==value.root_name())continue;
        const DWORD attrs=GetFileAttributesW(current.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES) {
            const DWORD error=GetLastError();
            if(!must_exist && (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND))continue;
            windows_error("Cannot inspect path");
        }
        if(attrs&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Links and reparse points are not accepted");
    }
}
std::wstring final_path(HANDLE file) {
    std::vector<wchar_t> buffer(32768);
    const DWORD count=GetFinalPathNameByHandleW(file,buffer.data(),static_cast<DWORD>(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count||count>=buffer.size())windows_error("Cannot verify opened path");
    std::wstring result(buffer.data(),count);
    if(result.rfind(L"\\\\?\\UNC\\",0)==0)result=L"\\\\"+result.substr(8);
    else if(result.rfind(L"\\\\?\\",0)==0)result=result.substr(4);
    return result;
}
BY_HANDLE_FILE_INFORMATION information(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info))windows_error("Cannot inspect opened file");
    return info;
}
uint64_t size_of(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
}
bool same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh &&
        a.nFileIndexLow==b.nFileIndexLow && a.nFileSizeHigh==b.nFileSizeHigh && a.nFileSizeLow==b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime && a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
void verify_open(HANDLE file,const fs::path& expected,bool directory=false) {
    auto info=information(file);
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
       static_cast<bool>(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory ||
       !equal(final_path(file),fs::path(expected).make_preferred().native()))throw std::runtime_error("Opened path differs from inspected path or is an unexpected filesystem object");
}
std::wstring wide(const std::string& input) {
    if(input.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if(!count)throw std::runtime_error("Manifest path is not valid UTF-8");
    std::wstring result(static_cast<size_t>(count),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),result.data(),count)!=count)throw std::runtime_error("Cannot decode manifest path");
    return result;
}
std::string escape(const std::string& input) {
    std::string result;constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:input) {
        if(c=='"'||c=='\\'){result+='\\';result+=static_cast<char>(c);}
        else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=static_cast<char>(c);
    }
    return result;
}
void validate_relative(const std::wstring& name) {
    if(name.empty()||name.size()>1024||name.front()==L'/'||name.back()==L'/')throw std::runtime_error("Invalid relative manifest path");
    size_t start=0;
    while(start<name.size()) {
        const size_t end=name.find(L'/',start);
        const auto part=name.substr(start,end==std::wstring::npos?name.size()-start:end-start);
        if(part.empty()||part==L"."||part==L".."||part.front()==L' '||part.front()==L'.'||part.back()==L' '||part.back()==L'.')throw std::runtime_error("Ambiguous or traversing manifest path");
        for(wchar_t c:part)if(c<32||std::wstring(L"<>:\\|?*\"").find(c)!=std::wstring::npos)throw std::runtime_error("Invalid character in manifest path");
        auto base=part.substr(0,part.find(L'.'));
        if(!base.empty()&&base.back()==L' ')throw std::runtime_error("Ambiguous whitespace before a filename extension");
        if(equal(base,L"CON")||equal(base,L"PRN")||equal(base,L"AUX")||equal(base,L"NUL")||equal(base,L"CLOCK$"))throw std::runtime_error("Reserved Windows filename in manifest");
        if(base.size()==4 && (equal(base.substr(0,3),L"COM")||equal(base.substr(0,3),L"LPT")) &&
           ((base[3]>=L'1'&&base[3]<=L'9')||base[3]==0x00b9||base[3]==0x00b2||base[3]==0x00b3))throw std::runtime_error("Reserved Windows device filename in manifest");
        if(end==std::wstring::npos)break;
        start=end+1;
    }
    if(equal(name,L"install-receipt.json"))throw std::runtime_error("Manifest cannot replace the installer receipt");
}
struct Entry {std::string hash;uint64_t size;std::string text;fs::path relative;};
std::vector<Entry> manifest(const fs::path& source) {
    const fs::path file=source/L"setup-payload.txt";check_ancestors(file,true);
    Handle input(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(input.value==INVALID_HANDLE_VALUE)windows_error("Cannot open payload manifest");
    verify_open(input.value,file);
    auto before=information(input.value);
    const auto length=size_of(before);if(!length||length>65536)throw std::runtime_error("Payload manifest size must be 1..65536 bytes");
    std::string contents(static_cast<size_t>(length),'\0');DWORD read=0;
    if(!ReadFile(input.value,contents.data(),static_cast<DWORD>(length),&read,nullptr)||read!=length)windows_error("Cannot read payload manifest");
    if(!same(before,information(input.value)))throw std::runtime_error("Manifest changed during inspection");
    std::istringstream lines(contents);std::string line;std::vector<Entry> entries;uint64_t total=0;
    while(std::getline(lines,line)) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty())throw std::runtime_error("Empty payload manifest record");
        const auto first=line.find('\t'),second=first==std::string::npos?std::string::npos:line.find('\t',first+1);
        if(first!=64||second==std::string::npos||line.find('\t',second+1)!=std::string::npos)throw std::runtime_error("Malformed payload manifest record");
        Entry e{};e.hash=line.substr(0,first);e.text=line.substr(second+1);
        if(!std::all_of(e.hash.begin(),e.hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw std::runtime_error("Manifest hashes must be lowercase SHA-256");
        auto size_text=line.substr(first+1,second-first-1);
        if(size_text.empty() || !std::all_of(size_text.begin(),size_text.end(),[](char c){return c>='0'&&c<='9';}))throw std::runtime_error("Invalid payload size");
        const auto parsed=std::from_chars(size_text.data(),size_text.data()+size_text.size(),e.size);
        if(parsed.ec!=std::errc{}||parsed.ptr!=size_text.data()+size_text.size()||e.size>maximum_total-total)throw std::runtime_error("Payload exceeds 100 MiB budget");
        auto decoded=wide(e.text);validate_relative(decoded);e.relative=fs::path(decoded).make_preferred();
        for(const auto& previous:entries)if(below(e.relative,previous.relative)||below(previous.relative,e.relative))throw std::runtime_error("Duplicate, overlapping or case-equivalent manifest paths");
        total+=e.size;entries.push_back(std::move(e));if(entries.size()>maximum_files)throw std::runtime_error("Payload exceeds 256-file budget");
    }
    for(const auto* required:{L"JStarsReborn.exe",L"config/settings.ini",L"README.md",L"THIRD_PARTY_NOTICES.md"})
        if(std::none_of(entries.begin(),entries.end(),[&](const Entry& e){return equal(e.relative.generic_wstring(),required);}))throw std::runtime_error("Payload is missing a required runtime file");
    return entries;
}
struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    Sha256(){if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot initialize SHA-256");if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0){BCryptCloseAlgorithmProvider(algorithm,0);algorithm=nullptr;throw std::runtime_error("Cannot create SHA-256 state");}}
    ~Sha256(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    void add(unsigned char* data,ULONG count){if(BCryptHashData(hash,data,count,0)<0)throw std::runtime_error("Cannot hash payload data");}
    std::string finish(){std::array<unsigned char,32> digest{};if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)throw std::runtime_error("Cannot finish SHA-256");constexpr char hex[]="0123456789abcdef";std::string output;for(auto b:digest){output+=hex[b>>4];output+=hex[b&15];}return output;}
};
void copy_verified(const fs::path& source,const fs::path& target,const Entry& entry,std::stop_token stop) {
    check_ancestors(source,true);
    Handle input(CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(input.value==INVALID_HANDLE_VALUE)windows_error("Cannot open a runtime payload file");
    verify_open(input.value,source);const auto before=information(input.value);
    if(size_of(before)!=entry.size)throw std::runtime_error("Payload size differs from manifest");
    fs::create_directories(target.parent_path());check_ancestors(target.parent_path(),true);
    Handle output(CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(output.value==INVALID_HANDLE_VALUE)windows_error("Cannot create staged payload file");
    verify_open(output.value,target);
    Sha256 hash;std::vector<unsigned char> buffer(1024*1024);uint64_t copied=0;
    while(copied<entry.size) {
        cancelled(stop);DWORD count=0;
        const DWORD requested=static_cast<DWORD>(std::min<uint64_t>(buffer.size(),entry.size-copied));
        if(!ReadFile(input.value,buffer.data(),requested,&count,nullptr)||!count)windows_error("Cannot read complete payload file");
        hash.add(buffer.data(),count);DWORD written=0;
        if(!WriteFile(output.value,buffer.data(),count,&written,nullptr)||written!=count)windows_error("Cannot write complete payload file");
        copied+=count;
    }
    if(!same(before,information(input.value)))throw std::runtime_error("Payload changed during copying");
    verify_open(input.value,source);
    if(hash.finish()!=entry.hash)throw std::runtime_error("Payload SHA-256 does not match manifest");
    if(!FlushFileBuffers(output.value))windows_error("Cannot flush staged payload");
}
void remove_tree_without_following(const fs::path& folder) noexcept {
    WIN32_FIND_DATAW info{};HANDLE search=FindFirstFileW((folder/L"*").c_str(),&info);
    if(search!=INVALID_HANDLE_VALUE) {
        do {
            if(std::wstring(info.cFileName)==L"."||std::wstring(info.cFileName)==L"..")continue;
            auto item=folder/info.cFileName;
            if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
                if(!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))remove_tree_without_following(item);
                else RemoveDirectoryW(item.c_str());
            } else DeleteFileW(item.c_str());
        }while(FindNextFileW(search,&info));
        FindClose(search);
    }
    RemoveDirectoryW(folder.c_str());
}
void rollback(const fs::path& stage,const fs::path& parent,const BY_HANDLE_FILE_INFORMATION& owned) noexcept {
    try {
        if(stage.parent_path()!=parent||!below(stage,parent))return;
        check_ancestors(stage,true);
        Handle check(CreateFileW(stage.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(check.value==INVALID_HANDLE_VALUE)return;
        verify_open(check.value,stage,true);const auto now=information(check.value);
        if(now.dwVolumeSerialNumber!=owned.dwVolumeSerialNumber||now.nFileIndexHigh!=owned.nFileIndexHigh||now.nFileIndexLow!=owned.nFileIndexLow)return;
        remove_tree_without_following(stage);
    }catch(...){}
}
void receipt(const fs::path& stage,const std::vector<Entry>& entries) {
    std::ostringstream json;json<<"{\n  \"schema\":\"reborn.install-receipt.v1\",\n  \"foundation_only\":true,\n  \"game_data_imported\":false,\n  \"payload_authenticity\":\"Local SHA-256 integrity only; no digital signature\",\n  \"files\":[\n";
    for(size_t i=0;i<entries.size();++i){const auto& e=entries[i];if(i)json<<",\n";json<<"    {\"path\":\""<<escape(e.text)<<"\",\"size_bytes\":"<<e.size<<",\"sha256\":\""<<e.hash<<"\"}";}
    json<<"\n  ]\n}\n";const auto text=json.str();
    Handle out(CreateFileW((stage/L"install-receipt.json").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(out.value==INVALID_HANDLE_VALUE)windows_error("Cannot create installation receipt");
    DWORD written=0;
    if(!WriteFile(out.value,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)||written!=text.size()||!FlushFileBuffers(out.value))windows_error("Cannot write installation receipt");
}
}
InstallResult install_foundation(const fs::path& package_root,const fs::path& destination,std::stop_token stop,std::function<void(uint64_t,uint64_t)> progress) {
    cancelled(stop);const auto source=normalized_absolute(package_root),target=normalized_absolute(destination),parent=target.parent_path();
    if(below(source,target)||below(target,source))throw std::runtime_error("Installation and package directories must be separate, non-overlapping trees");
    if(target==target.root_path()||parent.empty())throw std::runtime_error("Installation needs a named destination directory");
    validate_relative(target.filename().native());
    check_ancestors(source,true);check_ancestors(parent,true);check_ancestors(target,false);
    if(!fs::is_directory(source)||!fs::is_directory(parent))throw std::runtime_error("Package and destination parent must be existing directories");
    const auto attrs=GetFileAttributesW(target.c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Installation destination already exists; choose a new folder");
    if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)windows_error("Cannot inspect installation destination");
    Handle parent_lock(CreateFileW(parent.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(parent_lock.value==INVALID_HANDLE_VALUE)windows_error("Cannot protect destination parent");
    verify_open(parent_lock.value,parent,true);
    const auto entries=manifest(source);cancelled(stop);
    static std::atomic<uint64_t> sequence{0};fs::path stage;bool created=false;
    for(unsigned attempt=0;attempt<16&&!created;++attempt) {
        stage=parent/(L".reborn-install-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(sequence.fetch_add(1)));
        created=CreateDirectoryW(stage.c_str(),nullptr)!=FALSE;
        if(!created&&GetLastError()!=ERROR_ALREADY_EXISTS)windows_error("Cannot create installation staging directory");
    }
    if(!created)throw std::runtime_error("Cannot allocate a unique installation staging directory");
    BY_HANDLE_FILE_INFORMATION owned{};
    try {
        {Handle owned_handle(CreateFileW(stage.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));if(owned_handle.value==INVALID_HANDLE_VALUE)windows_error("Cannot inspect installation staging directory");verify_open(owned_handle.value,stage,true);owned=information(owned_handle.value);}
        InstallResult result;result.executable=target/L"JStarsReborn.exe";
        for(const auto& entry:entries) {
            cancelled(stop);copy_verified(source/entry.relative,stage/entry.relative,entry,stop);++result.files;result.bytes+=entry.size;if(progress)progress(result.files,result.bytes);
        }
        receipt(stage,entries);cancelled(stop);check_ancestors(parent,true);verify_open(parent_lock.value,parent,true);
        if(GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Destination appeared during installation; nothing was replaced");
        if(!MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))windows_error("Cannot commit installation to the new directory");
        return result;
    }catch(...) {rollback(stage,parent,owned);throw;}
}
}
