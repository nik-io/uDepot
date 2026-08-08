# Concurrent directory-map grow: crash fixed, data loss still open

## Status

**Partly fixed. Still quarantined.** The SIGSEGV is fixed (root cause below).
`udepot-grow-test` now fails a different way — a `get()` returns `ENODATA` for
a key that was written — so it stays quarantined via
`do_run_known_failing_test`. It is not flaky; it fails every run.

| | before | after the drain fix |
|---|---|---|
| `-t 17 --thin` | SIGSEGV (139), 3/3 | no segfault, 4/4 |
| `-t 23 --thin` | SIGSEGV/abort | no segfault, 4/4 |
| both | — | abort (134): `get returned 61` |

**Whether the remaining `ENODATA` is pre-existing or was introduced by the
drain change is not established.** The segfault previously killed the run
before the read-back could happen, so there was no opportunity to observe it.
Do not assume either way without measuring.

## Root cause of the crash: the coroutine migration dropped the rollback

`rwlock_pagefault` is not an ordinary rwlock. Its read side is protected by a
SIGSEGV handler plus `sigsetjmp`/`siglongjmp`: during a grow the old table is
`mprotect`ed `PROT_READ` and then `PROT_NONE`, and an in-flight reader that
touches it faults, longjmps out, and **retries the whole operation**. The
header says so, and points at an example test that has never existed in this
repo:

```
 * The code here implements (1) by mapping the table RO during the copy, and
 * installing SIGSEGV handlers that will rollback operations in case of a page
 * fault.
 * see test/rwlock-pagefault/resizable_table for an example
```

The rollback lived in `rwlock_pagefault::rd_execute__`, which is the only thing
that ever set `rwlpf_rb__.rb_set = 1`. Before the C++20 migration,
`uDepotSalsa::local_op_execute` drove every operation through it, passing
`prepare`/`rollback`/`finalize` handlers.

`23f51de udepot: migrate to C++20 coroutine API` replaced that with a bare
pair:

```cpp
rwlpf->rd_enter();
uDepotMap<RT> *udm = this->map_m.hash_to_map(h);
co_await udm->lock(h);
trt::RetT ret = co_await op(this, std::forward<Args>(a)...);
udm->unlock(h);
rwlpf->rd_exit();
```

It had to: a `sigsetjmp` checkpoint cannot survive a `co_await`. The coroutine
pops the stack the checkpoint refers to, and `rd_execute__`'s own comment
already spelled out that constraint — *"we can only push into the stack, never
pop, because our checkpoint (that includes a point in the stack) might become
invalid."* Coroutines violate it by construction.

`rd_execute__` has had **no callers** since. So `rwlpf_rb__.rb_set` is
permanently 0, and `sigsegv_handler` always takes the branch that chains to the
previous handler:

```cpp
if (rwlpf_rb__.rb_set != 1)
        return (oldact_g.sa_sigaction(sig, siginfo, uctx));   // -> crash
siglongjmp(rwlpf_rb__.rb_jmp, 1);                             // never reached
```

The fault stopped being recoverable and became a crash. Nothing warned, because
removing the last caller of a template is not an error.

## The fix applied

Readers are now drained *before* the protection bits change, so no reader is
ever inside a table that is `PROT_READ`/`PROT_NONE` and no fault can occur.

This matters because `RWLock::wr_enter()` is **non-blocking** — it subtracts
`RWLOCK_BIAS`, which makes `rd_try_lock()` fail for *new* readers, and returns
immediately. In-flight readers were still inside when `mprotect` ran. That is
precisely the window the rollback used to cover. `write_wait_readers()` polls
`wr_ready()` (`rwlock_ == 0`) until they leave.

Cost: the copy no longer overlaps with readers, so a grow is now a full stall.
That overlap is what the page-fault design bought, and it has been worth
negative since the migration — it crashed instead. Neither `grow()` call site
runs while holding the read lock, so draining cannot deadlock.

## Still open: `ENODATA` after a grow

With the crash gone, both invocations now fail here instead:

```
test/uDepot/udepot-test.cc:302: get_test_thin() get returned 61 vale=... valret=...
udepot-test: test/uDepot/udepot-test.cc:303: get_test_thin: Assertion `0' failed.
```

61 is `ENODATA`: a key the test wrote is not found on read-back. That is a
correctness bug, not a crash, and it is the reason the test stays quarantined.

Ruled out so far:

- **Not the GC relocation path.** `uDepotSalsa::gc_callback` does take
  `rwpflock.rd_enter()` around its `local_gc_callback` mutation
  (`udepot-lsa.cc:721-727`), so it is not mutating a table behind the grow's
  back.
- **Not the discarded-lock family.** Those are fixed
  (`docs/concurrent-get-fix.md`) and a clean `-Werror` build with
  `[[nodiscard]]` proves no discarded `CoroTask` remains.

Worth trying next, roughly in order of cost:

- Establish whether it predates the drain change. Hard to do directly, since
  the segfault used to kill the run first; a build with the drain plus the
  `mprotect` calls removed would at least separate "protection mechanics" from
  "copy/lookup logic".
- Audit the copy loop in `uDepotDirectoryMap<RT>::grow()` for entries that are
  live in the old table but not carried into the new one — particularly
  `try_shift`ed entries and tombstones (`deleted()`), whose invariants differ
  from ordinary used entries.
- Check the retry path in `put_mbuff`: on `ENOSPC` it calls `grow()` and
  re-runs with `EAGAIN`. Confirm the retry re-derives `udm` from the *new*
  directory rather than reusing anything cached from before the grow.

## Why the segfault was never caught by CI

`do_run_test` printed `FAILURE.` and then exited 0, so `make run_tests` — which
CI runs — reported success while this segfaulted on every run. That is fixed:
failures now propagate, and this test is quarantined explicitly rather than
being swallowed along with everything else.

## The missing rwlock_pagefault test

`make run_tests` invoked `test/rwlock-pagefault/resizable_table` — the very
example the `rwlock-pagefault.hh` header points at. That binary has no source,
no build rule, and no history in this repo: the line came in with the initial
import and the test never did. Because `do_run_test` swallowed failures, the
command-not-found reported as a silent non-failure for the life of the repo.

So the component whose rollback the migration removed has never had a test.
Writing one — a resizable table hammered by concurrent readers across a resize
— would have caught this regression at the commit that introduced it, and is
still the best way to pin down what remains.

## Reproducing

```
bin/udepot-test -f /dev/shm/udepot-test --segment-size 4096 \
    --size 1077936129 -w 100000 -r 100000 -t 17 --thin \
    --force-destroy --grain-size 32 --val-size 3072
```

No special hardware. `-t 17` and `--thin` matter: the thin store is what forces
the grow, and the concurrency is what makes readers be in flight during it. The
second invocation in `udepot-grow-test` reads the store the first leaves
behind, so its failure is downstream of the first and not independent evidence.
