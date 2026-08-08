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

Affected call sites — every one a plain non-coroutine function, so `co_await`
was not available to them as written:

| file | what was unprotected | fix |
|---|---|---|
| `src/include/uDepot/mbuff-cache.hh` (×3) | shared Mbuff cache + Mbuff allocation | `std::mutex` |
| `src/uDepot/lsa/udepot-directory-map.cc` | directory grow | `lock_blocking()` — the coroutine conversion was tried and reverted, see below |
| `src/uDepot/lsa/udepot-dir-map-or.cc` (×3) | directory grow / shadow alloc | `lock_blocking()`; experimental, now excluded from the build (`docs/TODO-dir-map-or.md`) |
| `test/uDepot/uDepotMapTest.cc` (×2) | the *multi-threaded* map tests, which were running unlocked | `lock_blocking()` (plain test threads) |

Why they differ is the whole subject of "What `lock_blocking()` actually is"
below: the right lock depends on whether the critical section can suspend, and
on whether the caller can afford to block a scheduler thread.

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

## Root cause: the C++20 coroutine migration

The call sites were correct when they were written. `git log --follow` on
`src/include/uDepot/sync.hh` shows exactly two commits: the initial import, and
`23f51de udepot: migrate to C++20 coroutine API`.

Before `23f51de`:

```cpp
virtual void lock() = 0;        // ordinary call; `lock->lock();` took the mutex
```

After:

```cpp
virtual trt::CoroTask lock() = 0;   // must be driven; `lock->lock();` does nothing
```

`TrtLock::lock()` needed to become a coroutine so it could `co_await
trt::T::yield()` instead of using the legacy jctx yield. Because the two locks
share a virtual base, `PthreadLock::lock()` was dragged along — even though it
never suspends and had no reason to be a coroutine.

The migration introduced `lock_blocking()` in the same commit precisely as the
escape hatch for non-coroutine callers (it did not exist before), and used it
correctly in `udepot-map.hh`. So the hazard was understood. What it missed were
the callers living in files the commit never opened:

| file | in `23f51de`? | left broken |
|---|---|---|
| `src/uDepot/udepot-lsa.cc` | yes (459 lines of `co_await`) | — |
| `src/include/uDepot/lsa/udepot-map.hh` | yes (`lock_blocking` added) | — |
| `src/include/uDepot/mbuff-cache.hh` | **no** | 3 sites |
| `src/uDepot/lsa/udepot-dir-map-or.cc` | **no** | 3 sites |
| `src/uDepot/lsa/udepot-directory-map.cc` | **no** | 1 site |
| `test/uDepot/uDepotMapTest.cc` | **no** | 2 sites |

**Why nothing caught it:** changing a function's return type from `void` to a
class type leaves `lock->lock();` valid C++. The call still compiles, builds a
temporary, and discards it. No warning, no error — under `-Wall -Werror`, in a
build that was otherwise clean. The signature change was silent at every call
site that did not need editing to keep compiling, which is precisely the set of
call sites that needed editing to keep *working*.

The general hazard, stated correctly: **moving from stackful to stackless
coroutines changes who must drive a suspending call, at every existing call
site, while changing the syntax at none of them.** The pre-migration `lock()`
was not eager — TRT had been doing coroutine suspension by hand with longjmp
for years — but suspension was *invisible to callers*, and stackless coroutines
make it every caller's problem. `[[nodiscard]]` on the coroutine type is the
mechanical defence, and is now in place — a full `-Werror` build is the proof
that no discarded `CoroTask` remains.

Residual gap: that proof covers the default and `BUILD_URING=1` builds.
`BUILD_SPDK=1` compiles additional translation units that were not compiled
here (no SPDK submodule in the container). Inspection of the SPDK-only sources
found only `std::mutex` locks, which are eager and unaffected, but that is grep
rather than the compiler. Anyone with SPDK checked out should build it once
with `[[nodiscard]]` in place to close this properly.

## What `lock_blocking()` actually is, and why it exists

`lock_blocking()` is not a serialization principle TRT was missing. It is an
artifact of going **stackless**, and it is worth being precise about that
because the earlier framing here ("eager function became lazy") was wrong.

Before `cfed697 trt: replace jctx fibers with C++20 coroutines`, TRT's yield
was:

```cpp
static void yield(void);      // plain call; switched stacks via jctx/longjmp
```

That is a **stackful** coroutine. Suspension is implemented by swapping the
whole stack, so a suspending function is called like any other function, at any
call depth, with no syntactic marking and no effect on its callers. One `lock()`
served everyone:

| | behaviour | caller wrote |
|---|---|---|
| `PthreadLock::lock()` | blocks the OS thread | `lock->lock();` |
| `TrtLock::lock()` | yields the fiber | `lock->lock();` |

C++20 coroutines are **stackless**: a coroutine suspends only its own frame, by
returning to its caller. Every caller in the chain must itself be a coroutine
and `co_await` — function coloring, and it is viral.

`run_sync()` is not a general escape hatch, because it is only valid for
coroutines that never truly suspend. That holds for `PthreadLock::lock()` and
fails for `TrtLock::lock()`, which really does `co_await trt::T::yield()`. With
no single implementation able to serve both colors of caller, the API had to
split, and `lock_blocking()` is the non-suspending half.

### `lock_blocking()` is not safe everywhere

```cpp
void lock_blocking() { for(;;) { if (trylock()==0) return; sched_yield(); } }
```

`sched_yield()` yields the **OS thread**, not the TRT task. If the lock holder
is another task on the same scheduler thread, it can only run when the current
task yields to the TRT scheduler — which spinning never does. Deadlock.

So `lock_blocking()` is correct only where blocking the thread is acceptable:
genuinely non-TRT threads, and critical sections that cannot themselves wait on
another task. Choosing it anywhere else trades a race for a hang.

### How each site was resolved

| site | resolution | why |
|---|---|---|
| `MbuffCache` (×3) | plain `std::mutex` | Section is one deque push/pop: no I/O, no suspension point, nothing for a suspending lock to buy. Using `RT::LockTy` here forced `lock_blocking()` (the methods are ordinary functions) and with `TrtLock` that is the deadlock above. A `std::mutex` removes the coloring question entirely. |
| `uDepotDirectoryMap<RT>::grow()` | still `lock_blocking()` | The `std::mutex` argument does **not** apply here: the critical section is long and itself waits on other tasks (`write_wait_readers`), so a thread-blocking wait can deadlock under TRT. The correct fix is to make `grow()` a coroutine and `co_await` — that was implemented and **hangs**, so it is reverted and documented in `docs/TODO-grow-race.md`. Taking this lock for real appears to expose a pre-existing re-entrancy, since it has not actually been held since the migration. |
| `uDepotDirMapOR` (×3) | left on `lock_blocking()` | Experimental; now excluded from the build, which does not compile anyway. `docs/TODO-dir-map-or.md`. |

### Known remaining hazards

1. `grow_lock_m` is taken with `lock_blocking()`, which is unsafe from a TRT
   task for exactly the reason above. Converting `grow()` to a coroutine is the
   right fix and currently hangs; see `docs/TODO-grow-race.md`.
2. `write_wait_readers()` polls with `std::this_thread::sleep_for(5ms)`. Inside
   a TRT task that blocks the scheduler thread, and the readers it waits for may
   be tasks on that same thread. Predates all of the above; needs an awaitable
   wait.

Both are confined to the TRT runtimes. The pyudepot path
(`RuntimePosixODirect`, `PthreadLock`) blocks a plain thread, which is correct
there — that is the path the measurements in this document cover.

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
