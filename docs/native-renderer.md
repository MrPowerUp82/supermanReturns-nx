# Native renderer (milestone 1)

Status: Tasks 1-5 of `docs/superpowers/plans/2026-10-01-renderer-nativo-marco1.md` are implemented
and host-tested. Nothing here has run on the Switch yet. Milestone 1 only consumes the PM4 stream and presents an opaque black image; game draws
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

## Behavior that is deliberately blocked

- A swap blocks until the clear completed; with no presentation it blocks and says so.
- Draws are omitted only when `ShaderIsMemorySafe` proves the loaded shaders cannot memexport.
  Anything the scan does not fully understand (reserved bits, unknown fetch opcodes, jumps
  outside the control-flow section, memory-export allocation) is unsafe and blocks the draw.
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
