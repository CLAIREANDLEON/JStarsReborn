# ADR-007: how online play is implemented

Status: **accepted for the first milestone** (a private two-machine Friendly Match), 2026-11-01. Supersedes nothing.

## What the game actually needs

Established from the owner's own executable and from recorded Windows runs, not from the names of the APIs
(method and addresses in [reverse-engineering/FUNCTION_MAP.md](reverse-engineering/FUNCTION_MAP.md)):

1. **Its own transport.** `eboot.bin+0x2cc710` creates one socket, sets two options, binds it and starts a receive
   thread looping on `sceNetRecvfrom` into a handler at `+0x2b2410`. The socket is created with family 2,
   **type 6**, protocol 0 at `+0x2cc760`: a P2P datagram socket with virtual ports. The game carries its own
   battle traffic. Nothing in this project needs to understand that traffic.
2. **A way to find other players and learn their addresses.** That is what `libSceNpMatching2` (rooms) and
   `libSceNpSignaling` (per-peer connections) are for.
3. **Permission to go online at all.** `libSceNpManager`: a signed-in state, and an async request that answers
   "can this user use the network?". Recorded: the game reaches Online Battle, calls `sceNpGetOnlineId`, then
   retries `sceNpCreateAsyncRequest` every eight seconds until it succeeds.

So the online work is: sign-in, then rooms, then addresses. The battle itself already has a transport.

## Decision

**Implement the console's online libraries natively in the runtime, over a backend behind a narrow interface.**

```
   game code (original)
        |  libSceNpManager / libSceNpMatching2 / libSceNpSignaling
        v
   runtime/jstars_np*.c          <- the console's API, implemented here
        |  room directory interface (join, list, members, peer address)
        v
   backend:  [ local network ]  first        [ self-hosted server ]  later
```

### Why not vendor shadPS4's NP stack

shadPS4 is the reference this project already vendors for graphics, and its NP sources were read while writing
this (revision `55a97e46`, GPL-2.0-or-later, the same licence as this runtime, so reuse is permitted). They were
**not** adopted wholesale, for reasons of integration rather than licence:

* `np_matching2/`, `np_signaling/` and `np_handler/` are about 12,500 lines of C++ bound to shadPS4's own core:
  its configuration, user manager, logging, memory, event queues and object manager. This runtime is C with a
  different ownership model; taking those files means taking that core too.
* Their server side, shadNet, speaks a protobuf protocol to a Qt server. That is a dependency chain this runtime
  does not have, for a feature whose first milestone is two machines on one desk.
* The reference revisions in the handoff are not a proven interoperable pair for this game; nothing in them was
  written or tested against J-Stars.

What **is** taken from them is knowledge, which is the valuable part: structure layouts, error codes, the
request/callback shape, and the fact that virtual ports must be multiplexed. Each is attributed where used.

### Why the backend is an interface

The first backend finds players on the local network, so a Friendly Match needs no server, no account and no
port forwarding. A later backend can be a self-hosted server (shadNet's protocol is a candidate) for play over
the internet. The library layer above does not change when that happens, and it is the library layer that is
difficult, because it must match what the game expects exactly.

## Consequences

* Traffic is not interoperable with a PlayStation 4, and this is stated wherever it matters. The virtual-port
  framing is this project's own (`runtime/jstars_net.c`), because the console's is not reproduced here.
* No account, password or title secret is ever handled, stored or sent. There is no sign-up and no identity
  beyond the local player name.
* Nothing contacts PlayStation Network. The runtime says so in its log when the network layer starts.
* Offline behaviour is unchanged: without `--online` every answer is the one this runtime has always given.
* A room that two players share is a trust boundary. Before anything is exposed beyond a local network, input
  from a peer must be treated as untrusted, which is a separate piece of work recorded in the checklist.
