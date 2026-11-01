#include "reborn/pkg_intake.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <windows.h>
namespace fs = std::filesystem;
int main() {
  auto root = fs::temp_directory_path() /
              (L"reborn-pkg-tests-" + std::to_wstring(GetCurrentProcessId()) +
               L"-" + std::to_wstring(GetTickCount64()));
  int checks = 0;
  try {
    fs::create_directories(root / L"source");
    auto check = [&](bool v) {
      ++checks;
      if (!v)
        throw std::runtime_error("PKG check failed at " +
                                 std::to_string(checks));
    };
    auto reject = [&](auto fn) {
      bool failed = false;
      try {
        fn();
      } catch (const std::exception &) {
        failed = true;
      }
      check(failed);
    };
    auto write = [&](const fs::path &p, const std::vector<char> &data) {
      std::ofstream f(p, std::ios::binary);
      f.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!f)
        throw std::runtime_error("fixture write failed");
    };
    auto read = [&](const fs::path &p) {
      std::ifstream f(p, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(f), {});
    };
    std::vector<char> fixture(0x5a0, 0);
    fixture[0] = 127;
    fixture[1] = 'C';
    fixture[2] = 'N';
    fixture[3] = 'T';
    std::string id = "SYNTHETIC-HEADER-NOT-A-GAME";
    std::copy(id.begin(), id.end(), fixture.begin() + 0x40);
    auto source = root / L"source" / L"sample.pkg";
    write(source, fixture);
    auto result =
        reborn::setup::inspect_pkg(source, root / L"reports" / L"valid.json");
    check(result.bytes == fixture.size());
    check(result.sha256.size() == 64);
    check(result.content_id == id);
    check(!result.game_import_supported);
    auto json = read(root / L"reports" / L"valid.json");
    check(json.find("\"container_validated\":false") != std::string::npos);
    check(json.find("\"content_extracted\":false") != std::string::npos);
    check(json.find("\"executable_converted\":false") != std::string::npos);
    check(read(source) == std::string(fixture.begin(), fixture.end()));
    reject([&] {
      reborn::setup::inspect_pkg(source, root / L"reports" / L"valid.json");
    });
    check(read(root / L"reports" / L"valid.json") == json);
    reject([&] {
      reborn::setup::inspect_pkg(source,
                                 root / L"source" / L"nested" / L"bad.json");
    });
    check(!fs::exists(root / L"source" / L"nested"));
    auto bad = fixture;
    bad[0] = 0;
    write(root / L"source" / L"bad.pkg", bad);
    reject([&] {
      reborn::setup::inspect_pkg(root / L"source" / L"bad.pkg",
                                 root / L"bad.json");
    });
    check(!fs::exists(root / L"bad.json"));
    write(root / L"source" / L"short.pkg", {127, 'C', 'N', 'T'});
    reject([&] {
      reborn::setup::inspect_pkg(root / L"source" / L"short.pkg",
                                 root / L"short.json");
    });
    write(root / L"source" / L"wrong.bin", fixture);
    reject([&] {
      reborn::setup::inspect_pkg(root / L"source" / L"wrong.bin",
                                 root / L"wrong.json");
    });
    reject([&] {
      reborn::setup::inspect_pkg(root / L"source" / L"missing.pkg",
                                 root / L"missing.json");
    });
    std::stop_source stop;
    stop.request_stop();
    reject([&] {
      reborn::setup::inspect_pkg(source, root / L"cancelled.json",
                                 stop.get_token());
    });
    check(!fs::exists(root / L"cancelled.json"));
    std::stop_source during;
    reject([&] {
      reborn::setup::inspect_pkg(
          source, root / L"cancelled-during.json", during.get_token(),
          [&](uint64_t, uint64_t) { during.request_stop(); });
    });
    check(!fs::exists(root / L"cancelled-during.json"));
    bad = fixture;
    bad[0x40] = static_cast<char>(255);
    write(root / L"source" / L"binary.pkg", bad);
    auto binary = reborn::setup::inspect_pkg(root / L"source" / L"binary.pkg",
                                             root / L"binary.json");
    check(binary.content_id.empty());
    auto unicode = root / L"source" / L"\u65e5\u672c.pkg";
    write(unicode, fixture);
    auto ur = reborn::setup::inspect_pkg(unicode, root / L"\u5831\u544a.json");
    check(ur.sha256 == result.sha256);
    check(fs::exists(root / L"\u5831\u544a.json"));
    fs::remove_all(root);
    std::cout << "PASS " << checks
              << " PKG intake checks (synthetic outer headers; no real game "
                 "tested)\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return 1;
  }
}
