# gpu/jsnet: online libraries (shadPS4 v0.20.0 network, NP and shadNet client)

GPL-2.0-or-later. Decision and design: `docs/ADR-008-online-shadnet.md`.

| Path | Origin | Changes |
|---|---|---|
| `vendor/shadps4/` | shadPS4 v0.20.0 (`529ceb32f5546b820588f194b903cc39d08015fe`): `src/core/libraries/{net,network,np}`, `src/core/net`, `src/shadnet`, as vendored by bbport (github.com/Supermedo/bloodborne_pc @ `b48052617a919fc4d6b10ae3395e209c9e94e17a`, `gpu/bbnet/vendor`) including bbport's fixes (`vendor-changes-bbport.diff`, marked `bbport` in the code) | `jstars:` the client reports itself as `JStarsReborn (shadPS4 v0.20.0 network code)` (env `JSTARS_CLIENT_VERSION`), `shadnet/client.cpp` |
| `include/` | bbport `gpu/bbnet/include` (GPL-2.0-or-later): the parts of shadPS4 these libraries need, reduced | settings read `JSTARS_*` environment variables instead of `BB_*`; default user name `Player`; `Ws2tcpip.h` case shim (ours) |
| `jsnet_glue.cpp` | bbport `gpu/bbnet/bbnet_glue.cpp` (GPL-2.0-or-later) | The Hunter's Dream backend (`thd/`, GPL-3.0, Bloodborne-only) removed; entry points `jsnet_configure/register/initialize/resolve`; own symbol table; `sceKernelUsleep`; inert `bbthd_*` hooks |
| `third_party/httplib` | cpp-httplib (MIT), via bbport | none |
| `third_party/wepoll` | wepoll (BSD-2-Clause), via bbport | none |

Build: `tools/build_jsnet.sh <build dir> <deps prefix> <boost dir>` (clang++ for x86_64-w64-windows-gnu with mingw-w64 GCC 13's
libstdc++ and `-femulated-tls`). Dependencies (not vendored, built from their official sources): protobuf 3.21.12 (`protoc` 3.21.12 on the
host), OpenSSL 3.0.15, miniupnpc 2.2.8, nlohmann_json 3.11.3, zlib (mingw-w64 distribution package). Link: `make -C runtime jstars-boot-gpu.exe
JSNET_LIB_WIN=... JSNET_DEPS=...`.
