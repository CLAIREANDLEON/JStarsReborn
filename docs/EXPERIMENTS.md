# Diagnosis-only experiments (`--experiment name[,name]`)

An experiment changes one platform answer to find out whether the game depends on it. Experiments are off by default, are
logged loudly at start (`EXPERIMENTS ENABLED: ...`) and are never evidence that a behavior is correct. An answer becomes a
default only after the evidence is understood and recorded in `docs/IMPORT_AUDIT.md`.

| Name | Effect | Status |
|---|---|---|
| `online-probe` | The local user reports **signed in** (`sceNpGetState` = 2, `sceNpGetNpId`/`GetOnlineId` return the local name), the network reports **IP obtained** (`sceNetCtlGetState` = 3), and every still-unimplemented import of a `libSceNp*`/`libSceNet*`/`libSceSsl`/`libSceHttp`/`libSceRudp` library logs `ONLINE PROBE` and returns `0x80550006` instead of stopping the run. | Mapping only. It exists to record, in one run, which online functions the game calls and in what order. It is **not** an implementation and is not evidence that anything online works. |
| `trophy-strict` | `sceNpTrophyRegisterContext` reports TITLE_CONF_NOT_INSTALLED (the old behavior) | Comparison only. Evidence: with this answer the game stays on a black screen after the controller check (runs 20261101-044207, 044604); with the default (empty trophy set) it continues. |

Retired: `trophy-register-ok` (became the default as the explicit "empty trophy set": the trophy pack is encrypted and is not
decrypted by this port, so there is no trophy data to register; no trophy can unlock and nothing is recorded).
