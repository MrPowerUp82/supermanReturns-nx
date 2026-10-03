# Native renderer (milestone 1)

## Gameplay implementation in progress — 2026-10-02

The PC runtime corpus (223 shaders) has been translated, compiled and validated
as SPIR-V locally. New checked guest access, PM4 state snapshots and opt-in
`sr_native_capture` hooks are being integrated on branch
`codex/superman-native-gameplay`. This is capture infrastructure, not completed
native gameplay. Host tests and a linked NRO do not establish console acceptance.
The user confirmed the remote logs are old Xenos runs; no native test run has
occurred. Keep new build directories/logs separate from those historical logs.

The hooks preserve original PPC calls and capture arguments before register
clobber, with state after dirty-state flush. UP nests BeginVertices and updates
the segment cursor itself; independent Begin/End calls keep their data per thread.
Segment-tail capture spans allocation changes and retains distinct epochs.
Clear float4 and depth are copied as floats. Resolve stencil comes from entry
SP+92, proven by the original's 368-byte stack frame and new-SP+460 load.
Shader creators 820F5148/820F4D90 take a container in r3 and return the object in
r3; associations compare exact container bytes and stage.

Static call-graph audit found fence waits through RingMakeSpace/allocators in
DrawVertices, indexed draws, UP, clear, resolve and swap. The BlockOnFence
observation hook synchronizes the mirror before invoking the original wait.
Immutable observation fragments are emitted at the segment switch and fence
entry before the original proceeds. Drained stamps are removed from the outer
scope so its final capture cannot replay them. These fragments do not execute
native clears/draws; execution must preserve partial-operation semantics and
verify correlation on a fresh console trace. Capture mode never bypasses a guest wait or completes
GPU work. Log output is bounded to the first 512 completed operations and 32
rejections/fence probes; stamps report host-order words and physical sites.

Cross builds use the real Mesa/NVK SDK. The existing Docker build volume keeps
its generated game sources and dependencies, with tracked sources overlaid;
the new NRO is copied under a distinct capture filename. Existing SD settings,
historical logs, shader packs and NROs must be preserved when deploying.
TOML path overrides are finalized after config loading, allowing an isolated
capture folder to reference the original game data without copying or changing it.

Status: Tasks 1-7 of `docs/superpowers/plans/2026-10-01-renderer-nativo-marco1.md` are implemented
and host-tested, but the milestone is **not accepted**: no console round has run (see `checkpoint8.md`). Milestone 1 only consumes the PM4 stream and presents an opaque black image; game draws
are counted as omitted.

Adapted from nfsmw-nx (`nfsmw_nativo_sistema`, revision `df2de32ee569873062b8f8d1da8ad0b8d0a90a5d`).
NFSMW hooks, the shader library and scene paths were not imported.

## Selecting the renderer

`sr_renderer = "xenos" | "native"` in `superman_returns.toml` (default `xenos`, requires restart).
The choice holds for the whole run: no runtime switching and no per-draw fallback to Xenos.
Any other value is an error: native setup fails and the run ends instead of silently using Xenos.
`pack_shaders` keeps controlling only the existing hybrid Xenos route. Each run logs
`[sr-native] sr_renderer=<mode> milestone=1 build=<source revision>` (the revision comes from
`SR_BUILD_REVISION` or git at configure time; it identifies the source, not the NRO bytes, so
record the artifact SHA256 separately).

Closing the app stops and joins the native workers before the SDK terminates the title and
hard-exits (`ReXApp::OnWindowClosing`, called from the SDK `OnClosing`); the final summary then carries
`shutdown=complete`. Xenos is unaffected.

## What to expect from milestone 1

- The expected visual result is a **black screen**. No scene is drawn.
- A test round is **invalid** when any `BLOCKED`, `invalid` or `FAILED` line appears: milestone 1
  is not accepted with a blocker, and the first such event identifies what to implement next.
- There is no 30 FPS claim: `surface_paints` and `refreshes` are not game frame rates, and a
  black clear says nothing about draw cost.

## Log lines

A `[sr-native] summary` line is written every 10 s and once at shutdown. `BLOCKED` lines carry
opcode, ring/indirect location, counters, the last progress value and how long progress has been
stalled; `WAIT pending` lines carry the wait condition. The first event of every distinct site is
always logged, repeats are limited to one per 5 s per site, and after 32 distinct sites only the
summary reports them.

## Counters

The interval and final `[sr-native] summary` lines keep these meanings. They are different
quantities and must not be read as frame rate.

| Key | Meaning |
|---|---|
| `swaps` | `PM4_XE_SWAP` packets received from the game, whatever happened to the presentation. |
| `refreshes` | Native clears that were submitted **and** finished on the GPU (`RingCounters::refresh_completed`). A failed, cancelled or refused clear is not counted. |
| `surface_paints` | Successful surface paints reported by the SDK presenter (`Presenter::surface_paints()`), including repeated paints of the same image and UI-only paints. Not the game's FPS. |
| `draws_omitted` | Draws skipped on purpose by this milestone. |
| `blocked`, `invalid` | Packets that stayed pending (no semantics yet, missing effect) and malformed packets. |
| `progress` | Increases only after a packet was processed, a read-pointer write-back happened or an interrupt was delivered. The vblank timer is not progress. |
| `vblanks` | Timer-driven vblank interrupts (source 0). |

`shutdown=complete` appears on the final line only after the ring and vblank workers were joined.
Each worker gets 5 s to exit; if one does not, the line is `summary kind=final shutdown=incomplete`,
the presenter and provider are not freed, and the report tool fails the run.
`SHADER rejected stage=... reason=... first_words=...` is logged once per distinct rejected shader
(at most 16), so the first blocked draw says why the proof failed.

## Behavior that is deliberately blocked

- A swap blocks until the clear completed; with no presentation it blocks and says so.
- Draws are omitted only when `ShaderIsMemorySafe` proves the loaded shaders cannot memexport.
  Anything the scan does not fully understand (reserved bits, unknown fetch opcodes, jumps
  outside the control-flow section, memory-export allocation) is unsafe and blocks the draw.
- The shader proof also requires the control flow to end in an unconditional `exece`, so execution
  cannot fall off the scanned section into unchecked instruction data.
- A `PM4_INTERRUPT` with no interrupt callback installed stays pending (the SDK drops it silently);
  it completes as soon as the guest installs the callback.
- `COHER_STATUS_HOST` dirty, resolves with memory payloads, unknown queries and similar effects
  are never completed by guessing.

## Presentation

- The guest output is a fixed 1280x720 black clear; the guest swap size only sets the aspect ratio.
- Native mode asks the SDK presenter to paint from the UI thread only
  (`Presenter::SetPaintFromUIThreadOnly`, default off so Xenos is unchanged), so the ring thread
  never blocks on the host surface.
- Fence waits poll in 50 ms slices, report timeouts and stop on cancellation. Nothing a pending
  submission may use is reset or destroyed; shutdown that cannot drain the GPU retains the
  presenter and provider and logs it.

## Not verified on hardware

Host tests use fake callbacks. The Vulkan clear, the UI-thread paint path, the interrupt delivery
and the shutdown order still need the console rounds in Tasks 6-7.

## Console test procedure (milestone 1)

Two equivalent rounds, each: boot, 60 s, then a manual exit in full (hbmenu full) mode. One
thing changes per round (`sr_renderer`); the previous NROs and configuration are kept.

1. **Build.** Host tests: `docker run --rm --mount "type=bind,source=<repo>,target=/project" -w /project superman-returns-nx-mesa:build bash tests/test_sr_native.sh`.
   NRO: `tools/switch/rebuild.sh` in the `superman-returns-nx-check` volume (incremental; if
   `/work/game-check/CMakeCache.txt` is missing run `tools/switch/compile-check.sh` first, never
   delete the volume). Set `SR_BUILD_REVISION` to the source revision when building.
2. **Name and record.** Copy the NRO to `superman_returns-native-boot-test.nro`, keep the ELF
   from the same link in `out/console/symbols/` and note size and SHA-256 of both
   (`out/console/builds/<build>/MANIFEST.txt`). Never symbolize a failure with another NRO's ELF.
3. **Upload (FTP `192.168.100.37:5000`, server enabled by the user).** `cwd` into
   `/switch/superman-returns-nx` *before* listing. Download `superman_returns.toml` as a backup
   and check it is the current one before editing; change only `sr_renderer`. Upload the new NRO
   under its new name, then download it back and compare size and hash. Do not overwrite earlier
   NROs or any game package.
4. **Round.** The user opens the NRO. Round A: `sr_renderer = "xenos"` (baseline). Round B:
   `sr_renderer = "native"`. The user reports audio, boot progress and how the exit went.
5. **Collect** the log and the TOML into a new local folder named by date, build and mode, and run
   `python tools/switch/native-report.py --expect-build <revision> <log>` (only the last run in an
   appended log is judged). If the run is blocked, ask for an exit and
   download the log; do not change synchronization to make it look like progress. Restore only the
   key that was changed, and only if that keeps later edits by the user.

`native-report.py` never reports a pass. Its statuses are `failed` (setup/Vulkan failure, invalid
packets, no report, missing `shutdown=complete`, not a native run), `blocked` (blocked counter or
`BLOCKED` event, no or stalled progress) and `needs_console_review`, which still requires the
manual checklist it prints. Presentation counters (`refreshes`, `surface_paints`) never turn a
blocked run into a pass. It only reads the log: no SD or config access, nothing is published.
