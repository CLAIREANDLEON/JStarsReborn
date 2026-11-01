// jstars-prepare: command-line front end of the native preparation (the launcher uses the same code).
//   jstars-prepare <game-folder> <output-folder>
#include "reborn/prepare.hpp"
#include <cstdio>
#include <exception>
int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s <game-folder> <output-folder>\n", argv[0]); return 2; }
    try {
        auto r = reborn::prep::prepare(argv[1], argv[2], [](double f, const std::string& s) { std::fprintf(stderr, "[%3.0f%%] %s\n", f * 100, s.c_str()); });
        for (const auto& i : r.inputs) std::printf("input %s %llu %s\n", i.relative.c_str(), (unsigned long long)i.bytes, i.sha256.c_str());
        std::printf("boot-linked.bin sha256 %s; image %llu bytes, %zu imports, %zu bindings, %zu relocations, %zu segments\n", r.boot_sha256.c_str(),
                    (unsigned long long)r.image_bytes, r.imports, r.bindings, r.relocations, r.segments);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "prepare failed: %s\n", e.what());
        return 1;
    }
}
