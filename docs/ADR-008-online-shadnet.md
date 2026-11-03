# ADR-008: online play through shadPS4's network/NP stack and a shadNet server

Status: accepted (owner, 2026-11-03: build dependencies approved; "both, public by default" server choice). Supersedes the Matching2/Signaling
parts of ADR-007 when `--shadnet` is used; ADR-007's LAN directory stays as the fallback for `--online` without `--shadnet`.

## Context
The hand-written NP layer (runtime/jstars_platform.c) reached the game's Friendly Match host screen but implements a subset of Matching2 and no
Signaling connection. The Bloodborne PC port (bbport) runs online play on the same kind of runtime by importing shadPS4 v0.20.0's network, NP and
shadNet client unchanged behind a ~500-line glue, with a shadNet server for matchmaking and peer-to-peer game traffic.

## Decision
- `gpu/jsnet/` vendors that stack (provenance in gpu/jsnet/README.md) and is built as `libjstars_net.a` (clang, mingw libstdc++ ABI).
- `--shadnet` (implies `--online`): the runtime registers the stack's 1293 functions ahead of its own (runtime/jstars_rt.c: `rt_resolve`
  asks `rt_jsnet_resolve` first), after the GPU library starts and once the image is loaded (libSceNpScore reads the game's SDK version),
  and starts the session after the module initializers, just before the game's entry point (as bbport).
- Coverage: all 117 network/NP/HTTP/SSL imports of CUSA01593 v01.02 are provided (checked against `import-names.tsv`, 2026-11-03).
- Server: `--shadnet-server host[:port]` or `JSTARS_SHADNET_SERVER`; launcher default will be shadPS4's public server `srv.shadps4.net:31313`,
  with a field for a self-hosted one (shadNet server source: github.com/shadps4-emu/shadnet). Account: `JSTARS_SHADNET_NPID` /
  `JSTARS_SHADNET_PASSWORD` (from the launcher; never on the command line or in logs). The client identifies honestly as JStarsReborn;
  whether the public server admits non-shadPS4 clients is that server's policy (bbport reports that it refused theirs).
- Matches stay peer-to-peer (signaling with STUN/UPnP from the stack); the server only does accounts, rooms and signaling exchange.

## Verified so far
- Windows build links; `--selftest-jsnet` 9/0 under Wine (registration of Matching2/Signaling/Net/NetCtl/NpManager/WebApi).
- Not yet run with the game. Not yet tested against a server. No match played.
