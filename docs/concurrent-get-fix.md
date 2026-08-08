# Concurrent get() on the O_DIRECT backend — FIXED

## Status

**Fixed.** `uDepotLock::lock()` is a coroutine, and six call sites called it as
if it were an ordinary function, discarding the returned `trt::CoroTask`. The
lock was never taken at any of them. The most damaging one left the shared
Mbuff cache unlocked, so concurrent readers were handed the *same* `Mbuff`.

`trt::CoroTask` is now `[[nodiscard]]`, which makes the mistake a compile
error rather than a silent no-op.

## The bug

`trt::CoroTask::promise_type::initial_suspend()` returns `std::suspend_always`.
Calling a function that returns `CoroTask` therefore only builds the coroutine
frame and suspends — **none of the body runs** until something resumes it.

`uDepotLock::lock()` returns `CoroTask`:

```cpp
trt::CoroTask lock() override {
        int ret = pthread_mutex_lock(&lock_m);   // never runs when discarded
        ...
        co_return 0;
}
```

So `lock->lock();` acquires nothing and leaks a frame. Only `co_await
lock->lock()` (from a coroutine) or `lock->lock_blocking()` (from an ordinary
function) actually locks.

Affected call sites, all now using `lock_blocking()` — every one is a plain
non-coroutine function, so `co_await` was not an option:

| file | what was unprotected |
|---|---|
| `src/include/uDepot/mbuff-cache.hh` (×3) | shared Mbuff cache + Mbuff allocation |
| `src/uDepot/lsa/udepot-dir-map-or.cc` (×3) | directory grow / shadow alloc |
| `src/uDepot/lsa/udepot-directory-map.cc` | directory grow |
| `test/uDepot/uDepotMapTest.cc` (×2) | the *multi-threaded* map tests, which were running unlocked |

## Why it presented as an I/O sizing assertion

```
src/uDepot/udepot-lsa.cc:1025:
  Assertion `io_size_aligned == bytes && bytes == mb_dst.get_valid_size()' failed.
```

`MbuffCache::mb_get()` pops from a `std::deque<Mbuff *>` under the (missing)
lock. Two threads racing there receive the same `Mbuff *`. One thread's
`reslice(0)` then resets the buffer another thread has just read into, so the
read reports N bytes while the mbuff reports something else.

Instrumenting the assertion showed exactly that:

```
CGDBG tid=4294967295 io_size_aligned=8192 bytes=8192 valid=0    free=8192 ...
CGDBG tid=4294967295 io_size_aligned=8192 bytes=8192 valid=22   free=0    ...
```

`valid=0` after an 8192-byte read is another thread's `reslice(0)`. `valid=22`
is a *key* length — the test's keys are 21–22 bytes — i.e. one thread was
appending a key into the very Mbuff another was using as the value
destination.

Wrong answers came with it: `get()` returned `ENODATA` for keys that existed.
That is worse than the crash, and on a build with `NDEBUG` (no assertion) it
is all that would be left.

## Why it was never hit before

uDepot's threaded deployments are not affected, and the code really has been
correct in them. `MbuffCache::mb_get()` picks a cache by thread id:

```cpp
unsigned tid = ThreadId::get();
if (tid < mbuff_thread_caches_m.size()) {
        cache = &mbuff_thread_caches_m[tid];   // private, no lock needed
        lock = nullptr;
} else {
        cache = &mbuff_shared_cache_m;         // shared, lock required
        lock = &mbuff_shared_cache_lock_m;
}
```

Threads that register via `ThreadId::allocate()` get a **private, lock-free**
cache, and the broken lock is never consulted — `lock` is `nullptr`. The
shared path is a fallback for unregistered threads, and the pyudepot bindings
never register, so every Python thread had `tid == UINT_MAX` and landed on the
one path whose locking did not work.

That is why this survived: the defect is in a fallback that the proven
deployments never take.

## Reproduction

`test/uDepot/concurrent-get-test` populates an O_DIRECT store and reads it back
from several unregistered pthreads. No Python involved — the original report
came through the bindings, but the bug is entirely in C++.

Before the fix it aborted in roughly half of runs (and printed wrong-answer
`ENODATA` lines before that). After, 18/18 clean. Driving the same load through
the bindings: 4/4 runs aborted before, 3/3 clean after.

It runs as part of `make run_tests`.

## Remaining work

`ThreadId` registration from the bindings is still worth doing, but as an
optimisation rather than a correctness fix: registered threads get the
lock-free private cache instead of contending on the shared one. `flywheel`'s
`TensorStore.multi_get` can now be made concurrent again.
