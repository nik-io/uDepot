# Concurrent directory-map grow leaves readers holding stale HashEntry pointers

## Status

**Open.** `udepot-grow-test` is quarantined in the Makefile
(`do_run_known_failing_test`) so it stays visible without failing the build.
It is not fixed, and it is not flaky — it fails every run.

## Symptom

```
bin/udepot-test -f /dev/shm/udepot-test --segment-size 4096 \
    --size 1077936129 -w 100000 -r 100000 -t 17 --thin \
    --force-destroy --grain-size 32 --val-size 3072
```

segfaults, immediately after the directory map decides to grow:

```
src/uDepot/lsa/udepot-directory-map.cc:166: grow() Trying to grow the directory map.

Thread 34 "udepot-test" received signal SIGSEGV, Segmentation fault.
0x00005555555abc70 in udepot::uDepotMap<udepot::RuntimePosix>::update (
    pba=778240, h=7381833846413672882, he=0x7ffff47dafd0,
    this=0x7ffff7a16010) at src/include/uDepot/lsa/udepot-map.hh:197
197             he->bucket_offset = bucket_offset;
#1  uDepotMap<RuntimePosix>::insert            udepot-map.hh:188
#2  uDepotDirectoryMap<RuntimePosix>::insert   udepot-directory-map.hh:78
#3  uDepotSalsa<RuntimePosix>::set_mapping     udepot-lsa.cc:1259
#4  uDepotSalsa<RuntimePosix>::local_put_mbuff udepot-lsa.cc:1380
#5  trt::CoroTask::run_sync                    trt/src/trt/task.hh:114
#6  udepot::test_thread<RuntimePosix>          test/uDepot/udepot-test.cc:544
```

The faulting write is through `he`, a `HashEntry *` obtained before the grow.
The hash tables are mmap'd and the grow remaps them, so a pointer taken before
the remap dangles afterwards.

The second invocation in `udepot-grow-test` reads the store the first one left
behind. Once the first crashes mid-write, the second is running crash recovery
over a corrupt store, so its failure is downstream noise, not a second bug:

```
udepot-directory-map.cc:590: restore() Found no valid directory mapping segments restored=0
udepot-directory-map.cc:102: init() Directory map restore failed with 61.
udepot-lsa.cc:688: try_restore_entry: Assertion `trgt' failed.
```

## Not caused by the lock fixes

Checked, not assumed. Built `bin/udepot-test` at `d744876` — the commit before
`docs/concurrent-get-fix.md`'s changes — in a separate worktree and ran the
same command:

| build | result over 3 runs |
|---|---|
| `d744876` (before the lock fixes) | segfault, abort, segfault |
| `02cb083` (after) | segfault, segfault, segfault |

Pre-existing. Taking `grow_lock_m` properly (it was a discarded coroutine and
had never been held) did not introduce this and does not fix it — the grow lock
serialises *growers*, while this is a reader holding a pointer across a remap.

## Why CI never caught it

`do_run_test` printed `FAILURE.` and then exited 0, so `make run_tests` — which
CI runs — reported success while this segfaulted on every run. That is fixed:
failures now propagate, and this test is quarantined explicitly rather than
being swallowed along with everything else.

## Where to look

`rwlock_pagefault` (`RT::RwpfTy`) is the mechanism meant to keep readers out of
a table while it is being remapped. Worth establishing first:

- whether `uDepotMap::insert`/`update` take the pagefault read lock at all on
  the `RuntimePosix` path, or only on the TRT one (`rwlock_pagefault_trt`);
- whether `set_mapping` can hold `he` across a point where the grow can remap;
- whether the fix belongs at the pointer (re-look-up after grow) or at the
  lock (hold the read lock across the insert).

Reproduce with the command at the top; it does not need special hardware.
`-t 17`/`--thin` matter — the thin store is what forces the grow.
