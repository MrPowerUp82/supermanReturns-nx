# Checkpoint 8 — native renderer, milestone 1 (state after Task 7)

Date: 2026-10-02. Branch `codex/native-renderer` (worktree
`C:\Users\webpa\.codex\worktrees\superman-native-renderer\supermanReturns-nx`), **not merged to `main` and
not pushed**. The Task 3/4 work of the other machine (`C:\Users\Gusta`) was never committed and was redone here
inline (no subagents). `checkpoint6.md`/`checkpoint7.md` are transcripts from the other machine.

## Verdict

**The milestone is NOT accepted.** Everything host-side is done and verified; every criterion that needs the
console is pending because the FTP server (`192.168.100.37:5000`) timed out on 2026-10-02 (three probes) and
no NRO has run on the Switch. Nothing below claims correct video, gameplay or any frame rate.

## Commits (on top of `34fce6e`)

| Commit | Content |
|---|---|
| `900a2fd` | Task 3: `sr_native_system.*` (SDK adapter, MMIO, workers, interrupts, write-back), `sr_native_shader_safety.*`, lifecycle/shader tests |
| `5fe5347` | Task 4: `sr_native_present.*` (`ClearSequence` + Vulkan clear), `RecordRefresh`, SDK `Presenter::SetPaintFromUIThreadOnly` and `surface_paints()` |
| `8f82d3c`, `fca37bd` | Task 5: `sr_renderer` cvar, `OnPreSetup` injection, `ReXApp::OnWindowClosing` hook, diagnostics, notices |
| `8a30619` | Task 6: `tools/switch/native-report.py`, tests, console procedure |
| (this) | Task 7: this checkpoint and doc status |

## Build and artifacts (facts)

- Source revision of the NRO: `fca37bd8c980` (logged at startup). Task 6 added only tools/tests/docs.
- `out/console/builds/native-boot-test-fca37bd/superman_returns-native-boot-test.nro` — 60,045,509 bytes,
  SHA-256 `2bad9f87aaffa2cc04ace71a84d619ac0fce77fc5b6c279ff9a190020083074b`.
- `out/console/symbols/superman_returns-native-boot-test-fca37bd12.elf` — same link (12:09:01Z), 80,456,224 bytes,
  SHA-256 `2f3f840a24cb7345f9bd70ad847ac036d2f8f22b9092ffc142aa204b4df0693b`. Contains the build string.
- Incremental build in volume `superman-returns-nx-check` (`.superpowers/.../rebuild.sh`, content-diff sync).
  Link finished with no `undefined reference`; native code is referenced (strings `sr-native`, `sr_renderer`).
- No config was changed on the SD card; no file was uploaded. The previous NROs are untouched.

## Verification run on 2026-10-02 (facts)

- Host tests (`tests/test_sr_native.sh`, Docker Linux): PASS. UBSan on all three binaries: PASS.
- Python suite (`unittest discover -s tests`, Linux container): PASS (15 pre-existing + 16 new report tests).
- `shaders/test_pack_identify.sh`: exit 0 (synthetic pack identification did not regress).
- `git diff --check`: clean. Search for NFSMW addresses/hooks in `app/src/sr_native*`: no matches.
- Cross compiler (devkitA64, app flags, `-Wall -Wextra`): no warnings in the native units.
- Mutation checks killed: shader proof allowing eM0 export, `ClearSequence` destroying with work in flight or
  without checking creation flags.

## Known gaps / risks (read before trusting the above)

- **GCC ASan is unreliable in this Docker**: even an empty program hangs intermittently (DEADLYSIGNAL). Each
  native test binary passed ASan+UBSan at least once; no stable ASan run exists. TSan does not start here.
- **No independent review** of Tasks 3-5 (earlier tasks had real findings from reviews). Recommended next.
- **SDK changes to review**: `presenter.h/.cpp` (opt-in paint mode, `surface_paints`), `rex_app.h/.cpp`
  (`OnWindowClosing`), and `guest_memory_switch.cpp` differed in the build volume from the worktree (pre-existing
  on `main`, unrelated).
- Heap validation walks `QueryRegionInfo` per access; cost on large regions was not measured.
- The all-queue drain of the old design was dropped (the presenter destructor awaits its own submissions); not
  validated on hardware.
- Initialization failures (`InitializeRingBuffer` invalid, ring memory lost) log `FAILED` and idle the ring; they
  do not abort the process.

## Criteria of the spec — status

| Criterion | Status |
|---|---|
| NRO compiles/links, starts in full mode with `sr_renderer = "native"` | compiled and linked; start **pending console** |
| Logs confirm native selection, ring consumption, guest progress, Vulkan presentation | log lines implemented; **pending console** |
| Same 60 s window as the Xenos boot without new crash or hang | **pending console** |
| Omitted draws counted; no parser errors, ignored blockers or fabricated sync | counted/blocked in code and host tests; **pending console** |
| User exit ends the app with no stuck workers | hook implemented (`OnWindowClosing` → quiesce); **pending console** |
| Xenos run preserved | default unchanged by construction (`sr_renderer = xenos` returns before injection; presenter opt-in defaults off); **pending console** |

## Hypotheses to confirm on the first native round (not facts)

- First blockers will likely be PM4 effects without semantics yet: `EVENT_WRITE*` with memory payload, resolve /
  copy modes, `COHER_STATUS_HOST` dirty, unknown `VIZ_QUERY`, memexport shaders, or swaps if the clear fails.
- A black screen with `blocked=0`, advancing `progress` and `shutdown=complete` is the expected clean outcome;
  audio/boot events remain a manual observation.

## Limits that guide the next designs

1. EA video (`DATA/fmvlegal.AST`): decoder route not confirmed; do not assume the NFSMW WMV3 path.
2. Captured shader containers for the pack must be checked and validated on the SD before rebuilding the pack
   (the pack identified 1/7 VS and 0/20 PS in the earlier round).
3. The 1280x720 black target avoids the SDK size-change waits; real drawing will need a real resolution policy.
4. 30 FPS is a final target to measure later, never inferred from `refreshes`/`surface_paints`.

## Next actions

1. User enables FTP. Then follow "Console test procedure" in `docs/native-renderer.md`: backup TOML, upload
   the NRO under the new name, verify hash, round A `xenos`, round B `native`, run `native-report.py`.
2. Request an independent review of Tasks 3-5 and of the SDK diffs.
3. After a clean console round, record opcodes/waits/omitted draws here and mark the milestone; otherwise fix the
   first blocker in the task that owns it and repeat only the affected round.
4. Push or merge the branch so this work is not stranded on one machine again.
