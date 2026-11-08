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

## In real gameplay, none of the above is the cost

Everything else in this document was measured on an attract demo or a title screen. `perf` on a
session someone was actually playing -- Ocarina of Time, past the menus, 14 video-decode lines
against the title screen's 153 -- says something different:

| share of the process | thread |
|---|---|
| ~63% combined | the six emulated `CPUCore_N` threads |
| 14.58% | `GPUBack` |
| 14.19% | `GPU` (the front) |

and by symbol:

| share | symbol | thread |
|---|---|---|
| 4.80% | `GpuSplit::WaitFor` | GPU (front) |
| 4.32% | `GpuSplit::Backoff` | GPU (front) |
| 2.04% | `GpuSplit::BackLoop` | GPUBack |

**`Tegra::Texture::UnswizzleTexture` does not appear at all, not even at 0.05%.** The two
changes made to it (`f23a948ebb`, `189f0336b7`) were measured against an attract demo, where
texture streaming dominates. They are real and they help that, but they do not touch what a
played session spends its time on.

The largest identifiable cost in the whole process is the GPU front thread **spinning**:
`WaitFor` plus `Backoff` is 9.1%, and `Backoff` spins 16,384 times before it considers sleeping.
The back thread it waits for has no hotspot of its own -- 14.6% of the process spread thin
enough that nothing but `BackLoop` clears 2%.

Ocarina of Time in gameplay runs at **43-47 fps**, not the 59.5 this document previously
recorded. That figure was its title screen, which is a video.

So the shape of the problem in play is: the emulated CPU is the bulk of the work, the GPU front
thread spends a tenth of the process waiting on the back, and the back is diffusely busy. That
is a different problem from texture upload, and it is the one worth working on.

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

### Ocarina of Time has not been profiled in gameplay yet

A first attempt looked like good news -- 59.5 fps, GPU at 70% and 76 W, `UnswizzleTexture` only
11.41% of the back thread against Mario Kart World's 19-21% -- and it was worthless. The run
never left the title screen, and that title screen is a 1080p video: the log shows
`nx2:vdecsvc frame 3600 (1920x1080)` and 153 other video lines across the window that was
sampled. The profile is of a video decoder and a blit, which is why `SwizzleSubrect` is 11.95%
of it and why holding 60 was easy.

Mario Kart World's attract demo, by contrast, is real 3D. The two were never comparable, and
nothing should be concluded from the difference between them.

Profiling Ocarina properly needs it in gameplay, which a headless run cannot reach: it has no
input, so it cannot load a save or leave the menu. The way to do it is to attach `perf` to the
`GPUBack` thread of a session someone is actually playing.

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

## The headless runs had the split switched off

`elysium-cli` reads `sdl2-config.ini`, not the GUI's `qt-config.ini`, and its `gpu_accuracy`
sat at the built-in default of `High` (`ENUM(GpuAccuracy, Low, High)`, so `gpu_accuracy=1`)
while the GUI ran at `Low`. `GpuSplit::MaybeStart` requires `!Settings::IsGPULevelHigh()`:

```cpp
const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                     Settings::values.renderer_backend.GetValue() == RendererBackend::Vulkan;
```

So **the GPU thread split never started in a headless run**. "GPU thread split: engine methods
run on a second thread" is absent from every `elysium-cli` log, and with the split inactive
`WaitFor` is never called, so no front-wait figure could have come from one. Any number in this
document that concerns the split and was taken headless measured a thread that did not exist;
only the figures from a GUI session stand. Setting `gpu_accuracy=0` in `sdl2-config.ini`
(with `gpu_accuracy\default=false`, or the startup write-back restores `High`) makes a headless
run represent the configuration people actually play.

This is the fourth time in one investigation that a measurement turned out to describe something
other than what it was pointed at -- after Xvfb's 24% of the back thread, the zone profiler
disabling `fast_maxwell3d`, and a stale Ocarina of Time session left running through a Mario Kart
World benchmark. The pattern is the same each time: the run completes, the numbers look
plausible, and nothing in them says they are about the wrong thing.

## Where the front actually waits

With the split genuinely running (Mario Kart World, `EDEN_NX2_WAIT_STATS=1`), per-site
attribution puts essentially all of it in two places:

| window | site | share of spins | waits | mean spins |
|---|---|---|---|---|
| t=35s | list writers | 97.9% | 30,276 | 403 |
| t=40s | macro dma read | 56.5% | 2,299 | 5,263 |
| t=40s | list writers | 43.5% | 15,518 | 600 |

`macro dma read` is the long one -- 5,263 spins per wait against `list writers`' 600 -- and it is
what `EDEN_NX2_RANGED_RELEASES=1` narrows.

**The spin threshold is not a lever.** The front parks **0.1 times per wait**: of 17,817 waits
only 689 reached 16,384 spins at all. Lowering the ceiling can only convert those few hundred
long waits into futex sleeps; it cannot shorten a wait. An earlier A/B appeared to show the
threshold mattering because "reached the sleep" was read off the `>=16k` bucket rather than
counted at the sleep, so a lowered ceiling reported zero sleeps -- which is impossible, and was
the tell. The park is now counted where the thread actually parks.

And the front has nothing to do with the frame rate regardless: it idles 300 to 372 ms of every
second. Reducing its waits turns spinning into idling, which is worth having for power and for
weaker CPUs, but it is not where the frames are.

## Nothing is saturated, which is the whole diagnosis

`perf` over 30 seconds of a played session, weighting each sample by its period and dividing by
a P-core's 5.2 GHz:

| thread | Gcycles | cores busy |
|---|---|---|
| GPUBack | 115.2 | 0.74 |
| GPU (front) | 112.1 | 0.72 |
| CPUCore_1 | 89.2 | 0.57 |
| CPUCore_0 | 87.3 | 0.56 |
| CPUCore_2 | 84.2 | 0.54 |
| CPUCore_3/4/5 | ~80 each | 0.51 |
| VulkanWorker | 26.0 | 0.17 |
| **total** | **789.6** | **5.06** |

Not one thread is saturated. The whole emulator wants 5.06 cores on a machine with 6 physical
P-cores and 8 E-cores idle, and the GPU sits at 55 to 66%. The host is a 13th-gen i5-13600K, so
a hybrid-placement problem was the obvious guess -- and it is wrong: of 51,602 samples, **zero**
came from the `cpu_atom` PMU, so every thread ran on a P-core throughout.

When no thread, no core and no GPU is at capacity and the frame rate is still half of target, the
limit is not throughput. It is a serial dependency chain, and the cure is to shorten or overlap
it rather than to make any one stage faster or to add threads.

## The front thread does 113 ms of work per second

Its second, from the zone profiler: 384 ms/s writer wait, 203 ms/s macro DMA read wait, 300 ms/s
idle. That leaves about **113 ms/s of actual work -- 11% busy**. Every later idea about the front
has to be read against that number: it has an order of magnitude of slack, so no change to how it
waits can produce a frame.

## Parks, not spins

Wall-clock instrumentation (`EDEN_NX2_WAIT_STATS=1`) on Mario Kart World:

| | front wait | parked | mean wait | mean park |
|---|---|---|---|---|
| window 2 | 570.3 ms/s | 508.9 ms/s (89%) | 196 us | 1,525 us |
| window 3 | 472.6 ms/s | 315.4 ms/s (67%) | 118 us | 739 us |

The front parks 0.1 times per wait, so 90% of waits never sleep -- and the few that do are 67 to
89% of all the time spent waiting. **A spin count was the wrong instrument**, and reporting one
is what made the spin threshold look like a lever for a day.

These totals land on the zone profiler's 384 and 203 ms/s, measured independently and months
apart. That is the first cross-validation in this document; everything above it rests on one
instrument each.

### Why a park lasts a millisecond

The back publishes `consumed` and wakes the front at `Execute` boundaries, when it has advanced
4096 words or reached `wait_target`. `WaitForWriters` waits for `last_writer_position` -- the
newest compute or copy work queued. When that sits near `write_position`, the front is waiting
for the back to drain **everything queued**, which is a full pipeline flush, not a latency blip.
That is the millisecond.

Narrowing it the way the releases were narrowed is not available: ranging needs the write's
address range, and a compute dispatch's output range is not recorded anywhere. It would have to
be tracked first.

## GPU fence behavior: Balanced cuts the wait and costs frames

Mario Kart World, same build, frame rate averaged over ~1,250 presents from t=45s:

| `gpu_fence_behavior` | frame rate | front wait (two windows) |
|---|---|---|
| Default (0) | 33.9 fps | 217.8 / 420.0 ms/s |
| Balanced (2) | 31.3 fps | 137.1 / 262.0 ms/s |

With Default and `gpu_accuracy` Low, `is_synced` resolves true for fences, so the expectation was
that Balanced would unblock the thread and pay. It does unblock it -- front wait falls by 38% --
and the frame rate does not follow; here it was slightly worse.

**This is the control the rest of this document needed.** Balanced is a large, independent
reduction in front wait that produced no frames, which is what an 11%-busy front thread predicts.
It also retires a result from two sections up: ranging the semaphore releases cut front wait and
appeared to raise the frame rate from 27.7 to 37.1 fps in one pair and 31.4 to 33.4 in another.
Those gains cannot be attributed to the wait reduction when a bigger wait reduction here produced
none. Single-pair frame rates on this machine vary by more than the effect being claimed, and two
pairs are still single pairs.

What survives: ranging the releases measurably shrinks the site it targets (`macro dma read` waits
fall from 717/2246/2385 per window to 612/438/1581, mean spins from ~5,478 to ~3,952, parks down
17%). Whether that is worth the `CounterReport` hazard is a frame-rate question, and the frame
rate has not answered it.

## The guest CPU is blocked 400 ms/s reading GPU-written memory

Everything above this section measured threads with slack. This one does not. With
`EDEN_NX2_READBACK_STATS=1` on Mario Kart World:

| window | readbacks | guest CPU blocked | mean stall |
|---|---|---|---|
| 1 | 96/s | 466.1 ms/s | 4,844 us |
| 2 | 144/s | 479.9 ms/s | 3,343 us |
| 3 | 70/s | 330.1 ms/s | 4,707 us |

**A guest CPU thread is blocked for 40% of every second**, in stalls averaging 3 to 5 ms -- a
quarter of a 16.7 ms frame each. It is the first cost measured here that is both large and on the
critical path, and it accounts for the guest cores sitting at 0.51 to 0.57 of a core: they are
not short of work, they are stopped.

Only 57 distinct page ranges are involved, read over and over.

The scheduler, measured in the same runs, is not the problem: **1,526 submits/s, 7,046 render
passes/s, 132 finishes/s waiting 53.3 ms/s**. The fence batching that won 10-15% in Hyrule
Warriors (`DeferFlush`, ~22,000 submissions a second there) has nothing left to batch here.

### The split turned a readback into a pipeline drain

`OnCPURead` queues a sync operation, ticks the GPU thread, and blocks the caller on it:

```cpp
const u64 fence = RequestSyncOperation([this, raster_area]() {
    InvalidateGPUCache();
    renderer->ReadRasterizer()->FlushRegion(...);
});
gpu_thread.TickGPU(is_async);
WaitForSyncOperation(fence);   // the guest CPU thread stops here
```

`TickWork()` drains those requests -- but with the split active, the tick that reaches it is
itself deferred onto the back thread's ring:

```cpp
} else if (std::holds_alternative<GPUTickCommand>(next.data)) {
    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); });
}
```

So the guest CPU waits for the back thread to execute **everything already queued** before its
flush runs. That is the 3 to 5 ms, and it is a consequence of the split rather than of the flush
itself: the work is ordered behind the whole ring, not behind the writes it actually depends on.

Correctness only requires the flush to follow the writes to *that range*, which is what
`WaitForMemoryWriters` / `MemoryWriterTarget` already express for the macro DMA reads. Running the
flush at the range's last write instead of at the end of the queue is the shape of the fix.

### Removing the stall entirely does not produce frames

`EDEN_NX2_BUFFER_DOWNLOADS=1` copies GPU-written buffer ranges back at every fence instead of
waiting for something to ask. It is off by default for Switch 2 titles. Turning it on:

| | readbacks | guest CPU blocked | finishes | frame rate |
|---|---|---|---|---|
| off (default) | 74-125/s | 395-486 ms/s | 140/s, 70.0 ms/s | 35.1 fps |
| on | 0-3/s | **0.1-2.3 ms/s** | 18/s, 22.5 ms/s | 31.7 fps |

The stall is **gone** -- a 99.5% reduction, and the finish waits fall with it. The frame rate does
not improve; it was slightly worse.

So this is the third large, independent reduction in blocking that bought nothing, after Balanced
fences (-38% front wait) and ranged releases. Taken together they say what the per-thread cycle
table said at the top: the front's waiting and the guest CPU's stalling are **consequences of
waiting for the back thread, not causes of the frame rate**. The back thread is the only actor in
the system without slack -- about 92% of its wall clock occupied, 0.74 of a core burned -- and
everything else in this document is downstream of it.

One honest caveat: eager downloads are not a free removal of the stall. They copy (250-330 MB/s
on title 1), so a real gain could be masked by the copy cost. The clean version of this experiment
is the ranged flush described above, which shortens the stall without copying anything extra, and
it has not been run. Until it is, "the readback stall is not the gate" is well supported but not
proven.

What is no longer in doubt is where to look: the back thread, and what it spends 0.74 of a core
on.

## Ocarina of Time: our own Max Resolution patch was the 60 fps blocker

None of the above. The built-in `Max Resolution` patch, on by default for this title, exists to
stop the game lowering its own internal resolution:

> Ocarina of Time takes its "dynamic resolution disabled" path, so the 3D renders at full scale
> instead of following the GPU-time controller down to its 480p floor.

So the game cannot trade resolution for frame rate -- and the recommended profile then enables
FSR3 frame generation to make up the frames the patch suppresses. The two pull against each
other. Measured on a real display, Hyrule Field, twelve 600-frame windows each (7,200 frames an
arm), both with the profile and frame generation unchanged:

| | mean | range | GPU |
|---|---|---|---|
| Max Resolution on | 41.9 fps | 39.0-44.5 | 79% |
| Max Resolution off | 54.6 fps | six windows at 59.4-60.0 | 70% |

Those twelve windows an arm were a sample, and sampling flattered the result. The full runs,
every window after shader compilation settles:

| | windows | shape |
|---|---|---|
| Max Resolution on | 32 | **flat**: 33.2 to 44.6, mean ~41, no trend |
| Max Resolution off | 45 | **locked 58.9-60.0 for ~36 windows (~21,600 frames, six minutes), then 55.1, 53.7, 52.3, 15.3, 26.8, 37.6, 33.6** |

So the honest comparison is not "41.9 against 54.6". It is six minutes of a genuine locked 60
against a flat 41 -- a large win while it lasts -- followed by a decline into the 26-38 range that
**overlaps the patched run**. The earlier claim here that the two arms never overlap was taken
from the sampled windows and is withdrawn: at the tail they do.

The patched run showed no such decline over 39 windows, which raised the question of whether the
dynamic resolution path degrades over time or whether the tail was simply a heavier area.

**It was the area.** A ten-minute headless run with the patch off, on a scene that does not
change, holds 58.2 to 60.0 across 43 windows after shader compilation -- two dips (49.6 and 43.3)
and 60.0 at t=598. Nothing declines. That run was under Xvfb, which costs about 30% of the frame
rate, and a build was competing with it for part of its length; both can only have pushed the
number down, so holding 60 at the end is a lower bound. The dynamic resolution path is stable
over time.

So the honest summary: a flat ~41 fps with the patch, against ~59 without it with
area-dependent dips. The profile now disables it.

The trade is image quality: the game now scales its 3D down under load, as it does on hardware.
That is a judgement call rather than a measurement, so the patch default is unchanged and this is
recorded for whoever makes it. Disabling it is per-game, in Properties -> Add-Ons.

**Worth noting against the rest of this document.** Every threading change measured here moved
waiting around without moving frames; the one change that moved frames was two defaults
contradicting each other. The per-thread cycle table said the system was latency-bound and
nothing was saturated -- and the GPU was at 79% in real gameplay against the 55-66% the headless
captures showed, so even that table was taken under lighter load than people actually play in.
