# Concurrent directory-map grow: fixed

## Status

**Fixed and un-quarantined.** `udepot-grow-test` runs under `do_run_test` in
`make run_tests` again. Two independent bugs had to go:

1. the C++20 coroutine migration dropped the page-fault rollback the grow path
   depended on — that was the SIGSEGV;
2. `grow()` published the new directory and its index width as two separate
   stores, one inside the write lock and one outside — that was the `ENODATA`.

| | at the start | after the drain fix | after the publish fix |
|---|---|---|---|
| `-t 17 --thin` | SIGSEGV (139), 3/3 | abort (134): `get returned 61` | pass, 10/10 |
| `-t 23 --thin` | SIGSEGV/abort | abort (134) | pass, 10/10 |

The document below keeps both diagnoses, because both mechanisms are things
this codebase can grow again.

## Bug 1: the coroutine migration dropped the rollback

`rwlock_pagefault` is not an ordinary rwlock. Its read side is protected by a
SIGSEGV handler plus `sigsetjmp`/`siglongjmp`: during a grow the old table is
`mprotect`ed `PROT_READ` and then `PROT_NONE`, and an in-flight reader that
touches it faults, longjmps out, and **retries the whole operation**. The
header says so, and pointed at an example test that had no source in this repo:

```
 * The code here implements (1) by mapping the table RO during the copy, and
 * installing SIGSEGV handlers that will rollback operations in case of a page
 * fault.
 * see test/rwlock-pagefault/resizable_table for an example
```

The rollback lived in `rwlock_pagefault::rd_execute__`, the only thing that
ever set `rwlpf_rb__.rb_set = 1`. Before the C++20 migration,
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

`rd_execute__` has had **no callers** since, so `rwlpf_rb__.rb_set` is
permanently 0 and `sigsegv_handler` always takes the branch that chains to the
previous handler:

```cpp
if (rwlpf_rb__.rb_set != 1)
        return (oldact_g.sa_sigaction(sig, siginfo, uctx));   // -> crash
siglongjmp(rwlpf_rb__.rb_jmp, 1);                             // never reached
```

The fault stopped being recoverable and became a crash. Nothing warned, because
removing the last caller of a template is not an error.

### The fix — and it is a deliberate deviation from the design

Readers are drained *before* the protection bits change, so no reader is ever
inside a table that is `PROT_READ`/`PROT_NONE` and no fault can occur.

This matters because `RWLock::wr_enter()` is **non-blocking** — it subtracts
`RWLOCK_BIAS`, which makes `rd_try_lock()` fail for *new* readers, and returns
immediately. In-flight readers were still inside when `mprotect` ran. That is
precisely the window the rollback used to cover. `write_wait_readers()` polls
`wr_ready()` (`rwlock_ == 0`) until they leave. Neither `grow()` call site runs
while holding the read lock, so draining cannot deadlock.

**This trades away reader concurrency that the design intends to keep.** See
"What the design intends" below before treating it as settled. It is a stopgap
that stops a crash; it is not the end state.

Be precise about what it costs, though, because it is less than it looks and
the intent was already not being met:

`write_enter()` sits before the copy in both the original and current code, and
it makes `rd_try_lock()` fail for every *new* reader, which then futex-waits in
`rd_enter()`. So new readers were blocked for the whole copy either way. The
late `write_wait_readers()` bought overlap only for readers **already in
flight** at the instant `write_enter()` ran. The drain removed that narrow
overlap. It did not introduce the reader blocking — `write_enter()` did.

## What the design intends, and how far the code is from it

Recorded from the author, because none of it is visible in the code and two of
the three were being contradicted by comments in this file.

1. **`grow()` is single-writer by design.** `grow_lock_m`, plus the
   `old_dir != dir_ref_m.directory.load()` recheck that turns a lost race into
   `EAGAIN`, is that guard. Working as intended — not something to redesign.
   What the coroutine experiment below found is narrower than it first read:
   making the guard *real* (it was a discarded `CoroTask`, so the lock had
   never been held) surfaced a re-entrancy in the grow logic that had been
   harmless while the lock did nothing.
2. **The old and new tables are meant to coexist** while references to the old
   one are still outstanding, with the old one reclaimed lazily.
3. **Blocking readers as little as possible is the point**, which is why the
   original code waits for readers late rather than at `write_enter()`.

### Point 2 is not implemented at all

`grow()` destroys the old tables *inside* the write lock, before the swap:

```cpp
mprotect(dme.mm_region, dme.size_b, PROT_NONE);
udepot_io_m.munmap(dme.mm_region, dme.size_b);   // old tables gone here
salsa::SalsaCtlr::invalidate_grains(...);
dir_ref_m.directory = new_dir;                   // swap only after
...
delete old_dir;
```

There is no grace period, no reference count, no deferred reclaim. The two
tables never coexist and nothing is freed lazily. **This predates all the
recent lock and grow work** — it is the original shape, in both
`uDepotDirectoryMap` and `uDepotDirMapOR`.

It is also *why* the write lock has to cover the whole operation: with the old
mapping being unmapped inline, the lock is the only thing keeping readers off
memory that is about to disappear. Reader concurrency cannot be restored while
this stands, whatever the lock does.

### What restoring the intent takes

The hard part is writers, not readers.

- **Readers** on an old-directory snapshot need no protection at all, only
  somewhere for the old mapping to live until they leave. `mprotect` +
  rollback existed to catch **writes** to the old table after the copy had
  passed that slot — the header is explicit that reads are the easy half and
  writes "would need some kind of log".
- The rollback cannot come back on the coroutine path: `sigsetjmp` cannot span
  a `co_await` (see bug 1).

So, in order:

1. **Deferred reclaim of retired directories** (epoch- or RCU-style). Retire
   instead of `munmap`; free once no reader can still hold the pointer. This is
   point 2, and it is what lets readers leave the write lock entirely. No
   faults, so no rollback needed.
2. **Gradual growth** — copy one old table at a time, so only that table's
   writers stall, and only for one table's copy. This is the improvement
   discussed in the uDepot paper; `uDepotDirMapOR`'s shadow directory
   (`alloc_shadow()`) is the partially-implemented piece. See
   `docs/TODO-dir-map-or.md` — it currently does not compile.

Ordering matters: gradual growth without deferred reclaim still cannot let the
two tables coexist, so it would not remove the stall on its own.

The bug-2 fix below is a **prerequisite** for step 1, not a conflict with it.
Once a reader holds a snapshot of the old directory while the new one is
published, an index width kept in a separate `grow_nr_m` is read against the
wrong directory by construction — the same wrong-table bug, permanently rather
than in a window. Deriving the width from the snapshot pointer is what makes a
snapshot self-describing.

## Bug 2: the new directory and its index width were published separately

With the crash gone, both invocations failed here instead:

```
test/uDepot/udepot-test.cc:302: get_test_thin() get returned 61 vale=... valret=...
udepot-test: test/uDepot/udepot-test.cc:303: get_test_thin: Assertion `0' failed.
```

61 is `ENODATA`: a key the test wrote is not found on read-back.

`hash_to_map()` read two independently-updated fields and needed them to agree:

```cpp
std::vector<DirMapEntry> *const directory = dir_ref_m.directory.load(...);
const u64 idx = hash_to_dir_idx_(h, grow_nr_m - 1);
return &((*directory)[idx]).map;
```

`grow()` stored them in two places:

```cpp
dir_ref_m.directory = new_dir;      // inside the write lock
dir_ref_m.rwpflock.write_exit();    // readers resume here
// ... dirmap header writes ...
grow_nr_m++;                        // outside
```

Between `write_exit()` and `grow_nr_m++`, readers ran against the new — twice
as large — directory while still using the *old* index width. Every key whose
correct new index is `>= old_dir->size()` hashed one bit short. A put in that
window inserted into `new_dir[idx_old]`; every later get computed
`new_dir[idx_new]` and missed. The index stayed in bounds throughout, so
nothing asserted and nothing crashed — the entry was simply in the wrong table.

This is what the instrumentation showed directly, at the `ENODATA` return in
`lookup_mbuff`:

```
GETMISS h=7506397764127141955 expect_idx=10 insert_gen=4 cur_gen=5
```

`insert_gen=4, cur_gen=5` is exactly a put that observed the stale `grow_nr_m`
while `dir_ref_m.directory` already named the generation-5 directory.

### The fix

`hash_to_map()` now derives the index width from the directory pointer it just
loaded, rather than from a second variable:

```cpp
static u32 dir_idx_bits_(const std::vector<DirMapEntry> *const dir) {
        return 63U - static_cast<u32>(__builtin_clzl(dir->size()));
}
```

The directory doubles on every grow, so `floor(log2(size))` is precisely what
`grow_nr_m - 1` counted. Reading it from the object makes the lookup a single
atomic load: there is no longer a pair to keep in sync, so moving a statement
cannot reintroduce this. `grow()`'s copy loop uses the same helper on
`new_dir` instead of `hash_to_dir_idx_(h, grow_nr_m)`.

`grow_nr_m++` also moved inside the write lock. That is now belt and braces —
`grow_nr_m` is only read by the destructor's stats line — but there is no
reason to publish it outside the lock.

### What this cost to find, and what did not find it

Three measurements, in the order they were taken:

1. **`mprotect` compiled out, drain retained** — `ENODATA` reproduced
   identically. So it was not the protection mechanics, and the drain fix had
   not caused it.
2. **Copy-loop accounting** — `src_used == dst_used` at every grow (0, 7615,
   14968, 29399, 54759), tombstones 0, shifted 0. The copy loses nothing.
3. **`GETMISS` at the `ENODATA` return** — `insert_gen != cur_gen`, which
   pointed straight at the two-field read.

Steps 1 and 2 were both negative results, and both were necessary: they are
what ruled out the two hypotheses this document had recorded as most likely.

## Attempted and reverted: making grow() a coroutine

`grow_lock_m` is taken with `lock_blocking()`. That is correct from an ordinary
function, but **not** safe from inside a TRT task: `TrtLock::lock_blocking()`
spins on `sched_yield()`, which yields the OS thread and not the task, so if
the holder is another task on the same scheduler thread it can never run.
`grow()` is reachable from `put_mbuff`, which is a task under `RuntimeTrt`.

The obvious fix is to make `grow()` a `trt::CoroTask` and `co_await
grow_lock_m.lock()`. That was implemented (callers: `put_mbuff` co_awaits;
`init()` and `try_restore_entry()` drive it with `run_sync()`, both being
single-threaded) and it **builds clean and hangs**, so it was reverted.

What the hang looked like, on `-t 17 --thin`:

- CPU time stays at `00:00:00` while elapsed time climbs — sleeping, not slow.
- All 17 worker threads blocked in `PthreadLock::lock()` on the **same** mutex.
- No thread anywhere in `grow()`, `write_wait_readers()`, or `rd_enter()`.
- The mutex's `__data.__owner` names a thread itself blocked on that mutex.

Note what this implies regardless of the coroutine conversion: **`grow_lock_m`
had not actually been held since the migration** — it was a discarded
`CoroTask` until `lock_blocking()` was added — so the code under it had never
run with the lock genuinely taken. Making the lock real is what surfaced this.

To be clear about what that does and does not mean: single-writer grow is the
intended design and is not in question. The finding is that the grow logic has
a re-entrancy or lost-unlock path that was harmless while the guard was a
no-op, and becomes a deadlock once the guard is real.

Caveat on the owner evidence: glibc does not reliably maintain `__owner` for
default-type mutexes, so "the owner is waiting on itself" is a strong hint
rather than proof. The 17-threads-one-mutex observation does not depend on it.

This remains open. It is not currently reachable — `grow()` is only called from
`put_mbuff`'s `ENOSPC` path and from single-threaded init/restore, and the test
suite passes — but the hazard is real if a second caller appears. Worth
checking when picking it up:

- whether `grow()` can be re-entered on one thread — e.g. via
  `salsa::SalsaCtlr::allocate_grains()`'s `do { } while (EAGAIN)` loop
  triggering GC, and GC's `gc_callback` reaching a path that grows;
- whether any `co_return` path in `grow()` can be reached with the lock held
  (the reverted version unlocked on all three exits, which is why a re-entrancy
  is the better hypothesis).

## Why none of this was caught by CI

`do_run_test` printed `FAILURE.` and then exited 0, so `make run_tests` — which
CI runs — reported success while `bin/udepot-test` segfaulted on every run.
That is fixed: failures propagate. `do_run_known_failing_test` exists for
genuinely-known-broken tests and currently has no users.

## The rwlock_pagefault test

`test/rwlock-pagefault/resizable_table` — the example the header pointed at,
which had no source in this repo — now exists and runs in `make run_tests`. It
resizes an mmap'd table under concurrent readers, in both orderings uDepot has
used.

| ordering | rollback | result |
|---|---|---|
| drain-first (what `grow()` does today) | none | 0 faults, 0 bad values |
| protect-first (the original design) | `rd_execute__` | 3–13 rollbacks, 0 bad values |
| drain-first | `rd_execute__` | 0 faults, 0 bad values |

**The mechanism the migration abandoned is not broken.** Driven from an
ordinary function, `rd_execute__` catches the fault, rolls back, retries, and
returns correct data every time. What made it unusable was the *caller*
becoming a coroutine. Restoring it for the grow path is viable if that path is
kept non-coroutine — worth weighing against the `co_await` conversion that
hangs, if the grow stall ever shows up in a profile.

The protect-first case pins readers inside the protected window with an atomic
handshake rather than hoping the timing lands. An earlier version relied on
luck and reported 0 rollbacks — passing while exercising nothing, which is the
failure mode this whole document exists because of. The test now fails if no
rollback occurs.

### A fault without the rollback does not crash cleanly

Worth knowing before reading a crash report from the grow path. With
`rwlpf_rb__.rb_set` at 0, `sigsegv_handler` chains to `oldact_g.sa_sigaction`.
Once the process has installed the handler, that previous disposition *is* the
handler, so a fault recurses into it until the stack is gone. The process hangs
rather than terminating — it does not even respond to `alarm()`. A grow-path
fault may therefore present as a hang, not a segfault.

This is not hypothetical here: the A/B build used to confirm bug 2 (fix
reverted, everything else identical) hung after the fifth grow rather than
aborting with `61`, on the same command line that had aborted every previous
run.

## Reproducing

```
bin/udepot-test -f /dev/shm/udepot-test --segment-size 4096 \
    --size 1077936129 -w 100000 -r 100000 -t 17 --thin \
    --force-destroy --grain-size 32 --val-size 3072
```

No special hardware. `-t 17` and `--thin` matter: the thin store is what forces
the grow, and the concurrency is what makes readers be in flight during it. The
second invocation in `udepot-grow-test` reads the store the first leaves
behind, so it only tests anything while the first succeeds.
