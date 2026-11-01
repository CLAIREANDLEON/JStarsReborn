#include "reborn/setup_inventory.hpp"
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
unsigned checks=0, skips=0;
void check(bool condition,const char* message) {++checks;if(!condition)throw std::runtime_error(message);}
template<class F> void rejects(F f,const char* message) {
    bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,message);
}
void put(const fs::path& file,const std::string& bytes) {
    std::ofstream out(file,std::ios::binary);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    if(!out)throw std::runtime_error("Fixture write failed");
}
std::string read(const fs::path& file) {
    std::ifstream in(file,std::ios::binary);if(!in)throw std::runtime_error("Fixture read failed");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
// libstdc++ returns temp_directory_path() with a trailing separator while libc++ does not;
// strip it so parent_path() comparisons behave identically with either standard library.
fs::path temp_base() {
    auto base=fs::absolute(fs::temp_directory_path()).lexically_normal();
    if(!base.has_filename())base=base.parent_path();
    return base;
}
struct Temporary {
    fs::path base=temp_base();
    fs::path root=base/(L"reborn-inventory-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    bool created=false;
    Temporary() {
        if(root.parent_path()!=base)throw std::runtime_error("Temporary fixture escaped its parent");
        created=fs::create_directory(root);if(!created)throw std::runtime_error("Temporary fixture already exists");
    }
    ~Temporary(){if(created&&root.parent_path()==base){std::error_code ignored;fs::remove_all(root,ignored);}}
};
void ordinary_and_unicode(const fs::path& root) {
    const auto source=root/L"source-\u65e5\u672c\u8a9e";fs::create_directory(source);
    const auto abc=source/L"\u00e9-\u65e5\u672c\u8a9e.dat";
    put(abc,"abc");put(source/L"image.bin",std::string("\x89PNG\r\n\x1a\n",8));put(source/L"empty","");
    const auto destination=root/L"new-reports"/L"nested"/L"report-\u00e9.json";
    std::uint64_t callback_files=0,callback_bytes=0;
    const auto result=reborn::setup::inventory_folder(source,destination,{},[&](std::uint64_t files,std::uint64_t bytes){callback_files=files;callback_bytes=bytes;});
    check(result.complete,"Simple inventory incomplete");check(result.files==3,"Wrong file count");check(result.bytes==11,"Wrong byte count");
    check(!result.game_import_supported,"Inventory must never imply game import");check(result.diagnostics.empty(),"Unexpected source diagnostics");
    check(callback_files==3&&callback_bytes==11,"Progress did not reach final totals");
    check(read(abc)=="abc","Source file was altered");
    const auto report=read(destination);
    check(report.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")!=std::string::npos,"SHA-256 of abc is wrong");
    check(report.find("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")!=std::string::npos,"SHA-256 of empty file is wrong");
    check(report.find("png-signature")!=std::string::npos,"PNG signature hint absent");
    check(report.find("\"game_import_supported\":false")!=std::string::npos,"Report lacks truthful import status");
    check(report.find("\"validated_asset\":false")!=std::string::npos,"Signature hint falsely validates assets");
    check(report.find("\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.dat")!=std::string::npos,"Unicode path was not encoded as UTF-8");
    rejects([&]{reborn::setup::inventory_folder(source,destination);},"Existing report was overwritten");
    check(read(destination)==report,"Rejected overwrite modified existing report");
    auto nested=source/L"never-created"/L"report.json";
    rejects([&]{reborn::setup::inventory_folder(source,nested);},"Report inside source was accepted");
    check(!fs::exists(nested.parent_path()),"Rejected overlap created a source folder");
    rejects([&]{reborn::setup::inventory_folder(abc,root/L"not-folder.json");},"File accepted as source folder");
    rejects([&]{reborn::setup::inventory_folder(root/L"missing-source",root/L"missing-report.json");},"Missing source accepted");
    rejects([&]{reborn::setup::inventory_folder(source,root/L"trailing-space " / L"report.json");},"Ambiguous Windows path accepted");
}
void cancellation(const fs::path& root) {
    auto source=root/L"cancel-source";fs::create_directory(source);put(source/L"large.dat",std::string(2*1024*1024,'x'));
    auto out=root/L"cancel-reports"/L"report.json";
    std::stop_source already;already.request_stop();
    rejects([&]{reborn::setup::inventory_folder(source,out,already.get_token());},"Pre-cancelled scan completed");
    check(!fs::exists(out.parent_path()),"Pre-cancelled scan created report directory");
    std::stop_source during;
    rejects([&]{reborn::setup::inventory_folder(source,out,during.get_token(),[&](std::uint64_t,std::uint64_t){during.request_stop();});},"Mid-file cancellation ignored");
    check(!fs::exists(out.parent_path()),"Cancelled scan left report or directory");
    check(fs::file_size(source/L"large.dat")==2*1024*1024,"Cancellation changed source");
}
void depth_limit(const fs::path& root) {
    auto source=root/L"depth-source";fs::create_directory(source);auto current=source;
    for(unsigned i=0;i<34;++i){current/=L"d";fs::create_directory(current);}
    put(current/L"not-reached","data");
    auto result=reborn::setup::inventory_folder(source,root/L"depth.json");
    check(!result.complete,"Excessive depth reported complete");check(result.files==0,"File beyond depth limit was read");
    check(read(root/L"depth.json").find("Maximum directory depth")!=std::string::npos,"Depth limit not diagnosed");
}
void entry_limit(const fs::path& root) {
    auto source=root/L"entries-source";fs::create_directory(source);
    for(unsigned i=0;i<10001;++i)fs::create_directory(source/std::to_wstring(i));
    auto result=reborn::setup::inventory_folder(source,root/L"entries.json");
    check(!result.complete,"Entry-limited inventory reported complete");
    check(read(root/L"entries.json").find("10000-entry")!=std::string::npos,"Entry limit not diagnosed");
}
void file_size_limit(const fs::path& root) {
    auto source=root/L"size-source";fs::create_directory(source);auto file=source/L"sparse.dat";
    HANDLE handle=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create sparse fixture");
    DWORD returned=0;
    if(!DeviceIoControl(handle,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)) {
        CloseHandle(handle);++skips;std::cout<<"SKIP sparse size-cap fixture unavailable\n";return;
    }
    LARGE_INTEGER end{};end.QuadPart=8LL*1024*1024*1024+1;
    if(!SetFilePointerEx(handle,end,nullptr,FILE_BEGIN)||!SetEndOfFile(handle)){CloseHandle(handle);throw std::runtime_error("Cannot extend sparse fixture");}
    CloseHandle(handle);
    auto result=reborn::setup::inventory_folder(source,root/L"size.json");
    check(!result.complete&&result.files==0&&result.bytes==0,"Oversize source was hashed");
    check(read(root/L"size.json").find("per-file or remaining total")!=std::string::npos,"File-size budget not diagnosed");
}
void symlink_rejection(const fs::path& root) {
    auto source=root/L"link-source";fs::create_directory(source);put(root/L"private-data","unread");
    if(!CreateSymbolicLinkW((source/L"link").c_str(),(root/L"private-data").c_str(),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        ++skips;std::cout<<"SKIP actual symlink fixture unavailable (Windows error "<<GetLastError()<<")\n";return;
    }
    // Some environments (e.g. Wine) report success without materializing a reparse point.
    // Asserting rejection against a link that does not exist would be a false failure and,
    // worse, would hide that the real rejection path was never exercised.
    const DWORD link_attributes=GetFileAttributesW((source/L"link").c_str());
    if(link_attributes==INVALID_FILE_ATTRIBUTES||!(link_attributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
        ++skips;std::cout<<"SKIP symlink creation reported success but no reparse point exists (attributes 0x"<<std::hex<<link_attributes<<std::dec<<")\n";return;
    }
    auto result=reborn::setup::inventory_folder(source,root/L"links.json");
    check(!result.complete&&result.files==0&&result.bytes==0,"Symlink content was read");
    check(read(root/L"links.json").find("reparse point")!=std::string::npos,"Symlink not diagnosed");
}
}
int main() {
    try {
        Temporary temp;
        ordinary_and_unicode(temp.root);cancellation(temp.root);depth_limit(temp.root);
        entry_limit(temp.root);file_size_limit(temp.root);symlink_rejection(temp.root);
        std::cout<<"PASS: "<<checks<<" native inventory checks, "<<skips<<" optional environment skips\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
