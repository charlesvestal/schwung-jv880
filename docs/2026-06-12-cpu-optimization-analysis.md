# JV-880 Emulator CPU Optimization Analysis (Move / CM4)

**Date:** 2026-06-12
**Branch:** `perf/emulator-optimizations`
**Baseline:** ~38% CPU on CM4 (per README), ~46ms latency, buffered operation.
**Scope:** emulator core may be modified; bit-exactness desirable, small deviations acceptable for large wins.

## Architecture (as found)

- Dedicated emu thread (`v2_emu_thread_func`, `jv880_plugin.cpp:1442`) runs `updateSC55(64)` batches, converts int16→float, resamples 64kHz→44.1kHz via libresample, pushes into a 512-frame mutex-guarded ring; host callback drains it (`v2_render_block`, `jv880_plugin.cpp:3573`).
- Emulated chip: Hitachi **H8/532** MCU + Roland custom PCM chip. Hot loop `MCU::updateSC55` (`mcu.cpp:780-800`): every instruction it (a) scans ~29 interrupt sources, (b) executes one instruction (fixed 12 cycles — a documented FIXME approximation), (c) clocks 3 FRT timers + timer8, (d) updates UART RX/TX + analog, (e) catches up the PCM chip. One PCM step (~711 cycles, emits 2 samples — oversampling hardcoded on) per ~59 MCU instructions.
- PCM voice loop (`pcm.cpp:954-1413`) processes **all 32 slots unconditionally** every step — address generation, ROM gather reads, interpolation, 3 envelope calcs, ~6 32-bit mults per slot — whether or not the voice is sounding. `active` only gates the final accumulation (`pcm.cpp:1338`).
- Estimated time split (code-reasoned, NOT profiled): PCM rendering ~45-55%, interpreter dispatch ~25-35%, per-instruction interrupt/timer/UART scanning ~10-20%, resample+convert ~5-10%, LCD ~0% (event-driven only — not a target).

## Prioritized opportunities (gain/effort)

| # | Optimization | Est. gain | Effort | Risk | Refs |
|---|---|---|---|---|---|
| 1 | Build flags: `-mcpu=cortex-a72`, `-flto`, `-fvisibility=hidden -fno-semantic-interposition`, `-funroll-loops` (currently `-Ofast -march=armv8-a -mtune=cortex-a72`, 4 TUs, no LTO → hot cross-TU calls can't inline; PLT indirection in shared lib) | 8–20% overall | Trivial | None | `scripts/build.sh:53-77` |
| 2 | **Skip fully-inactive PCM voices** — early-out path for slots with `!active && !key && !okey`, preserving the exact state writes/IRQ flags the firmware reads back (`ram2[7]` key bit `pcm.cpp:1417`). Also removes random wave-ROM gathers for silent slots. | **20–40%** at typical polyphony | Medium | Medium — validate vs reference render | `pcm.cpp:954-1413` |
| 3 | Gate `MCU_Interrupt_Handle` behind a single `interrupt_pending_any` flag set in `MCU_Interrupt_SetRequest`; skip the 29-source scan when nothing is pending | 5–12% | Low | Low | `mcu.cpp:486-580`, `mcu.h:550` |
| 4 | Fast-forward `mcu.cycles` to next timer/PCM/UART deadline while `mcu.sleep` is set (SLEEP opcode exists, `mcu_opcodes.cpp:142`; loop currently still runs all peripheral machinery every 12 cycles while asleep). PCM is driven off `mcu.cycles` with its own catch-up loop, so coarser MCU stepping is safe for sample count. | 5–25% (idle-fraction dependent) | Medium | Medium — next-event computation must not skip interrupts | `mcu.cpp:788-798` |
| 5 | Emu thread: SCHED_FIFO + pin to cores 0–2 + **FTZ/DAZ** (no denormal handling anywhere today; float resampler tails can denormalize — slow on A72). Replace mutex ring with lock-free SPSC. | Latency ↓ toward ~15–20ms; CPU neutral; kills worst-case spikes | Low–Med | Low | `jv880_plugin.cpp:1442-1524, 1480, 1507` |
| 6 | `TIMER_Clock` next-event scheduling instead of per-instruction recompute of 3 FRT comparators + timer8 | 4–8% | Medium | Medium | `mcu.cpp:710-753` |
| 7 | NEON-vectorize PCM mix (4 slots at a time; `multi` at `pcm.cpp:250` is a 32×8 mult ×6/slot). Partial payoff — address gen has data-dependent branches + ROM gathers. Note: abandoned gather→mix split attempt visible at `pcm.cpp:947-952` (commented out). | 10–25% of PCM cost | High | Med-High | `pcm.cpp:954` |
| 8 | Shrink ring/pre-fill (512-frame ring ≈11.6ms, half pre-filled, `jv880_plugin.cpp:171, 1185`) — only after #5 makes the producer deterministic | Latency ↓ ~10ms | Low | Low (after #5) | |

## Recommended sequencing

1. **Phase 1 (nearly free, low-risk): #1 + #3 + #5.** Plausibly recovers 15–30% CPU and meaningfully cuts latency. Ship and measure.
2. **Phase 2 (the big algorithmic win): #2**, validated by rendering a reference corpus (representative patches, scripted note sequences) before/after and diffing output. Add #4 if profiling shows meaningful firmware sleep time.
3. **Phase 3 (only if still needed): #6, #7, #8.**

## Before Phase 2: profile on-device

The PCM/MCU time split above is reasoned from code density, not measured. Run `perf` (or a poor-man's cycle counter around `PCM_Update` vs the rest of the loop) on the Move to confirm where time actually goes before investing in #2/#7. Also unknown statically: how often the JV-880 firmware executes SLEEP (determines #4's value), and libresample's actual group delay (part of the 46ms budget).

## Validation harness (prerequisite for #2/#4/#6/#7)

Build a headless render mode: load ROMs, play a scripted MIDI sequence across a set of factory patches (including looped/sustained and velocity-switched ones), write WAV. Bit-diff (or null-test) optimized vs unoptimized output. The state writes that matter for correctness beyond audio: PCM IRQ flags and per-slot key bits the firmware reads back.

---

## Results addendum (2026-06-12, measured on CM5/Armbian Move — NOT stock CM4; expect proportionally larger shares there)

Workload: patch 25, one sustained note. Emulator thread CPU via /proc thread sampling + JV880_PERF_STATS.

| Stage | Thread CPU | Notes |
|---|---|---|
| Stock build | 31.1% | |
| Phase 1: -mcpu/LTO/visibility + interrupt gate + FIFO45/pin/FTZ | 20.6% | The bulk of the win |
| + PCM skip-inactive voices | 20.9% | Bit-exact, 28% faster in isolation on host — invisible on-device (post-LTO remainder is interpreter-dominated) |
| + ring poll 50us→1ms | 19.9% | Wakeups 20k/s → ~800/s; CPU accounting barely moved |
| + fixed-ratio 441/640 polyphase resampler | 16.6% | resamp bucket 24.2%→6.8% of thread; quality improved (alias −123dB vs −82dB) |
| + MCU sleep fast-forward | 16.0% | 62% sleep fraction, next-event jumps; LTO had already cheapened per-step calls |

**Conclusion: −49% total; diminishing returns reached.** Remaining ~15% ≈ PCM real work + awake-step interpretation. Further meaningful gains would require interpreter-level work (#7-class effort) for single-digit returns — not recommended. The perf-stats instrumentation (JV880_PERF_STATS) and WAV harness (tools/render_test) remain for future work.
