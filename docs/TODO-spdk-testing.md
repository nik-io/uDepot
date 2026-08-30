# Testing the TRT SPDK backend without NVMe hardware — RESOLVED

## Status

**Resolved.** The TRT SPDK backend — and therefore uDepot's SPDK KV path — is
now covered by a test that runs on a machine with no NVMe device:

```
make BUILD_SPDK=1 run_spdk_nvmef_test
```

It starts an SPDK `nvmf_tgt` exporting a RAM-backed malloc bdev as an NVMe
namespace over TCP loopback, then runs `udepot-test`'s SPDK backend (`-u 7`) as a
fabrics initiator against it and asserts that PUTs/GETs complete and the store
shuts down cleanly. Driver: `scripts/spdk-nvmef-test.sh`. CI runs it in the
`spdk` job. Unlike `run_spdk_bdev_test`, it drives `trt::SPDK`, `SpdkQpair` and
the TRT scheduler on the actual I/O path.

The backend learns the target from the `UDEPOT_NVMEF` environment variable
(`traddr:trsvcid:subnqn`), parsed in `TrtSpdkIO::global_init()`, so `udepot-test`
needs no command-line change to run against a soft target.

## What the stall actually was

The store used to initialise and then hang right after `persist_dev_md()`, and
the target eventually logged a keep-alive timeout. An earlier version of this
doc guessed the cause was **CPU-affinity contention** — the SPDK reactor
busy-polling the same cores as the target. That was wrong. Pinning the client
off the target's core changes nothing here; the real causes were three latent
bugs in the SPDK path, none of which had ever run to completion before (the
backend hung at the first async I/O, so shutdown and the keep-alive window were
never reached):

1. **The poller task was never scheduled.** `TrtSpdkIO::thread_init()` is a
   plain function, not a coroutine, and it spawned the completion poller with
   `trt::T::spawn(...)`. `T::spawn()` returns a `SpawnAwaitable` that only
   enqueues the task when it is `co_await`ed; called from non-coroutine code the
   awaitable is discarded and the poller never runs. Synchronous init writes
   completed (they self-poll), but the first *async* PUT/GET then suspended
   forever with nothing reaping its completion. Fixed by using
   `trt::T::spawn_detached_no_wait(...)`, the API the AIO and io_uring pollers
   already use from their own `thread_init()`. This is the primary fix; it is
   what makes the backend complete I/O at all. (Same bug fixed in
   `trt-spdk-array.cc` and `spdk.cc`.)

2. **The admin queue was never polled.** `execute_completions()` polls only the
   I/O qpair. On an NVMe-oF controller the keep-alive lives on the *admin* queue
   and only advances when `spdk_nvme_ctrlr_process_admin_completions()` is
   called; without it the target hits its keep-alive timeout and disconnects the
   host, after which no I/O completes. Fixed by `SpdkState::process_admin_completions()`,
   called from the poller and throttled to ~10x/sec (a no-op cost for local
   PCIe controllers, which default to KATO=0). Confirmed necessary: disabling it
   reproduces the keep-alive timeout on a run longer than the deadline.

3. **The directory-map footer overran a huge-page mapping.** Once the backend
   ran to shutdown, `uDepotDirectoryMap::shutdown()` crashed intermittently
   (~1/3 of runs) writing the directory footer at
   `seg_size*grain_size - sizeof(ftr)`. When the region is mmap'd with huge
   pages its size is `align_down(seg_size*grain_size, 2MiB)` — smaller — so the
   footer write landed past the mapping. Only the SPDK path reserves hugepages,
   which is why AIO/io_uring on tmpfs never hit it. The footer's on-disk offset
   is fixed by the restore path and never moves. The committed fix is a
   **stopgap**: it takes the huge-page mapping only when `seg_size*grain_size` is
   itself 2MiB-aligned (nearly never — the per-segment metadata steals the last
   grain of a 2MiB-multiple segment), so in practice the directory maps on 4KiB
   pages, which span the whole net region and keep the footer offset mapped. The
   **proper fix** keeps the directory on hugepages: a segment sized as a 2MiB
   multiple starts on a 2MiB-aligned device offset, so map `align_down(net, 2MiB)`
   with `MAP_HUGETLB` and keep the per-segment metadata grain and the 512B footer
   in the reserved tail beyond that span, reached via I/O (`restore()` already
   `pread`s the footer). It needs a restore round-trip test (write, reopen
   without `--force-destroy`, footer read back and matches). See
   `src/uDepot/lsa/udepot-directory-map.cc` and CLAUDE.md, "Segment geometry" /
   "Hugepage invariant for directory tables".

## What is already in place (unchanged, still correct)

- **`register_controllers()` ordering** (`trt/src/trt_util/spdk.cc`): a failing
  local PCIe probe is non-fatal when nvmef targets are configured; only ending
  with zero namespaces is an error.
- **`TrtSpdkIO::add_nvmef_target()`**: registers a fabrics target before
  `global_init()`. Now also reachable via the `UDEPOT_NVMEF` env var.
- **Whole-archiving the SPDK modules** (`Makefile`): `libspdk_nvme` and
  `libspdk_sock_posix` are whole-archived so the NVMe transports and socket
  implementations registered from static constructors are not dropped by the
  static link.

## Reproduction / manual run

```sh
# software target
sudo trt/external/spdk/build/bin/nvmf_tgt -m 0x1 &
R=trt/external/spdk/scripts/rpc.py
sudo $R nvmf_create_transport -t TCP
sudo $R bdev_malloc_create 513 512 -b Malloc0          # 513 MiB, not a multiple of the segment size
sudo $R nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
sudo $R nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
sudo $R nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420

# initiator: udepot-test's SPDK backend
make BUILD_SPDK=1 bin/udepot-test
sudo env UDEPOT_NVMEF=127.0.0.1:4420:nqn.2016-06.io.spdk:cnode1 \
    LD_LIBRARY_PATH=trt/external/spdk/dpdk/build/lib \
    bin/udepot-test -f 'SPDK' -u 7 --thin --force-destroy \
    -w 20000 -r 20000 -t 1 --grain-size 512 --val-size 3072

sudo pkill nvmf_tgt
```

`scripts/spdk-nvmef-test.sh` does all of the above (setup, run, teardown) and is
what `make run_spdk_nvmef_test` and CI invoke.

## Still open

- Wiring the zero-copy invariant over SPDK into `run_perf_test` (all three
  backends). The invariant already holds over the soft target when measured with
  `io_layer_bench --compare --spdk` (GET ~+13%, PUT ~+3% for mbuff), but it is
  not yet asserted in a Makefile target.
- The soft target reserves and busy-polls one core; on a 2-core runner the
  initiator shares it. The test handles this (it only pins the initiator off
  core 0 when there are ≥3 CPUs), but a very constrained runner will be slower.
