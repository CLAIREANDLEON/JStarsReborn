#include "reborn/setup_install.hpp"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
namespace {
int checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void write(const fs::path& p,const std::string& data){fs::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<data;if(!f)throw std::runtime_error("Fixture write failed");}
// libstdc++ returns temp_directory_path() with a trailing separator while libc++ does not;
// strip it so parent_path() comparisons behave identically with either standard library.
fs::path temp_base(){auto b=fs::absolute(fs::temp_directory_path()).lexically_normal();if(!b.has_filename())b=b.parent_path();return b;}
std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
constexpr const char* abc="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
std::string line(const std::string& name){return std::string(abc)+"\t3\t"+name+"\n";}
std::string base_manifest(){return line("JStarsReborn.exe")+line("config/settings.ini")+line("README.md")+line("THIRD_PARTY_NOTICES.md");}
void fixture(const fs::path& root){fs::create_directories(root);for(auto p:{"JStarsReborn.exe","config/settings.ini","README.md","THIRD_PARTY_NOTICES.md"})write(root/p,"abc");write(root/"setup-payload.txt",base_manifest());}
template<class F>void rejects(F fn,const char* message){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}expect(rejected,message);}
void clean_stage(const fs::path& parent){for(const auto& e:fs::directory_iterator(parent))expect(e.path().filename().native().rfind(L".reborn-install-",0)!=0,"Owned staging directory leaked");}
}
int main(){
    fs::path area;
    try {
        area=fs::temp_directory_path()/(L"reborn-install-tests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        expect(fs::create_directory(area),"Fresh test directory required");const auto package=area/L"package";fixture(package);
        const auto dest=area/L"Installed runtime";std::vector<uint64_t> callbacks;
        auto r=reborn::setup::install_foundation(package,dest,{},[&](uint64_t files,uint64_t bytes){expect(bytes==files*3,"Progress byte count");callbacks.push_back(files);});
        expect(r.files==4&&r.bytes==12,"Exact installation totals");expect(r.executable==dest/L"JStarsReborn.exe","Executable result");expect(callbacks==std::vector<uint64_t>({1,2,3,4}),"Progress order");
        for(auto p:{"JStarsReborn.exe","config/settings.ini","README.md","THIRD_PARTY_NOTICES.md"})expect(read(dest/p)=="abc","Exact installed bytes");
        expect(read(dest/"install-receipt.json").find("\"foundation_only\":true")!=std::string::npos,"Foundation receipt");
        rejects([&]{reborn::setup::install_foundation(package,dest);},"Existing destination rejected");expect(read(dest/"JStarsReborn.exe")=="abc","Existing content preserved");
        rejects([&]{reborn::setup::install_foundation(package,package/L"child");},"Child destination rejected");
        rejects([&]{reborn::setup::install_foundation(package,area);},"Ancestor destination rejected");
        rejects([&]{reborn::setup::install_foundation(package,package);},"Same destination rejected");
        rejects([&]{reborn::setup::install_foundation(package,area/L"missing"/L"target");},"Missing parent rejected");
        std::stop_source already;already.request_stop();rejects([&]{reborn::setup::install_foundation(package,area/L"cancel-early",already.get_token());},"Early cancellation rejected");
        expect(!fs::exists(area/L"cancel-early"),"Early cancellation not installed");
        std::stop_source later;rejects([&]{reborn::setup::install_foundation(package,area/L"cancel-later",later.get_token(),[&](uint64_t,uint64_t){later.request_stop();});},"Cancellation after copy rejected");expect(!fs::exists(area/L"cancel-later"),"Cancelled install uncommitted");clean_stage(area);
        write(package/L"README.md","bad");rejects([&]{reborn::setup::install_foundation(package,area/L"hash-mismatch");},"Wrong SHA rejected");expect(!fs::exists(area/L"hash-mismatch"),"Corrupt install not committed");clean_stage(area);write(package/L"README.md","abc");
        fs::remove(package/L"README.md");rejects([&]{reborn::setup::install_foundation(package,area/L"missing-file");},"Missing payload rejected");clean_stage(area);write(package/L"README.md","abc");
        write(package/L"README.md","abcd");rejects([&]{reborn::setup::install_foundation(package,area/L"wrong-size");},"Wrong size rejected");clean_stage(area);write(package/L"README.md","abc");
        const std::vector<std::string> invalid={"../outside","a/../outside","/absolute","C:/absolute","a\\b","a//b","a/./b","a/","a/file.","a/ file","a/file ","a/.hidden","CON","con.txt","CON .txt","COM1.foo","LPT9","NUL.txt","a:b","a?b","a\"b","install-receipt.json"};
        unsigned index=0;for(const auto& name:invalid){write(package/L"setup-payload.txt",base_manifest()+line(name));rejects([&]{reborn::setup::install_foundation(package,area/(L"invalid-"+std::to_wstring(index++)));},"Invalid path rejected");clean_stage(area);}
        write(package/L"setup-payload.txt",base_manifest()+line("readme.MD"));rejects([&]{reborn::setup::install_foundation(package,area/L"duplicate");},"Case equivalent rejected");
        write(package/L"setup-payload.txt",base_manifest()+line("README.md/sub"));rejects([&]{reborn::setup::install_foundation(package,area/L"overlap");},"File subtree overlap rejected");
        write(package/L"setup-payload.txt",line("JStarsReborn.exe"));rejects([&]{reborn::setup::install_foundation(package,area/L"incomplete");},"Required file missing rejected");
        write(package/L"setup-payload.txt",base_manifest()+std::string(abc)+"\t104857601\tlarge\n");rejects([&]{reborn::setup::install_foundation(package,area/L"oversize");},"Total cap rejected");
        write(package/L"setup-payload.txt",base_manifest()+std::string(abc)+"\t-3\tnegative\n");rejects([&]{reborn::setup::install_foundation(package,area/L"negative");},"Negative size rejected");
        auto too_many=base_manifest();for(int i=0;i<253;++i)too_many+=line("extra"+std::to_string(i));write(package/L"setup-payload.txt",too_many);rejects([&]{reborn::setup::install_foundation(package,area/L"too-many");},"File count limit rejected");
        const std::string unicode="docs/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt";write(package/fs::path(L"docs/日本語.txt"),"abc");write(package/L"setup-payload.txt",base_manifest()+line(unicode));
        auto u=reborn::setup::install_foundation(package,area/L"Unicode 日本語");expect(u.files==5&&read(area/L"Unicode 日本語"/L"docs/日本語.txt")=="abc","Unicode installation");expect(read(area/L"Unicode 日本語"/L"install-receipt.json").find(unicode)!=std::string::npos,"UTF8 receipt preserves path");
        write(package/L"setup-payload.txt",base_manifest());rejects([&]{reborn::setup::install_foundation(package,area/L"callback-throws",{},[](uint64_t,uint64_t){throw std::runtime_error("test callback");});},"Callback failure propagated");clean_stage(area);
        const auto concurrent=area/L"appeared-destination";
        rejects([&]{reborn::setup::install_foundation(package,concurrent,{},[&](uint64_t files,uint64_t){if(files==4)write(concurrent/L"sentinel.txt","keep");});},"Concurrent destination rejected");
        expect(read(concurrent/L"sentinel.txt")=="keep","Concurrent destination was preserved");clean_stage(area);
        const auto linked=package/L"linked-file";
        if(CreateSymbolicLinkW(linked.c_str(),(package/L"README.md").c_str(),SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            write(package/L"setup-payload.txt",base_manifest()+line("linked-file"));
            rejects([&]{reborn::setup::install_foundation(package,area/L"link-copy");},"Source file link rejected");
            DeleteFileW(linked.c_str());clean_stage(area);
        }else std::cout<<"SKIP: real symlink fixture unavailable (Windows error "<<GetLastError()<<")\n";
        write(package/L"setup-payload.txt",base_manifest());
        expect(read(package/L"JStarsReborn.exe")=="abc","Original payload unchanged");
        // Resolve and constrain the exact temporary tree before recursive removal.
        expect(area.parent_path()==temp_base(),"Cleanup stays in temp parent");fs::remove_all(area);
        std::cout<<"PASS: "<<checks<<" setup installer checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<" (fixtures retained at "<<area.string()<<")\n";return 1;}
}
