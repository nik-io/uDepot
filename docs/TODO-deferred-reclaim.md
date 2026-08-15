# TODO: deferred reclaim of retired directories

## Why

The design intends the old and new directory tables to **coexist** while
references to the old one are outstanding, with the old one reclaimed lazily,
and readers blocked as little as possible. `grow()` does neither: it
`mprotect`s and `munmap`s the old tables inline, before the pointer swap, so
the two never coexist and nothing is freed lazily. See `docs/TODO-grow-race.md`,
"What the design intends".

This is the first step, and it gates the second. Gradual per-table growth (the
uDepot paper's improvement, started in `uDepotDirMapOR`'s shadow directory —
`docs/TODO-dir-map-or.md`) still cannot let two tables coexist while the old
one is unmapped inline, so it does not remove the stall on its own.

It is also the reason the write lock spans the whole grow. With the old mapping
being destroyed inline, the lock is the only thing keeping readers off memory
that is about to disappear. Moving the lock around without changing this is
moving the lock while the reason for the lock stays put.

## What lands

Retire instead of destroy. `grow()` publishes the new directory, hands the old
one to a retire list, and returns; the old tables are `munmap`ed and their
grains invalidated once no reader can still hold the pointer. No `mprotect`, no
faults, so no rollback — which matters because the rollback cannot come back
(`sigsetjmp` cannot span a `co_await`; see `docs/TODO-grow-race.md` bug 1).

`hash_to_map()` already derives the index width from the directory pointer it
loads, so a snapshot describes its own geometry. That was a prerequisite for
this and is done.

## Two decisions needed before starting

**1. Where the grace period comes from.** TRT is cooperative, but that does not
give quiescence for free: `local_op_execute` holds the directory across a
`co_await` while it does I/O, which is exactly the case that matters. Options:

- an explicit epoch counter bumped at `rd_enter`/`rd_exit`, with retired
  directories tagged by epoch;
- reuse the BRLock's 128 sub-lock counters, which already track in-flight
  readers precisely — `wr_ready()` is a full-directory quiescence check today.
  Cheaper to build, but it is a global scan, and it answers "no readers at all"
  rather than "no readers holding *this* directory".

**2. What writers do during the copy.** Reclaim frees readers; writers still
cannot mutate a table the copy has already passed. `mprotect` + rollback is
what used to catch that, and it is gone. Options:

- keep writers on the write-lock path — much less contended than blocking
  everyone, and the smallest change. Suggested default;
- the write log the `rwlock_pagefault` header describes ("WR requests are more
  tricky, and we would need some kind of log for the changes that we could
  replay on the new version"), replayed after the copy;
- defer to gradual growth, where only the table currently being copied needs
  writers held off, briefly.

## Not to be done as a drive-by

Agent Rule 4 applies here in both directions. The current drain is a stopgap
that costs a documented property, and it is labelled as one — do not treat it
as the intended behaviour and "simplify" toward it. Equally, do not start
trading other properties to get reclaim working.
