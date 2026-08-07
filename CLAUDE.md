# uDepot Development Guidelines

## Agent Rules

1. **Never ignore user instructions.** Every instruction the user gives must be addressed — either acted on or explicitly acknowledged with a reason if it cannot be done.
2. **Never gaslight the user.** Do not claim something was done when it was not, do not fabricate results, and do not dismiss or reframe a user's concern as already handled when it has not been.
3. **Fix repeating problems at the root.** When you hit a bug or build
   problem for the second time, do not just work around it again — change
   something so it cannot recur: a test that catches it, a default that makes
   it impossible, a check in the build, or a documented rule. A fix that only
   repairs the current instance is not finished.

## Commit Attribution

Commits are **authored by nik-io <nicioan@gmail.com>** and **co-authored by
Claude**. Claude is a co-author, not the author.

`.claude/settings.json` sets `GIT_AUTHOR_*` and `GIT_COMMITTER_*` for this, and
is checked in so it survives a fresh checkout or container. If it ever fails to
apply, set it explicitly rather than committing as Claude:

```bash
git config user.name  "nik-io"
git config user.email "nicioan@gmail.com"
```

Keep the `Co-Authored-By: Claude ...` trailer on Claude-assisted commits.

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

No baselines and no base-revision A/B builds. Throughput on cloud containers
drifts more than any regression worth catching — one benchmark here moved 30.4s
to 19.5s across five consecutive iterations on an idle machine. What survives
that is an invariant measured inside a single run, plus CI running it on every
push.

| layer | test | language |
|---|---|---|
| uDepot KV interfaces | `make run_perf_test` | C++ (`bench/io_layer_bench --compare`) |
| pyudepot bindings | `make run_pyudepot_build_test` | python (build test, no perf assertion) |
| raw I/O backends | `make -C trt run_perf_test` | C++ (report only, no assertion) |

`io_layer_bench --compare` runs the raw-buffer and Mbuff interfaces alternately
over one store and fails if the zero-copy path is not ahead. Alternating is
what makes it valid: drift affects both sides of a pair equally and cancels.

Tuning: `-n` (ops per phase), `-i` (paired iterations); the Makefile exposes
`PERF_OPS` (default 150000) and `PERF_ITERS` (default 3).

Expect this to be slow. The PUT phase has to become I/O bound before the
comparison means anything, so the op count cannot just be lowered to make CI
quick: at 150k ops and 3 iterations a single backend takes over 15 minutes on a
4-core machine. Both backends on a shared runner is over half an hour. That is
the cost of the invariant, not a bug.

Only ever assert on the *same* operation done two ways — zero-copy against
copying. Comparing different operations to each other (GET against PUT) asserts
something about the backend, the page cache and the device rather than about
the code, and flips with the environment. That is why the Python bindings get a
build test rather than a perf assertion.

The invariant is checked on every backend the benchmark supports (AIO and
io_uring). An invariant that only holds on one backend is not an invariant.

SPDK is **not** covered. `make -C trt run_spdk_bdev_test` and
`run_spdk_bdev_perf` run against a memory-backed bdev and restore the hugepages
they reserve, but they use SPDK's own event framework and never touch
`trt::SPDK`, `SpdkQpair`, or the TRT scheduler -- they are build and
environment smoke tests, not backend coverage. Testing the backend needs an
NVMe namespace (it uses the raw NVMe driver, not bdev), and the NVMe-oF route
that would provide one stalls after store init. See
`docs/TODO-spdk-testing.md`.

Notes:
- Keep `-n` at 200000 or higher. Below roughly 100k ops the PUT phase never
  becomes I/O bound and the comparison inverts at random.
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
