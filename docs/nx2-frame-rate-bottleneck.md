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

The experiment is not in the tree. It cannot pay for itself, and a release that writes outside
the range it declares would be read past, which shows up as rare corruption rather than a crash.

## What to look at instead

`GPUBack` at 71% is the busiest thread, and the front idles waiting for it. The question is what
the back spends its time on and why the host GPU still sees only 66% at 62 W: whether its
submissions are too small, too serialised, or waiting on fences of their own. Measuring the back
the way `Nx2Profile` measures the front is the next step, and there is no zone breakdown for it
yet.
