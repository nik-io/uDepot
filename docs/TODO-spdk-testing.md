# TODO: testing the TRT SPDK backend without NVMe hardware

## Status

The TRT SPDK backend — and therefore uDepot's SPDK KV path — is **not covered
by any test** at present. It cannot be, on a machine without an NVMe device,
until the problem below is fixed.

What exists today and what it actually covers:

| test | covers | does *not* cover |
|---|---|---|
| `make -C trt run_spdk_bdev_test` | SPDK builds and links, the bdev API works, a malloc bdev can be created and read/written, hugepage setup is sane | anything in TRT or uDepot |
| `make -C trt run_spdk_bdev_perf` | same, with throughput reported | same |

`trt/src/tests/spdk_bdev_test.cc` includes only SPDK headers (`spdk/bdev.h`,
`spdk/event.h`, `spdk/thread.h`) and runs under `spdk_app_start`, SPDK's own
event framework. It never touches `trt::SPDK`, `SpdkQpair`, or the TRT
scheduler. It is a build and environment smoke test that happens to live in the
trt tree — useful, but it must not be mistaken for coverage of the backend.

## Why a bdev alone cannot test the backend

uDepot's SPDK backend uses the **raw NVMe driver**, not the bdev layer:
`trt/src/trt_util/spdk.{hh,cc}` has 24 `spdk_nvme_*` calls and zero
`spdk_bdev_*` calls. `SpdkQpair` is built on `spdk_nvme_ns_*` and
`spdk_nvme_ctrlr_alloc_io_qpair`.

So a malloc bdev can only reach the backend by being **exported as an NVMe
namespace**, which is what an NVMe-oF soft target does. "Test against a bdev"
and "test against NVMe-oF" therefore describe the same backing store; fabrics
is the adapter that lets the nvme driver consume a bdev. There is no third
option short of writing a bdev-based `uDepotIO_` backend.

## The blocker

With a loopback `nvmf_tgt` exporting a malloc bdev, uDepot gets a long way:

```
Connecting to NVMeoF target at 127.0.0.1:4420 (nqn.2016-06.io.spdk:cnode1)
Attached to NVMe over Fabrics controller at 127.0.0.1:4420: ...
set_SpdkNamespaceStr() Found a match ... Using: SPDK bdev Controller
...
init_local() No valid md header or force destroy was set, persisting one.
persist_dev_md() wrote dev md size=512 off=537918976
GC_NBIN_XNP_OVRW_HEAT_SEG with LOW_WATERMARK=2 HIGH_WATERMARK=4 bins=4 id=0
```

The controller attaches, the namespace is found, the store initialises and its
device metadata is written. Then **no I/O completes** and the process hangs.
This reproduces with as little as `-n 100 --trt-ntasks 4`, so it is not a
throughput or contention-under-load effect.

### Evidence gathered

**The target drops the host.** `nvmf_tgt` eventually logs:

```
nvmf_ctrlr_keep_alive_poll: Disconnecting host nqn.2014-08.org.nvmexpress:uuid:...
from subsystem nqn.2016-06.io.spdk:cnode1 due to keep alive timeout.
```

Keep-alives are sent from the host's poller. Their absence says the SPDK poller
on the uDepot side is not running, or not running often enough — which also
explains why no completion ever arrives.

**The TRT scheduler itself is alive.** `gdb -p` during the hang shows the
worker thread inside the scheduler loop, not blocked:

```
Thread 8 "dpdk-worker1":
#0  trt::Scheduler::start_ (this=...) at ...
#1  trt::scheduler_rte_thread (arg=...) at src/trt_backends/trt_spdk.cc:221
#2  eal_thread_loop () from librte_eal.so.24
```

Other threads are ordinary idle states: `eal_thread_wait_command` on unused
DPDK workers, `pthread_cond_wait` in `lsa_allocator_thread`.

The poller task *is* spawned — `TrtSpdkIO::thread_init()` runs during
`init_local() -> thread_local_entry()`, and the log shows
`thread_init() Spawining SPDK poller`.

### Leading suspect (unproven)

`trt/src/trt_util/spdk.cc` sets the SPDK/DPDK core mask from the process's CPU
affinity:

```c
err = sched_getaffinity(0, setsize, cpuset);
...
opts.core_mask = cpuset_s;
```

So the uDepot process claims **every** core it is allowed to run on. An SPDK
reactor busy-polls at 100%, so on a 4-core box the client's lcores and the
target's reactor fight for the same CPUs, and the client's poller may be
starved badly enough to miss keep-alive deadlines.

This was not confirmed. The obvious experiment — pin the target to one core
(`taskset -c 0 nvmf_tgt -m 0x1`) and the client to a disjoint set
(`taskset -c 2,3`), since the client mask follows affinity — was set up but not
carried through to a clean result.

## Suggested next steps

1. Run the disjoint-pinning experiment above. If the hang clears, the fix is to
   let callers set the core mask explicitly instead of inheriting affinity.
2. If pinning does not help, instrument `trt::SPDK::poller_task` to confirm
   whether it is being scheduled at all during the PUT phase; a poller that is
   spawned but never resumed is the other obvious shape for this.
3. Check whether the poller needs to run on the same lcore that owns the qpair.
   `run_trt_spdk()` in `bench/io_layer_bench.cc` deliberately spawns a single
   scheduler on one worker lcore; if the qpair is allocated on a different
   lcore than the poller, completions would never be reaped.
4. Once I/O completes, wire the zero-copy invariant over SPDK into
   `make run_perf_test` so all three backends are checked, and add a
   `run_spdk_nvmef_test` target that starts and tears down the soft target.

## What is already in place for this work

These are merged and correct on their own; the work above depends on them.

- **`register_controllers()` ordering** (`trt/src/trt_util/spdk.cc`): a failing
  local PCIe probe used to return immediately, so a host with no local NVMe
  could never reach a configured fabrics target. It is now non-fatal when
  nvmef targets exist; only ending with zero namespaces is an error.
- **`TrtSpdkIO::add_nvmef_target()`**: `SpdkGlobalState::add_nvmef_target()`
  existed but nothing called it. There is now a way to configure a target
  before `global_init()`.
- **Whole-archiving the SPDK modules** (`Makefile`): SPDK registers its NVMe
  transports and socket implementations from static constructors in objects
  nothing references, so the static link dropped them and TCP reported
  `trtype 3 (TCP) not available` at runtime. `libspdk_nvme` and
  `libspdk_sock_posix` are whole-archived — once only, since the plain libs are
  repeated to resolve circular deps and repeating a whole-archived one
  multiply-defines every symbol.
- **`--spdk` and `--nvmef A:P:NQN`** on `bench/io_layer_bench`. Not part of any
  test target.

## Reproduction

Two sizing rules matter, and both cost time to find:

- The malloc bdev must **not** be an exact multiple of the segment size. uDepot
  puts device metadata in the tail after
  `align_down(device_size, segment_size * grain_size)`; an exactly-divisible
  size leaves no tail and init fails with `Not enough spare capacity`. Use
  513MiB, not 512MiB.
- Grain size must be at least the device sector size, or segment metadata
  writes fail `EINVAL` under O_DIRECT. The default of 512 is fine.

```sh
sudo sh -c 'echo 1536 > /proc/sys/vm/nr_hugepages'
sudo mkdir -p /dev/hugepages && sudo mount -t hugetlbfs nodev /dev/hugepages

sudo trt/external/spdk/build/bin/nvmf_tgt -m 0x1 -s 512 &
R=trt/external/spdk/scripts/rpc.py
sudo $R nvmf_create_transport -t TCP
sudo $R bdev_malloc_create 513 512 -b Malloc0
sudo $R nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
sudo $R nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
sudo $R nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420

make BUILD_SPDK=1 BUILD_URING=1 bench/io_layer_bench
sudo LD_LIBRARY_PATH=trt/external/spdk/dpdk/build/lib \
    bench/io_layer_bench --compare --spdk \
    --nvmef 127.0.0.1:4420:nqn.2016-06.io.spdk:cnode1 -n 2000

# teardown -- hugepages stay reserved until released
sudo pkill nvmf_tgt && sudo umount /dev/hugepages
sudo sh -c 'echo 0 > /proc/sys/vm/nr_hugepages'
```
