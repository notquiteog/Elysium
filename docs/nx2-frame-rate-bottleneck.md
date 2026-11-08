# Where the frames go on Switch 2 titles

Mario Kart World, Ocarina of Time and Kirby Air Riders all run well short of 60 fps while
neither the CPU nor the GPU is near its limit. This is what that turns out to be, and what it is
not, so the obvious fixes are not tried again blind.

## Nothing is saturated

Mario Kart World, 70 s into a run on an RTX 5060 Ti:

| | |
|---|---|
| busiest thread (`GPUBack`) | 71% |
| emulated CPU cores | 28-36% each |
| process total | 384% of the 2000% available |
| host GPU | 66% utilisation, **61.6 W**, 2820 MHz |

A card that draws over 180 W under load is drawing 62. It is not short of work to do; it is
short of work being handed to it.

## The GPU front thread waits, but it is not the limit

`EDEN_NX2_GPU_PROFILE=1` reports where the GPU thread's second goes. Mario Kart World:

| zone | ms per second |
|---|---|
| writer wait | 384 |
| idle | 300 |
| split drain | 145 |
| push | 46 |

and within the waits, macro DMA reads are 204 ms/s of it. `host_cpu_busy` is ~330 ms/s: the
thread is a third busy and spends much of the rest waiting for the back.

That wait has an obvious cause. In `GpuSplit::MemoryWriterTarget` the target starts at

```cpp
u64 target = std::max({last_writer_position, last_sync_position, last_memory_write_position});
```

and the ranged-writes loop below can only raise it. An unranged write is therefore a floor under
every later macro DMA read: a read of one range waits for a write to any other. Semaphore
releases are recorded unranged (`Puller::OnBack` calls `NoteMemoryWrite`), although
`ProcessSemaphoreRelease` knows the address it writes.

## Ranging those releases works, and buys nothing

Ranging both release sites to `[address, address + 16)` and measuring Mario Kart World both ways:

| | unranged | ranged |
|---|---|---|
| macro dma read wait | 203.7 ms/s | 129.8 ms/s |
| writer wait | 384.1 ms/s | 323.5 ms/s |
| deferred waits | 333/s | 208/s |
| idle | 299.9 ms/s | 371.9 ms/s |
| frame rate | 27.6 fps | 24.4 fps |

The waits fall by a third, and every millisecond saved turns into idle. The frame rate does not
improve; this run had it slightly worse, which is within what this machine varies by.

That is the finding: **the GPU front thread is not the bottleneck**, so cutting its waits cannot
raise the frame rate. Upstream reached the same place from another direction -- its commit
"Revert EXP10 (narrow macro DMA drains): no gain in Ocarina of Time" backs out a different
narrowing of the same waits.

The experiment is in the tree behind `EDEN_NX2_RANGED_RELEASES=1`, off by default. It is kept
because the reduction is real and this is one machine: a host whose front thread *is* the limit
would see that third of a second differently, and nothing here rules that out.

It is off rather than on for a reason worth knowing before anyone turns it on. `CounterReport`
may write reports queued earlier along with the one being released, and each of those was ranged
at its own, earlier position; a reader of one of those ranges would then wait for a position the
back has already passed. The hazard is unproven but unrefuted, and it would show as rare
corruption rather than a crash. The range itself is sound: a report is a payload and optionally a
timestamp, written 16-byte granular, which is what `query_cache.h` masks its watch address to.

## The back thread is the one that is busy

`Nx2Profile` does report the back thread, under "GPU back thread per second". Mario Kart World,
its second accounted in full:

| zone | ms per second | rate |
|---|---|---|
| other | 405.4 | |
| texture upload | 159.5 | 417/s |
| method | 82.9 | 2,737,295/s |
| split wait (idle) | 77.5 | 6,170/s |
| draw | 45.0 | 40,323/s |
| fill image views | 42.4 | 40,323/s |
| compute | 32.4 | 3,952/s |
| bind stage buffers | 31.7 | 85,894/s |
| update buffers | 22.6 | 40,323/s |

It idles **77.5 ms of each second**, against the front thread's 300 to 372. So the front waits
on the back, the back is busy, and the host GPU is still only 66% at 62 W: the back is not
feeding it fast enough, and the back is where the frames are going.

Two things stand out. **"Other" is 405 ms/s**, two fifths of the thread and larger than any
measured zone, and by definition everything outside the named ones. **Texture upload is 159.5
ms/s across 417 uploads**, 0.38 ms each, a sixth of the thread for a rate that is small beside
the 40,323 draws.

## Read the frame rates here with care

Two runs of the *same binary*, same title, same desktop, back to back: **25.7 fps and 35.4 fps**.
A 38% spread with nothing changed between them.

So a frame rate from one run says almost nothing, and any difference here smaller than about a
third is not evidence. The shares `perf` reports are steadier, because they are ratios within a
run rather than a comparison between runs, and the conclusions below lean on those where they
can. Where only a frame rate is quoted, treat it as an indication.

Anything measured by running twice and comparing needs many more runs than that, or a workload
that repeats exactly. Neither is true of booting a game and watching its attract demo.

## Measured on a real display

Everything below the next heading was measured under Xvfb and carries its cost. These are from
a normal desktop, `perf` sampling the back thread with no instrumentation effect:

| share of `GPUBack` | symbol |
|---|---|
| 19.3 - 21.1% | `Tegra::Texture::UnswizzleTexture` |
| 9.1 - 14.9% | libc, memcpy |
| 7.2 - 9.9% | `GpuSplit::BackLoop` |
| 3.6% | `GpuSplit::Execute` |
| 3.0 - 3.9% | `osDevReadReg032` (NVIDIA MMIO) |

Mario Kart World runs at **34.9 - 35.7 fps** here, not the 24 to 27 the headless runs showed:
Xvfb was costing about 30% of the frame rate on its own. The GPU sits at 73% and 60 W.

So the largest identifiable cost on the thread that gates the frame rate is unswizzling guest
textures on the CPU, with memcpy behind it.

### Ocarina of Time is not the one with the problem

Profiled the same way on the same desktop, Ocarina of Time runs at **59.5 fps** -- at its limiter,
not near it -- with the GPU at 70% and 76 W. Its back thread:

| share | symbol |
|---|---|
| 13.88% | `GpuSplit::BackLoop` |
| 11.95% | `Tegra::Texture::SwizzleSubrect` |
| 11.41% | `Tegra::Texture::UnswizzleTexture` |
| 8.42% | libc, memcpy |
| 5.04% | `pthread_mutex_lock` |
| 4.30% | `osDevReadReg032` |

So unswizzling costs it about half what it costs Mario Kart World (11.4% against 19-21%), and
whatever is left over is enough to hold 60. Ocarina also spends as much again in `SwizzleSubrect`,
swizzling data back towards the guest, which barely appears in Mario Kart World: the two titles
load the back thread differently.

That narrows the problem. **Mario Kart World at ~35 fps is the title that needs work**, and
unswizzling is twice the share of its bottleneck thread than of a title that reaches its target.

### Two ways of not doing it on the back thread, both worse

**On the GPU.** `gpu_unswizzle_enabled` exists and defaults off. Per upload it measured 0.217 ms
off against 0.391 ms on -- about 80% dearer for the same work -- so the default is right. Its
path is narrow anyway: BCn, 3D, one level, one layer, above a size threshold.

**In parallel.** `UnswizzleWorker::TryUnswizzle` splits the work across the thread pool but bails
when `(pitch & 3) == 0`, which is nearly always. That guard looks like an oversight and is not.
Forcing the split on:

| | serial | split |
|---|---|---|
| `UnswizzleTexture` | 21.14% | 16.18% |
| memcpy | 9.10% | 14.86% |
| frame rate | 34.9 fps | 25.9 fps |

The split must first copy the whole input to snapshot it against guest writes, and that copy
costs more than the parallelism saves: unswizzle time falls, memcpy rises further, and a quarter
of the frame rate goes. Not kept.

What is left is making `UnswizzleTexture` itself cheaper, or uploading less: 19 of this machine's
20 cores are idle, but neither way of handing them the work pays for the handover.

The split's frame rates (34.9 against 25.9) sit inside the spread above and would not stand on
their own. What carries that conclusion is the shares moving the way the mechanism predicts:
unswizzle time down, memcpy up by more, both within single runs.

### Xvfb is in every number above

`perf record` on the back thread, sampling from outside with no instrumentation effect, gives a
different answer again:

| share | symbol |
|---|---|
| 23.88% | `_copy_from_iter` (kernel) |
| 11.63% | `Tegra::Texture::UnswizzleTexture` |
| 6.75% | `GpuSplit::BackLoop` |
| 6.05% | libc, memcpy |
| 2.85% | `osDevReadReg032` (kernel, NVIDIA MMIO) |

and the call graph under the first one is

```
_copy_from_iter <- copy_page_from_iter <- unix_stream_sendmsg <- writev
  <- xcb_send_request <- xcb_put_image_checked
```

That is XCB pushing the frame to the X server through a unix socket, because these runs are
headless under `Xvfb`, which has no GPU and so presents by copying. **A quarter of the back
thread's time in these measurements is the test harness.** Every headless figure in this
document carries it: the zone shares, the 66% GPU at 62 W, the frame rates.

The symptom is still real -- it is reported on a normal desktop too -- but it has to be measured
there. `Xvfb` is fine for checking that a title boots and renders; it is not fine for asking
where the time goes.

What survives from this profile is what does not touch the display: `UnswizzleTexture` at 11.63%
is CPU texture unswizzling in our own code, on the thread that is the bottleneck.

### The profiler changes what it measures

Before trusting any of the above too far: `Maxwell3D::InitializeRegisterDefaults` sets

```cpp
fast_maxwell3d = !record_write_history &&
                 !g_trace_first_method_writes && !VideoCommon::Nx2Profile::Enabled();
```

so **turning the profiler on turns the fast Maxwell3D path off**. A profiled run takes the
generic method path, with its virtual call and its zone, instead of the specialised one that
ships. The zone shares above are of a slower, differently shaped run, and the method cost in
particular is overstated against what a normal run does.

Instrumenting that branch and letting the profiler keep the fast path was tried: "other" stayed
at ~500 ms/s, so it did not answer the question it was added for, and whether its `Scope` per
method costs anything could not be measured against the spread above. Reverted. With `perf`
available there is little reason to carry unmeasurable overhead in the hottest path for a zone
profiler that `perf` supersedes.

So "other" is not the fast path's method execution. It is genuinely unattributed time on the
generic path, and finding it needs either a profiler that does not disable what it measures or a
sampling profile of the back thread from outside.

### Next

Split "other" without changing what runs -- `perf record` on the back thread would do it without
the `fast_maxwell3d` problem -- and look at why a texture upload costs 0.38 ms.
