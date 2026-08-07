# uDepot Development Guidelines

## Agent Rules

1. **Never ignore user instructions.** Every instruction the user gives must be addressed — either acted on or explicitly acknowledged with a reason if it cannot be done.
2. **Never gaslight the user.** Do not claim something was done when it was not, do not fabricate results, and do not dismiss or reframe a user's concern as already handled when it has not been.

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

### Performance tests

Perf tests live with the layer they measure, and are written in that layer's
language. The C++ layers get shell drivers over the benchmark binaries; only
the Python bindings get a Python suite, because the bindings are what it
tests:

| layer | location | language |
|---|---|---|
| raw I/O backends (AIO, io_uring, SPDK) | `trt/bench/perf_test.sh` | shell |
| uDepot KV interfaces | `bench/perf_test.sh` | shell |
| pyudepot bindings | `bench/test_pyudepot_perf.py` | python |

Shared machinery is in `trt/bench/perflib.sh` — trt is the lowest layer, so
uDepot sources it without inverting the dependency direction.

Nothing is compared against a recorded baseline: throughput on cloud
containers drifts more than any regression worth catching. Every comparison
runs both sides alternately in one batch and reports the per-pair delta, which
cancels shared drift.

```bash
make -C trt run_perf_test      # raw I/O, A/B vs the base revision
make run_perf_test             # KV zero-copy invariant + A/B
make run_perf_zerocopy         # just the invariant (no base build)
make run_pyudepot_perf_test    # python bindings
```

Tuning: `PERF_ITERATIONS` (default 5), `PERF_THRESHOLD` (default 0.10),
`PERF_OPS`, `PERF_RUN_TIMEOUT`, `PERF_BASE_REF`, `UDEPOT_ROOT`.

Notes:
- `io_layer_bench` needs `-n 200000` or higher for the zero-copy comparison to
  be meaningful; below roughly 100k ops the PUT phase never becomes I/O bound
  and the result inverts at random.
- The io_uring backend currently aborts under any KV workload —
  `persist_seg_md()` issues a 64-byte O_DIRECT `pwritev` that fails EINVAL on
  alignment. Reproduces with plain `bin/udepot-test -u 6`, so it predates the
  perf work. The perf scripts probe each backend first and report this
  explicitly rather than letting it look like sample noise.

## Build

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
