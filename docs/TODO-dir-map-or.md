# uDepotDirMapOR — experimental, excluded from the build

## Status

**Experimental, not a supported feature, excluded from the build.** The source
is kept in the tree; nothing compiles it.

`uDepotDirMapOR` is an alternative directory map that keeps a *shadow*
directory (`alloc_shadow()`) alongside the live one, so a grow can be prepared
ahead of the switch rather than done inline. `uDepotDirectoryMap` is the
supported implementation and the one `uDepotSalsa<RT>::map_m` uses.

## Why it is excluded rather than deleted

It does not compile:

```
src/uDepot/lsa/udepot-dir-map-or.cc:119: error: 'udepot_io_m' was not declared
    in this scope; did you mean 'uDepotIO_'?
src/uDepot/lsa/udepot-dir-map-or.cc:329: error: 'udepot_io_m' was not declared
    in this scope
src/include/util/debug.h:22: error: too many arguments for format
    [-Werror=format-extra-args]
```

This predates the recent lock and grow work — `make test/uDepot/uDepotDirMapORTest`
failed the same way before any of it. It has evidently been broken for a while,
which was easy to miss because the target was never part of `make run_tests`.

Removed from the Makefile:

- `udepot_all_SRC` (it was pulling a broken source into dependency generation)
- the `test/uDepot/uDepotDirMapORTest` link target

The source and its test stay in the tree so the approach is not lost.

## What reviving it would involve

1. **Fix the build.** `udepot_io_m` needs to come from somewhere —
   `uDepotDirectoryMap` takes `typename RT::IO &udepot_io` in its constructor
   and stores it; `uDepotDirMapOR`'s constructor takes neither.
2. **Apply the lock fixes.** It carries the same defects that were fixed in
   `uDepotDirectoryMap`:
   - three `grow_lock_m` sites that discarded the `trt::CoroTask` returned by
     `lock()`, so the lock was never taken (`docs/concurrent-get-fix.md`).
     They currently read `lock_blocking()`, which is the right call from an
     ordinary function but **not** safe from inside a TRT task — see the
     deadlock note in `docs/TODO-grow-race.md` before relying on it.
   - the reader-drain ordering around `mprotect`, which
     `uDepotDirectoryMap<RT>::grow()` now does before changing protection bits
     rather than after the copy.
3. **Decide what it is for.** The shadow directory is only worth its complexity
   if it removes the grow stall that the supported implementation now has —
   readers are drained for the whole copy. If that stall matters, this is the
   design that addresses it, and that is the argument for making it supported.

## Related

- `docs/concurrent-get-fix.md` — the discarded-`CoroTask` lock defects
- `docs/TODO-grow-race.md` — the grow path in the supported implementation
