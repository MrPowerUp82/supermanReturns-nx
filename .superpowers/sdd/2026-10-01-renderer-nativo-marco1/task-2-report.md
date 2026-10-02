# Task 2 — PM4 guest effects, indirects, and waits

Status: implemented and host-validated. Base `ccf55aa`; clean `git status --short`
at entry. Work stayed in the `codex/native-renderer` worktree. No application
integration, Switch run, imported hooks/passes, or game-derived data.

Reference revision: `df2de32ee569873062b8f8d1da8ad0b8d0a90a5d`.
Compared the packet switch and relevant handlers against
`sdk/src/graphics/command_processor.cpp`, SDK register definitions and Xenos
opcodes. The reference's timeout success, dirty-coherence clearing and synthetic
query results were deliberately excluded.

## Implementation and contracts for Task 3

- Added `Services`, `RingCounters`, `CompareWait`, and `RingExecutor` to the pure
  C++ header. SDK types appear only in implementation/tests, never its interface.
- Type 0 sequential/same-register and type 1 writes; RMW register/immediate masks;
  REG_TO_MEM, MEM_WRITE, COND_WRITE; event initiator and explicit SHD token writes;
  constant register/memory loads; all bin mask/select forms; retained shader
  loads; CPU-mask interrupt dispatch and validated swap request dispatch.
- Memory callback addresses retain their endian bits. Constants and pointer
  shaders use the SDK's `Endian::k8in32` for big-endian guest source words.
  `read_indirect(address, length, bytes)` receives a normalized physical address
  and dword length, and must return exactly `length * 4` guest-order bytes.
- Effect callbacks returning false must not commit guest effects. A true return
  commits the effect once. Register callbacks must perform SDK register side
  effects, including scratch writeback and marking coherency dirty. Actual
  committed-page/register bounds/access validation belongs to the adapter.
- `interrupt(cpu)` dispatches command-stream source 1 to CPU 0–5. `present`
  accepts a swap request; it does not establish that a frame was displayed.
  `swap_requests` counts one attempted request per retained packet.
  `refresh_completed` stays zero in the executor; actual presenter completion
  must be included separately by the system's reporting aggregate.
- Added optional
  `std::function<bool(uint32_t, std::span<const uint32_t>)> shader_is_memory_safe`.
  Its inputs are stage 0 (VS) or 1 (PS) and retained host-order microcode.
  Missing/false blocks draws using that shader. True must be a real, bounded
  proof that the shader cannot export guest memory; this task provides no
  default-safe analysis and links no translator. Task 3 must validate malformed
  code while implementing that proof.
- `pause_wait` must be short and cancellable. `report_wait` receives info,
  address, reference and mask on each false probe; the adapter rate-limits its
  diagnostics by time. A count or elapsed diagnostic limit cannot satisfy a
  wait. Dirty COHER_STATUS_HOST is blocked and is never modified by the executor.
- Indirect address/extent/alignment, length flags, depth <=4 and active
  normalized address/length pairs are checked before dispatch. Each active
  indirect owns its storage and cursor. Parent completion is withheld until
  children finish. Per-packet effect indexes retain successful writes/interrupts
  across subsequent callback failures or cancellation; read-derived values are
  latched before effects so partial RMW/conditional/constant packets do not
  reevaluate changing source values.
- An executor owns one ring generation. Recreate it when replacing a ring,
  including a same-address generation replacement. A pending packet rejects
  backing-span/position/mask changes; only end publication may advance. The
  executor is noncopyable to protect owned indirect spans.
- Added `last_blocked_opcode`, `last_blocked_address`, `draws_shader_blocked` to
  counters. Address is a main-ring byte offset or an indirect physical address.
  Blocked/invalid counters count returned attempts; packet/opcode counters count
  committed packets, including skipped predicates and indirect descendants.

## Unsupported operations remain blocked

EVENT_WRITE_EXT/ZPD, VIZ_QUERY, SHD counter-bit writes, event packets with an
unimplemented memory payload, WAIT_FOR_IDLE, copy/resolve draws, immediate-index
draws, visibility-conditioned draws and unknown opcodes cannot report success.
Regular DMA/auto-index draws are omitted only after full packet-format validation,
copy-mode checks and any loaded shader's safety proof. No guest query value,
GPU counter or coherency completion is invented. If the next adapter lacks shader
safety analysis, shader draws remain blocked and physical boot may remain pending.

## RED/GREEN and verification

All tests use deterministic fake memory/register maps, callback results and guest
byte streams; no game data. Existing parser tests remain intact, with additional
read-only/guard-page and ring-capacity checks.

1. RED before implementation: runner compilation failed on missing
   `Services`/`RingExecutor`/`CompareWait`. Temporary interface stubs then produced
   an assertion failure in `WaitTests` on the comparator matrix (exit 1 from
   runner; assertion process exit 134). Wait/cancellation, malformed, predicate,
   unknown-opcode and indirect-replay fixtures were already present.
2. GREEN implementation: normal `bash tests/test_sr_native.sh` passed. During
   implementation, the swap fixture caught an extra swap-signature byte reversal;
   comparison now uses already-decoded host-order `kSwapSignature` directly.
3. Added shader-proof interface fixtures before the interface (RED compilation:
   missing `shader_is_memory_safe`), then GREEN absent/false/true classification.
4. Wait safety mutation: compiled a temporary copy with forbidden success after
   1000 probes. The 1001-probe assertion failed (exit 134), proving a timeout
   completion cannot silently pass. The committed source never contained that
   mutation.
5. ASan+UBSan: an initial default-handler attempt emitted repeated DEADLYSIGNAL
   without a usable stack and was stopped. Bounded `handle_segv=0` full suite
   passed; minimal default ASan smoke and default full suite also passed. This
   initial anomaly was not attributed to a proven cause. Later expanded tests
   produced a concrete stack-use-after-scope in a nested initializer_list fixture;
   replaced it with owned packet vectors. Final default ASan+UBSan suite passed,
   including guard pages, without suppressing instrumentation or signal handling.
6. GCC/gcov on the executor/parser: 99.66% lines (294), 100.00% branches executed
   (509), 80.94% branches taken, 93.77% calls executed. The remaining source line
   is the unreachable comparator fallback after exhaustive masked values 0–7.
7. `git diff --check` passed. Self-review covered SDK field/endian parity,
   packet/indirect ownership, depth/cycle rejection, no parent commit on block,
   exactly-once partial effects, cancellation and counters versus actual display.

Final reproducible normal host command:

```powershell
docker run --rm --mount type=bind,source=C:/Users/webpa/.codex/worktrees/superman-native-renderer/supermanReturns-nx,target=/project --mount type=bind,source=C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx/sdk/thirdparty,target=/project/sdk/thirdparty,readonly -w /project superman-returns-nx-mesa:build bash tests/test_sr_native.sh
```

Final sanitizer command: same mounts/image, add `-e SANITIZE=1` and run
`timeout 30 bash tests/test_sr_native.sh` (exit 0). SDK includes have a narrowly
scoped GCC diagnostic suppression for their pre-existing unused parameters;
project code remains under `-Wall -Wextra -Werror`. Temporary coverage/mutation
files lived under container `/tmp`; reference/thirdparty files were not edited.

Tests cover comparators 0–7 and value-only masking; false wait for 1001 probes then
success; cancellation at probe 1001 without completion or writes; dirty coherence;
all supported handler minima and smaller payloads; endian-preserving callbacks;
register/immediate RMW masks; memory/register conditional writes; every bin form;
false predicates and predicated swaps; unknown opcode diagnostics; malformed
shaders/draws/signature; unsupported GPU operations without effects; partial
MEM_WRITE/SHD/interrupt/RMW/conditional/swap replay; indirect truncation, extent,
alignment, cycles, depth 4 success/depth 5 rejection, partial indirect resumption
and cancellation; generation mismatch; read-only bytes, guard-page boundaries,
and partial ring publication.
