# uDepot Development Guidelines

## Agent Rules

1. **Never ignore user instructions.** Every instruction the user gives must be addressed — either acted on or explicitly acknowledged with a reason if it cannot be done.
2. **Never gaslight the user.** Do not claim something was done when it was not, do not fabricate results, and do not dismiss or reframe a user's concern as already handled when it has not been.
3. **Fix repeating problems at the root.** When you hit a bug or build
   problem for the second time, do not just work around it again — change
   something so it cannot recur: a test that catches it, a default that makes
   it impossible, a check in the build, or a documented rule. A fix that only
   repairs the current instance is not finished.
4. **Never compromise a design choice to fix a bug.** The design properties in
   this file and in `docs/` are constraints on the fix, not variables the fix
   may spend. If the only way you can see to stop a crash or a data loss is to
   give up a documented property — reader concurrency, zero copy, the absence
   of a global lock, an amplification bound — that is a signal your fix is
   wrong, not that the property is negotiable. Look for one that holds the
   property.

   If you conclude the design choice itself is wrong, **stop and ask for
   explicit consent before changing it.** Say which property, what evidence
   makes you think it is wrong, and what the change costs. Wait for an answer.

   Shipping the compromise and describing it in a doc afterwards is not
   consent, and neither is a commit message. That is exactly what happened to
   the grow path: the reader drain traded away concurrency the design is built
   around, and this file went on to assert that the overlap "has been worth
   negative" — an agent's opinion, written down as if it were the project's
   position. If a stopgap genuinely cannot wait, say plainly that it is a
   stopgap, say what it costs, and ask.
5. **Keep responses focused, brief, and concise**. Keep disclaimers and caveats short, and spend most of the response on the main answer. When asked to explain something, give a high-level summary unless an in-depth explanation is specifically requested.
6. **Before your first tool call, say in one sentence what you're about to do**. While working, give a brief update only when you find something important or change direction. When you finish, lead with the outcome: your first sentence should answer "what happened" or "what did you find," with supporting detail after it for readers who want it.
7. **Deliver what was asked, at the scope intended**. Make routine judgment calls yourself, and check in only when different readings of the request would lead to materially different work. If the request seems mistaken or a better approach exists, say so in a sentence and continue with the task as asked rather than quietly narrowing, widening, or transforming it. Finish the whole task, and stop short of actions that are clearly beyond what was asked.
8. **Delegate to a subagent only for large tasks that are genuinely independent and parallelizable, such as a wide multi-file investigation.** Do not delegate work you can finish yourself in a handful of tool calls, and do not use subagents to verify or double-check your own work. If one subagent can complete the task, use one rather than several, and keep spawn counts low.


## Project Overview

uDepot is a high-performance key-value store for NVMe storage, built on a coroutine-based task runtime (TRT). It supports multiple I/O backends (POSIX, O_DIRECT, SPDK, io_uring) and NVMe over Fabrics (TCP/RDMA). Reference paper: https://www.usenix.org/system/files/fast19-kourtis.pdf

## Design Principles

These are foundational constraints. Every change must preserve them.

1. **Zero copy**: Maintain the mbuff APIs for zero-copy data paths. Never introduce unnecessary copies between user buffers and storage. DMA buffers (via `rte_malloc`) are acceptable only at the SPDK boundary where hardware requires them.

2. **No global locking**: Avoid global locks completely. Minimize contention when locking is necessary — prefer per-thread, per-queue, or lock-free structures. The SPDK I/O path uses per-thread queue pairs (`SpdkQpair`) with no shared state.

3. **Minimal amplification**: Minimize read amplification (bytes read vs bytes used) and write amplification. Do not add indirection layers, journaling, or metadata overhead without explicit justification.

4. **Enterprise-grade crash recovery only**: Support crash recovery at the level required for enterprise storage. Do not add casual or partial durability guarantees — either the recovery path is correct and complete, or it should not exist.

## Architecture

- **TRT (Task Runtime)**: Coroutine-based cooperative scheduler in `trt/`. All task functions return `trt::CoroTask`. Use `co_await`/`co_return` — never legacy jctx yield.
- **Never discard a `trt::CoroTask`.** `initial_suspend()` is `suspend_always`, so calling a `CoroTask` function only builds the frame — the body does not run until something resumes it. A discarded `CoroTask` is a silent no-op plus a leaked frame, not a completed call. `CoroTask` is `[[nodiscard]]` and the build uses `-Werror`, so this is now a compile error; do not silence it with a cast to `void`. This is not hypothetical: `uDepotLock::lock()` returns `CoroTask`, and six call sites discarded it, leaving the shared Mbuff cache and the directory-grow path completely unlocked (`docs/concurrent-get-fix.md`). **From a non-coroutine, call `lock_blocking()`; from a coroutine, `co_await lock()`.**
- **Never split the directory map's identity across two variables.** A lookup
  picks its table from `dir_ref_m.directory` *and* the number of index bits.
  Those were separate fields, published by `grow()` on either side of
  `write_exit()`, and the window between them silently routed puts to the wrong
  table — `ENODATA` for keys that were written, with the index still in bounds
  so nothing asserted (`docs/TODO-grow-race.md`). `hash_to_map()` now derives
  the width from the directory it just loaded. Anything else `grow()` publishes
  belongs inside the write lock, in one place. This is a prerequisite for the
  design below, not just a bug fix: a directory snapshot has to describe its
  own geometry before a reader can be allowed to outlive the swap.
- **The grow path is a stopgap, and knowingly so.** The design is: `grow()` is
  single-writer (`grow_lock_m` — working as intended); the old and new tables
  *coexist* while references to the old one are outstanding, with the old one
  reclaimed lazily; and readers are blocked as little as possible. The code
  does none of the last two — `grow()` unmaps the old tables inline, which is
  why the write lock has to span the whole operation, and draining readers
  before `mprotect` was the only way to stop a crash once the page-fault
  rollback became unusable under coroutines. Deferred reclaim of retired
  directories comes first (`docs/TODO-deferred-reclaim.md`), then gradual
  per-table growth (the uDepot paper's improvement; `uDepotDirMapOR`'s shadow
  directory is the started half). Do not "simplify" the grow path on the
  assumption that the current stall is intended. See `docs/TODO-grow-race.md`,
  "What the design intends".
- **Segment geometry — one layout, three consumers.** The device is divided
  into fixed-size **segments** of `get_seg_size()` grains. Segments tile from
  the device start on a `segment_bytes = get_seg_size()*grain` grid, so segment
  N sits at device offset `N * segment_bytes`. The **last `get_seg_md_size()`
  grain(s) of every segment** hold salsa's per-segment metadata; what remains is
  the **net segment** (`seg_size = get_seg_size() - get_seg_md_size()` grains),
  used for either data or a directory table. A directory table lays out
  `[dirmap_hdr | hash table | dirmap_ftr]` inside the net segment, with the
  512-byte footer at the very end (`seg_size*grain - sizeof(dirmap_ftr)`);
  `restore()` reads it back with a plain `pread` at that offset, independent of
  how the table was mapped. Separately, one page of **device-level** salsa
  metadata lives in the tail past the last whole segment
  (`align_down(device, segment_bytes)`) — which is why the device size must not
  be an exact multiple of the segment size (an exact multiple leaves no tail →
  `check_dev_size` fails; hence the 513-not-512 MiB bdev and the `+1` in file
  sizes). A segment too large for a small device is silently right-sized down by
  `uDepotSalsa::init()` so GC keeps its minimum spare-segment count.
- **Hugepage invariant for directory tables (and the shutdown OOB it explains).**
  Because segments tile a `segment_bytes` grid, a segment whose **full size
  (`get_seg_size()*grain`) is a 2 MiB multiple** both sits at a 2 MiB-aligned
  device offset *and* has a 2 MiB-aligned length — so the **whole segment** is a
  `MAP_HUGETLB`-mappable unit. The correct design maps the full segment
  (`get_seg_size()*grain`, metadata grain included) and writes the directory's
  header/table/footer at their normal offsets inside it — footer still at
  `seg_size*grain - sizeof(dirmap_ftr)` — leaving the per-segment salsa metadata
  grain(s) at the tail `[seg_size*grain, get_seg_size()*grain)` untouched (the
  writeback must stop at the footer, not clobber them). No `align_down`, no
  separate footer I/O: the metadata just lives past the footer, still inside the
  mapping. The footer's on-disk offset is fixed by geometry and never moves. The
  bug that motivated this: `grow()` instead mapped only the **net** region and,
  for huge pages, `align_down(seg_size*grain, 2 MiB)` — smaller than the net
  region — yet still wrote the footer at `seg_size*grain - sizeof(ftr)`, past the
  mapping's end: an intermittent shutdown OOB (only the SPDK path reserves
  hugepages, so tmpfs backends never hit it). This is now **fixed as described
  above**: `grow()` and `restore()` always `mmap` the whole segment
  (`get_seg_size()*grain`), taking `MAP_HUGETLB` when that size is a 2 MiB
  multiple and falling back to 4 KiB pages otherwise — conditional, not enforced,
  so the segment size stays the user's choice (the default 16 MiB segment gets
  huge pages). The mapping length is therefore always the full segment and is
  used only for `mprotect()`/`munmap()`; `size_b` stays the net region and drives
  table sizing, footer offset, grain invalidation and the writeback, which stops
  at the footer (`invalidate_ftr()` bounds its `msync` there) so the metadata
  tail is never clobbered. Verified by strace (a real `MAP_HUGETLB` mmap of the
  16 MiB segment succeeds) plus `run_spdk_nvmef_test`; the earlier stopgap — huge
  only when the *net* region was itself 2 MiB-aligned, so 4 KiB in practice — is
  gone. History in `docs/TODO-spdk-testing.md`.
- **I/O backends**: Located in `src/uDepot/io/`. Each backend implements `uDepotIO_` interface. SPDK backends require DMA-safe buffers for NVMe commands.
- **SpdkQpair**: Per-thread NVMe queue pair. `read_sync`/`write_sync` handle DMA buffer allocation internally. `read_raw_sync`/`write_raw_sync` expect pre-allocated DMA buffers.
- **Python bindings**: `pyudepot` via ctypes in `src/uDepot/net/py-udepot.cc`. Shared library built as `libpyudepot.so`.

## Style Guide

Follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with these project-specific conventions:

- C++20 standard (`-std=c++20`)
- Member variables use `_m` suffix (e.g., `pread_iofn_m`, `mmap_off_m`)
- Use `snake_case` for functions and variables, `PascalCase` for types and classes
- Prefer `#pragma once` for new headers; existing code uses include guards — match the surrounding file's convention
- No exceptions in the I/O hot path

## Testing

- Unit tests live in `trt/src/tests/` (TRT-level) and alongside source files for uDepot-level tests
- Every new public API method must have a corresponding test
- Test both success and error paths — especially for I/O operations where partial reads/writes and DMA allocation failures are realistic
- SPDK tests require `BUILD_SPDK=1` and a checked-out SPDK v24.09 submodule
- Non-SPDK tests must always pass: `make clean && make` with default flags
- Do not merge code that breaks the non-SPDK build

`test/rwlock-pagefault/resizable_table` covers `rwlock_pagefault`, the
page-fault rollback the grow path used to depend on — `grow()` drains readers
instead now, but `rd_execute__` is still the fallback if the stall ever needs
removing. It resizes an mmap'd table under concurrent readers in both orderings
uDepot has used, and asserts a rollback actually fired — an earlier version
relied on timing luck and reported zero rollbacks, passing while exercising
nothing. See `docs/TODO-grow-race.md`.

**A failing test must fail the build.** `do_run_test` used to print `FAILURE.`
and then exit 0, so `make run_tests` — which CI runs — reported success while
`bin/udepot-test` segfaulted on *every* run of `udepot-grow-test`. It now
propagates the exit status. If a test is genuinely known-broken, quarantine it
explicitly with `do_run_known_failing_test` and a tracking document so it stays
visible; never make failure silent for every test to accommodate one.
`udepot-grow-test` was its one user and is un-quarantined now that the grow
race is fixed, so the macro currently has none — keep it anyway.

### Performance tests

No baselines and no base-revision A/B builds. Throughput on cloud containers
drifts more than any regression worth catching — one benchmark here moved 30.4s
to 19.5s across five consecutive iterations on an idle machine. What survives
that is an invariant measured inside a single run, plus CI running it on every
push.

| layer | test | language |
|---|---|---|
| uDepot KV interfaces | `make run_perf_test` | `scripts/perf-zerocopy.sh` driving `udepot-test` |
| pyudepot bindings | `make run_pyudepot_build_test` | python (build test, no perf assertion) |
| raw I/O backends | `make -C trt run_perf_test` | C++ (report only, no assertion) |

`run_perf_test` runs `scripts/perf-zerocopy.sh` for each backend (5=aio,
6=io_uring). The script runs `udepot-test` in copy (`--thin`) and zero-copy
(`--thin --zero-copy`) modes, interleaved, and fails if the zero-copy median is
slower than the copy median on **either PUT or GET**. Interleaving cancels the
slow throughput drift a shared runner has.

The store is on `/dev/shm` (RAM-backed, buffered), which is load-bearing: it
makes both PUT and GET **cache-bound**, so the value memcpy the zero-copy path
avoids is a real, consistent win (PUT ~+3-7%, GET ~+5-13%). On a real O_DIRECT
device the ops are I/O bound and that ~2% saving is below device noise, so the
delta flips sign run to run -- which is exactly why an earlier version, gating on
GET over a normal device, failed on the uring backend. A small tolerance (the
script's 4th arg, default 5%) absorbs residual per-pair noise; a real zero-copy
regression is far larger. (CI used to gate on a separate `bench/io_layer_bench`
that needed 150k+ ops and timed out; it is still buildable but no longer used.)

Tuning via the Makefile: `PERF_OPS` (default 10000) and `PERF_ITERS` (default 9,
the number of interleaved copy/zero-copy pairs). Raise them only for a deeper
local characterisation.

Only ever assert on the *same* operation done two ways — zero-copy against
copying. Comparing different operations to each other (GET against PUT) asserts
something about the backend, the page cache and the device rather than about
the code, and flips with the environment. That is why the Python bindings get a
build test rather than a perf assertion.

The invariant is checked on every backend the benchmark supports (AIO and
io_uring). An invariant that only holds on one backend is not an invariant.

The SPDK backend is covered by `make BUILD_SPDK=1 run_spdk_nvmef_test`
(`scripts/spdk-nvmef-test.sh`): it starts a loopback SPDK `nvmf_tgt` exporting a
RAM-backed malloc bdev as an NVMe namespace over TCP and runs `udepot-test`'s
SPDK backend (`-u 7`) against it as a fabrics initiator, so it drives
`trt::SPDK`, `SpdkQpair` and the TRT scheduler on the I/O path. The backend
learns the target from the `UDEPOT_NVMEF` env var, so `udepot-test` needs no new
flag. This is separate from `make -C trt run_spdk_bdev_test` /
`run_spdk_bdev_perf`, which use SPDK's own event framework and never touch the
backend -- they are build/environment smoke tests, not backend coverage. Getting
the nvmef route working exposed three latent bugs (the completion poller was
spawned with a discarded awaitable and never ran; the admin queue was never
polled so fabrics keep-alives timed out; the directory footer overran a
huge-page mapping at shutdown); see `docs/TODO-spdk-testing.md`. The zero-copy
perf invariant over SPDK is still not asserted in a Makefile target.

Notes:
- Both PUT and GET are gated, and hold at a few thousand ops -- but only because
  the store is on `/dev/shm`. On a real O_DIRECT device both are I/O bound and
  invert at random unless `-n` is very high (150k+), which is exactly the timeout
  this replaced. Keep the store on tmpfs.
- **Grain size must be at least the device sector size on O_DIRECT backends.**
  uDepot sizes its segment metadata writes in grains, so a 32-byte grain makes
  those writes 64 bytes, which `pwritev` rejects with EINVAL under O_DIRECT.
  The benchmark defaults to 512 (overridable with `--grain-size`), matching
  what the Makefile's own TRT tests use. The 32-byte-grain tests in this
  Makefile all run against `/dev/shm`, which is buffered and has no such
  constraint.

## Build

Objects depend on a stamp holding the build flags (`BUILD_SPDK`, `BUILD_URING`,
...), so changing flags forces a rebuild rather than silently reusing objects
compiled under different ones. That is necessary — `BUILD_SPDK` decides whether
whole template instantiations exist — but it means a flag flip is a full
rebuild.

**Install ccache.** The Makefiles pick it up automatically when present, and it
turns the flip back to a previously built configuration into cache hits.
Measured here:

| | time |
|---|---|
| flip to `BUILD_SPDK=1` (cold cache) | 156s |
| flip back to non-SPDK (warm) | 1s |
| flip to `BUILD_SPDK=1` again (warm) | 2s |

`NO_CCACHE=1` opts out.

```bash
# Default (non-SPDK)
make

# With SPDK
make BUILD_SPDK=1

# With io_uring
make BUILD_URING=1

# TRT only
cd trt && make
```
