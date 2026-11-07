<!--
SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Where shader translation time goes

First-run stutter is translation: Maxwell code to IR, the optimization passes, then SPIR-V.
`eden-shader-bench` measures it offline against a real Vulkan disk shader cache, so the cost can
be attributed to a pass instead of guessed at.

```bash
cmake --build build --target eden-shader-bench
./build/bin/eden-shader-bench <user folder>/cache/shader/<program id>/vulkan.fork-v57.bin
```

It runs single-threaded what the Vulkan pipeline cache runs before the driver compile — CFG,
`TranslateProgram`, the dual vertex merge, `ConvertLegacyToGeneric`, `EmitSPIRV` — and prints the
per-pass times, the slowest pipelines, and with `--hashes FILE` a hash of every emitted module.

## Measured: Ocarina of Time

5617 pipelines (5354 graphics, 263 compute), 11604 programs, **11536 modules**, best of 3 runs,
**25979 ms** — about 4.6 ms a pipeline. Output hash `83f607f2ef89d480`.

| pass | ms | share |
| --- | --- | --- |
| SsaRewrite | 7764 | 29.9% |
| GlobalMemoryToStorageBuffer | 4022 | 15.5% |
| TranslatePass | 3675 | 14.1% |
| DeadCodeElimination | 3429 | 13.2% |
| EmitCode | 2321 | 8.9% |
| ConstantPropagation | 1135 | 4.4% |
| EmitContext | 800 | 3.1% |
| CFG | 622 | 2.4% |
| Texture / BlitConstants / CollectShaderInfo | 463 / 459 / 368 | 1.8 / 1.8 / 1.4% |
| EmitFinish | 238 | 0.9% |

The slowest single pipeline is 54 ms. Mario Kart World, for comparison, is 9537 modules in about
14.5 s.

## What that means for where the work belongs

Of the 26 s, the SPIR-V emission side — `EmitContext` + `EmitCode` + `EmitFinish` — is **3359 ms,
13%**. That part already runs on a pipeline worker: `vk_pipeline_cache` hands `GraphicsPipeline`
an `emit_stages` callback and the worker's build function calls it. The other **87%, the IR
pipeline, still runs on the GPU thread**, because `emit_stages` closes over `job->programs`, which
are already-translated IR.

Moving that 87% is not simply a matter of deferring it further, and the reason is a deliberate
decision in this tree rather than a missing feature. Translation reads live 3D engine state
through the environment, so a worker needs a snapshot of it, and more importantly the GPU thread
has to do something useful while the worker runs. `BuiltPipeline` returns the pipeline **anyway**
when a native draw's build is unfinished — this tree blocks rather than dropping the draw, which
`async_draw_policy.h` states and `tests/video_core/async_draw_policy.cpp` asserts ("Native pending
draws are never discarded", exhaustively). So for the draw that first needs a pipeline, handing
translation to a worker and then waiting on it wins nothing.

Under that invariant the only lever is translating **before** the pipeline is needed.
`PipelinePrebuild` already does it for pipelines in the disk cache — `background_pipelines.Add` is
called from `LoadDiskResources` — which is why a second run stutters far less than a first. What
has no answer yet is a pipeline never seen before. Forks solve it by skipping the draw; that is
the one thing this tree has decided not to do.

## Reading the numbers honestly

- **Absolute times are not comparable between runs on a loaded machine.** One binary on one input
  measured 2885 ms and 5274 ms for the same pass, an 83% spread, at load average 17. Always build
  both versions, alternate them, and distrust anything smaller than the spread.
- **`--hashes` is the point of the tool.** An optimization that claims to leave the SPIR-V
  unchanged can be checked here rather than taken from another fork's measurement: emit, diff the
  per-module hashes, compare the total. Three changes have been verified that way, all identical
  over the 11536 Ocarina modules.
- **Upstream translation wins have not reproduced on these caches.** A storage-buffer search fix
  measured at 593 ms to 75 ms upstream gave about 3% on Mario Kart World, upstream's own benchmark
  title; three SSA-rewrite optimizations gave nothing distinguishable from noise on either
  Ocarina or Mario Kart World, when alternated at low load. They are output-identical and remain
  as guards against the pathological shaders upstream profiled, which are not in these caches.
