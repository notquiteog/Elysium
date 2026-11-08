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

Two things stand out. **"Other" is 405 ms/s**, two fifths of the thread, and is by definition
everything outside the named zones -- there is no finer breakdown of it, and it is larger than
any measured zone. **Texture upload is 159.5 ms/s across 417 uploads**, 0.38 ms each, which is a
sixth of the thread for a rate that is small beside the 40,323 draws.

So the next step is not another dependency to narrow. It is to split "other" into zones, since
nothing can be said about two fifths of the bottleneck thread while it has no name, and to look
at why an upload costs 0.38 ms.
