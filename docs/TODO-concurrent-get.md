# TODO: concurrent get() on the O_DIRECT backend trips an assertion

## Status

`uDepotSalsa<RuntimePosixODirect>::lookup_mbuff()` asserts when several OS
threads call `get()` at the same time through the pyudepot bindings. Reads are
therefore **single-threaded only** from Python today.

## Symptom

```
python: src/uDepot/udepot-lsa.cc:1025:
  trt::CoroTask udepot::uDepotSalsa<RT>::lookup_mbuff(
      u64, const udepot::Mbuff&, size_t, size_t, udepot::Mbuff&,
      udepot::HashEntry*, int*, size_t*)
  [with RT = udepot::RuntimePosixODirect]:
  Assertion `io_size_aligned == bytes && bytes == mb_dst.get_valid_size()' failed.
```

gdb backtrace, trimmed:

```
#5  __assert_fail_base (... "io_size_aligned == bytes && bytes == mb_dst.get_valid_size()",
                        "src/uDepot/udepot-lsa.cc", 1025, ...)
#7  udepot::uDepotSalsa<udepot::RuntimePosixODirect>::lookup_mbuff(...)
#8  uDepotGet ()
#9  ffi_call ...  (ctypes, from a Python worker thread)
```

## Reproduction

Any workload issuing concurrent `uDepotGet` calls on distinct keys against an
O_DIRECT store. From flywheel:

```bash
# Before multi_get was made serial; reproduced roughly 1 run in 3.
for i in 1 2 3; do python -m pytest tests/test_perf_regression.py -q; done
```

No concurrent writes are needed -- the store is read-only for the duration.
The keys are distinct, so this is not two threads racing on one entry.

## What is known

* The assertion is about I/O sizing, not memory corruption: the size actually
  read, the size expected, and the destination Mbuff's valid size disagree.
* It is timing-dependent. Single-threaded reads of the same keys always pass.
* The pyudepot bindings never call `uDepot::thread_local_init()`, so every
  Python thread runs with `ThreadId::get() == TLS_id_unitialized`
  (`UINT_MAX`). That is bounds-checked in `MbuffCache::mb_get()`, which sends
  all such threads to the shared, locked cache rather than a per-thread one,
  so it is not an out-of-bounds access -- but it does mean every caller thread
  is indistinguishable to the runtime, and `BRLock` maps them all onto the
  same `tid % LOCKS_NR` slot.

That last point is the most promising lead: it has not been confirmed as the
cause, and should not be treated as one without evidence.

## Why it was not caught earlier

flywheel's `TensorStore.multi_get` has issued concurrent gets since it was
written, but the tests covering it ran against an in-memory mock. The tests
that use the real library were skipped in CI because `libpyudepot.so` failed
to build there, so the concurrent path had never actually run under CI. Both
of those are fixed; this assertion is what the path does when it finally runs.

## Current mitigation

`TensorStore.multi_get` reads serially. The batching API is preserved so
callers do not change, but nothing in flywheel drives uDepot from more than
one thread. Restoring the concurrency is worth real throughput -- the design
calls for saturating NVMe queue depth per layer per token -- so this is a
genuine limitation, not a closed question.

## Fixing it

1. Reproduce under a debug build with the assertion left on, and capture the
   three disagreeing sizes.
2. Establish whether per-thread registration (`thread_local_init` /
   `thread_local_exit` from the bindings, with a thread-exit hook) changes the
   behaviour. If it does, the bindings need a proper thread lifecycle rather
   than a workaround at the Python layer.
3. Add a C++ test that issues concurrent gets against an O_DIRECT store, so
   the fix is covered where the bug lives rather than only from Python.
