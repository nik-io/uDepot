/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Benchmarks the uDepot zero-copy (Mbuff) KV interface against the
 *  non-zero-copy (raw buffer) one.
 *
 *  This replicates the "thin" test variants from test/uDepot/udepot-test.cc,
 *  which are selected there by --thin and --thin --zero-copy:
 *
 *    --copy  (non-zero-copy)  == put_test_thin      / get_test_thin
 *        Uses the raw buffer KV interface:
 *            KV->put(keyb, key_size, val, val_size)
 *            KV->get(keyb, key_size, val_out, ...)
 *        uDepotSalsa services these by staging the key/value through an
 *        internal Mbuff and copying in (put) or out (get).
 *
 *    --mbuff (zero-copy)      == put_test_thin_mbuff / get_test_thin_mbuff
 *        Uses the Mbuff KV interface (KV_MbuffInterface):
 *            KV->put(mb, key_size)
 *            KV->get(keymb, mb)
 *        The caller's Mbuff is handed straight to the I/O layer, so no
 *        staging copy happens.
 *
 *  Both modes run a PUT phase followed by a GET phase over the same
 *  deterministically generated key/value set, exactly as udepot-test does.
 *
 *  Backends: --aio (RuntimeTrt, default) and --uring (RuntimeTrtUring).
 *
 *  NOTE on --grain-size: O_DIRECT requires sector-aligned I/O, and uDepot
 *  sizes its segment metadata writes in grains, so the grain must be at least
 *  the device sector size. The default is 512; a 32-byte grain makes those
 *  writes 64 bytes and every O_DIRECT backend rejects them with EINVAL.
 *
 *  NOTE on -n: keep it at the default (200k) or higher when comparing the
 *  two modes. Below roughly 100k ops the store never fills enough for the
 *  PUT phase to become I/O bound, and the zero-copy difference sits inside
 *  run-to-run noise -- at 100k the PUT comparison inverts at random, while
 *  at 200k zero-copy wins every paired run. Compare paired runs (alternate
 *  the two modes) rather than medians of separate batches: throughput
 *  drifts steadily across a batch, which biases unpaired medians.
 */

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "kv.hh"
#include "trt_util/timer.h"
#include "util/types.h"
#include "uDepot/backend.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"
#include "uDepot/mbuff.hh"
#include "uDepot/udepot-lsa.hh"
#if defined(UDEPOT_TRT_SPDK)
#include <rte_lcore.h>
#include "uDepot/io/trt-spdk.hh"
#endif

#include "trt/uapi/trt.hh"
#include "trt_util/arg_pool.hh"

using namespace udepot;

// Same key/value generator udepot-test.cc uses for the thin tests.
constexpr u64 prime_g = 2654435761UL;

enum class Mode { MBUFF, COPY };

struct bench_conf {
    Mode   mode         = Mode::MBUFF;
    u64    seed         = 1;
    u32    val_size     = 3072;
    u64    nops         = 200000;
    size_t ntasks       = 128;
    size_t iterations   = 5;
    // O_DIRECT requires sector-aligned I/O, and uDepot sizes its
    // segment metadata writes in grains. A 32-byte grain makes those
    // writes 64 bytes, which EINVALs on any O_DIRECT backend -- so the
    // grain must be at least the device sector size. 512 is the
    // smallest that holds on both 512e and 4Kn devices here, and is
    // what the Makefile's own TRT tests use.
    u64    grain_size   = 512;
    u64    segment_size = 4096;  // in grains
};

static bench_conf bconf_g;

// Set by the comparison run; becomes the process exit status.
static bool compare_failed_g = false;

// ---------------------------------------------------------------------------
// Non-zero-copy variants (raw buffer KV interface), after put/get_test_thin
// ---------------------------------------------------------------------------

template<typename RT>
static trt::CoroTask put_thin(uDepotSalsa<RT> *const KV, const u64 start, const u64 end)
{
    char keyb[32] = { 0 };
    char *const val = (char *)malloc(bconf_g.val_size);
    if (nullptr == val)
        co_return (trt::RetT)(int)ENOMEM;
    memset(val, 0, bconf_g.val_size);

    for (u64 i = start; i < end; ++i) {
        const u64 key = ((bconf_g.seed + i) * prime_g);
        const u64 valu = key * prime_g;
        const u64 key_size = 8 + (key % 24);
        const u64 val_size = bconf_g.val_size;
        memcpy(val, &valu, sizeof(valu));
        memcpy(keyb, &key, sizeof(key));
        const int err = (int)(co_await KV->put(keyb, key_size, val, val_size));
        if (0 != err && EEXIST != err) {
            fprintf(stderr, "put returned %d key=0x%16lx\n", err, key);
            free(val);
            exit(1);
        }
    }
    free(val);
    co_return 0;
}

template<typename RT>
static trt::CoroTask get_thin(uDepotSalsa<RT> *const KV, const u64 start, const u64 end)
{
    char keyb[32] = { 0 };
    char *const val_out = (char *)malloc(bconf_g.val_size);
    if (nullptr == val_out)
        co_return (trt::RetT)(int)ENOMEM;

    for (u64 i = start; i < end; ++i) {
        const u64 key = ((bconf_g.seed + i) * prime_g);
        const u64 valu = key * prime_g;
        const u64 key_size = 8 + (key % 24);
        memcpy(keyb, &key, sizeof(key));
        size_t val_size_read, val_size;
        const int err = (int)(co_await KV->get(keyb, key_size, val_out,
                                               bconf_g.val_size,
                                               val_size_read, val_size));
        u64 val_ret;
        memcpy(&val_ret, val_out, sizeof(val_ret));
        if (0 != err || val_ret != valu) {
            fprintf(stderr, "get returned %d vale=%lu valret=%lu\n",
                    err, valu, val_ret);
            free(val_out);
            exit(1);
        }
        assert(val_size_read == val_size);
    }
    free(val_out);
    co_return 0;
}

// ---------------------------------------------------------------------------
// Zero-copy variants (Mbuff KV interface), after put/get_test_thin_mbuff
// ---------------------------------------------------------------------------

template<typename RT>
static trt::CoroTask put_thin_mbuff(uDepotSalsa<RT> *const KV, const u64 start, const u64 end)
{
    const size_t prefix_size = KV->putKeyvalPrefixSize();
    const size_t suffix_size = KV->putKeyvalSuffixSize();
    Mbuff mb = KV->mbuff_alloc(32 + bconf_g.val_size + prefix_size + suffix_size);

    for (u64 i = start; i < end; ++i) {
        const u64 key = ((bconf_g.seed + i) * prime_g);
        const u64 valu = key * prime_g;
        const u64 key_size = 8 + (key % 24);
        const u64 val_size = bconf_g.val_size;

        mb.reslice(0);
        auto append_fn =
            [&prefix_size, &key, &key_size, &valu, &val_size]
            (unsigned char *b, size_t b_len) -> size_t {
                assert(b_len > 32 + bconf_g.val_size + prefix_size);
                // skip prefix
                b += prefix_size;
                // write key
                *((u64 *)b) = key;
                b += 8;
                for (size_t j = 0; j < key_size - 8; j++) {
                    *b = 0;
                    b++;
                }
                // write val (only the first 8 bytes)
                *((u64 *)b) = valu;
                return prefix_size + key_size + val_size;
            };

        mb.append(std::ref(append_fn));
        assert(mb.get_valid_size() == prefix_size + key_size + val_size);
        mb.reslice(key_size + val_size /* len */, prefix_size /* offset */);
        const int err = (int)(co_await KV->put(mb, key_size));
        if (0 != err && EEXIST != err) {
            fprintf(stderr, "put returned %d key=0x%16lx\n", err, key);
            KV->mbuff_free_buffers(mb);
            exit(1);
        }
    }
    KV->mbuff_free_buffers(mb);
    co_return 0;
}

template<typename RT>
static trt::CoroTask get_thin_mbuff(uDepotSalsa<RT> *const KV, const u64 start, const u64 end)
{
    const size_t prefix_size = KV->putKeyvalPrefixSize();
    Mbuff mb = KV->mbuff_alloc(32 + bconf_g.val_size + prefix_size);
    Mbuff keymb = KV->mbuff_alloc(32);
    keymb.append_zero(32);

    for (u64 i = start; i < end; ++i) {
        const u64 key = ((bconf_g.seed + i) * prime_g);
        const u64 valu = key * prime_g;
        const u64 key_size = 8 + (key % 24);

        keymb.reslice(0);
        auto key_fn =
            [&key, &key_size] (unsigned char *b, size_t b_len) -> size_t {
                assert(b_len > 32); // assume a single buffer
                *((u64 *)b) = key;
                b += 8;
                for (size_t j = 0; j < key_size - 8; j++) {
                    *b = 0;
                    b++;
                }
                return key_size;
            };
        keymb.append(std::ref(key_fn));
        assert(keymb.get_valid_size() == key_size);

        mb.reslice(0);
        const int err = (int)(co_await KV->get(keymb, mb));
        const u64 val_ret = 0 == err ? mb.template read_val<u64>(0) : 0;
        if (0 != err || val_ret != valu) {
            fprintf(stderr, "get returned %d vale=%lu valret=%lu\n",
                    err, valu, val_ret);
            KV->mbuff_free_buffers(mb);
            KV->mbuff_free_buffers(keymb);
            exit(1);
        }
        assert(mb.get_valid_size() == bconf_g.val_size);
    }
    mb.reslice(0);
    KV->mbuff_free_buffers(mb);
    keymb.reslice(0);
    KV->mbuff_free_buffers(keymb);
    co_return 0;
}

// ---------------------------------------------------------------------------
// Task plumbing (after do_trt_run() in udepot-test.cc)
// ---------------------------------------------------------------------------

template<typename RT>
using bench_fn = std::function<trt::CoroTask (uDepotSalsa<RT> *, u64, u64)>;

template<typename RT>
struct worker_arg {
    KV                   *kv;
    ArgPool<worker_arg>  *ap;
    bench_fn<RT>          op_fn;
    u64                   start;
    u64                   len;
};

template<typename RT>
static trt::CoroTask t_worker(void *arg_)
{
    worker_arg<RT> *arg = static_cast<worker_arg<RT> *>(arg_);
    co_await arg->op_fn((uDepotSalsa<RT> *)arg->kv, arg->start, arg->start + arg->len);
    arg->ap->put_arg(arg);
    co_return 0;
}

template<typename RT>
static trt::CoroTask run_phase(KV *kv, bench_fn<RT> fn, u64 nops, double *secs_out)
{
    trt::Task::List tl;
    const size_t ntasks = bconf_g.ntasks;

    ArgPool<worker_arg<RT>> task_args(ntasks);
    for (size_t i = 0; i < ntasks; i++) {
        worker_arg<RT> *warg = task_args.get_arg();
        assert(warg);
        warg->kv    = kv;
        warg->ap    = &task_args;
        warg->op_fn = fn;
        warg->len   = nops / ntasks;
        warg->start = i * warg->len;
        if (i == ntasks - 1)
            warg->len += nops % ntasks;
        trt::Task *t = trt::T::alloc_task(t_worker<RT>, warg, nullptr, false);
        tl.push_back(*t);
    }

    xtimer_t t; timer_init(&t); timer_start(&t);
    co_await trt::T::spawn_many(tl);
    for (size_t i = 0; i < ntasks; i++)
        co_await trt::T::task_wait();
    timer_pause(&t);

    *secs_out = timer_secs(&t);
    co_return 0;
}

static void report(const char *phase, double secs, u64 nops, u32 val_size)
{
    printf("%s time=%lfs Kops/sec=%lf MiB/sec=%lf\n",
           phase, secs,
           nops / (secs * 1000),
           (1.0 * nops * val_size) / (secs * 1024 * 1024));
    fflush(stdout);
}

struct main_arg {
    std::shared_ptr<KV> kv;
    KV_conf            *conf;
};

template<typename RT>
static trt::CoroTask t_main(void *arg__)
{
    main_arg *arg = static_cast<main_arg *>(arg__);

    int err = ENOMEM;
    arg->kv = std::shared_ptr<KV>(KV_factory::KV_new(*arg->conf));
    if (!arg->kv || (err = arg->kv->init()) != 0) {
        fprintf(stderr, "KV init failed with %d (%s)\n", err, strerror(err));
        exit(1);
    }

    const bool zc = (bconf_g.mode == Mode::MBUFF);
    bench_fn<RT> put_fn = zc ? bench_fn<RT>(put_thin_mbuff<RT>)
                             : bench_fn<RT>(put_thin<RT>);
    bench_fn<RT> get_fn = zc ? bench_fn<RT>(get_thin_mbuff<RT>)
                             : bench_fn<RT>(get_thin<RT>);

    double secs;

    co_await run_phase<RT>(arg->kv.get(), put_fn, bconf_g.nops, &secs);
    report("PUT", secs, bconf_g.nops, bconf_g.val_size);

    co_await run_phase<RT>(arg->kv.get(), get_fn, bconf_g.nops, &secs);
    report("GET", secs, bconf_g.nops, bconf_g.val_size);

    arg->kv->shutdown();
    trt::T::set_exit_all();
    co_return 0;
}

// ---------------------------------------------------------------------------
// Paired comparison of the two KV interfaces
//
// Runs the raw-buffer and Mbuff variants alternately inside one scheduler,
// over one store. Alternating is what makes the comparison valid: throughput
// drifts steadily over a batch -- enough on a cloud container to swamp the
// effect being measured -- and pairing cancels that drift out of each delta.
//
// Both variants write identical key/value content, so they are interchangeable
// on the same store and the GET phases read the same data.
// ---------------------------------------------------------------------------

struct sample { double copy, mbuff; };

static double median_of(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// Reports one phase and returns true if the zero-copy path held up.
static bool compare_phase(const char *phase, const std::vector<sample> &samples)
{
    std::vector<double> deltas;
    unsigned wins = 0;
    for (const sample &s : samples) {
        // Both are durations, so a lower mbuff time is the win.
        const double d = (s.copy - s.mbuff) / s.copy;
        deltas.push_back(d);
        if (d > 0)
            wins++;
    }

    std::vector<double> copy_secs, mbuff_secs;
    for (const sample &s : samples) {
        copy_secs.push_back(s.copy);
        mbuff_secs.push_back(s.mbuff);
    }

    const double md = median_of(deltas);
    printf("%s: n=%zu mbuff faster in %u/%zu pairs\n",
           phase, samples.size(), wins, samples.size());
    printf("  raw-buffer median=%.3fs  mbuff median=%.3fs  delta=%+.1f%%\n",
           median_of(copy_secs), median_of(mbuff_secs), md * 100.0);
    printf("  pairs:");
    for (const sample &s : samples)
        printf(" %.2f->%.2f", s.copy, s.mbuff);
    printf("\n");

    if (md < 0) {
        printf("  FAIL: the Mbuff (zero-copy) interface is slower than raw buffers\n");
        return false;
    }
    printf("  OK\n");
    return true;
}

template<typename RT>
static trt::CoroTask t_main_compare(void *arg__)
{
    main_arg *arg = static_cast<main_arg *>(arg__);

    int err = ENOMEM;
    arg->kv = std::shared_ptr<KV>(KV_factory::KV_new(*arg->conf));
    if (!arg->kv || (err = arg->kv->init()) != 0) {
        fprintf(stderr, "KV init failed with %d (%s)\n", err, strerror(err));
        exit(1);
    }

    KV *kv = arg->kv.get();
    std::vector<sample> puts, gets;
    double a, b;

    for (size_t i = 0; i < bconf_g.iterations; i++) {
        co_await run_phase<RT>(kv, put_thin<RT>, bconf_g.nops, &a);
        co_await run_phase<RT>(kv, put_thin_mbuff<RT>, bconf_g.nops, &b);
        puts.push_back({a, b});

        co_await run_phase<RT>(kv, get_thin<RT>, bconf_g.nops, &a);
        co_await run_phase<RT>(kv, get_thin_mbuff<RT>, bconf_g.nops, &b);
        gets.push_back({a, b});

        fprintf(stderr, "  [%zu/%zu] put %.2f->%.2f  get %.2f->%.2f\n",
                i + 1, bconf_g.iterations,
                puts.back().copy, puts.back().mbuff,
                gets.back().copy, gets.back().mbuff);
    }

    bool ok = compare_phase("PUT", puts);
    ok = compare_phase("GET", gets) && ok;
    compare_failed_g = !ok;

    kv->shutdown();
    trt::T::set_exit_all();
    co_return 0;
}

template<typename RT>
static int run_trt(KV_conf *conf, bool compare)
{
    main_arg arg;
    arg.kv   = nullptr;
    arg.conf = conf;

    trt::Controller c;
    c.spawn_scheduler(compare ? t_main_compare<RT> : t_main<RT>,
                      &arg, trt::TaskType::TASK);
    c.wait_for_all();
    return compare_failed_g ? 1 : 0;
}

#if defined(UDEPOT_TRT_SPDK)
// SPDK runs on DPDK lcores rather than plain pthreads, so it needs an
// RteController and a worker lcore -- mirroring udepot-test's SPDK path.
template<typename RT>
static int run_trt_spdk(KV_conf *conf, bool compare)
{
    main_arg arg;
    arg.kv   = nullptr;
    arg.conf = conf;

    RT::IO::global_init();

    trt::RteController c;
    const unsigned lcores_nr = rte_lcore_count();
    if (lcores_nr < 2) {
        fprintf(stderr,
                "SPDK needs at least 2 lcores (got %u): widen the DPDK core "
                "mask, e.g. -c 0x3\n", lcores_nr);
        return 1;
    }

    unsigned lcore;
    bool spawned = false;
    RTE_LCORE_FOREACH_WORKER(lcore) {
        c.spawn_scheduler(compare ? t_main_compare<RT> : t_main<RT>,
                          &arg, trt::TaskType::TASK, lcore);
        spawned = true;
        break;  // single scheduler: the comparison is single-threaded
    }
    if (!spawned) {
        fprintf(stderr, "no worker lcore available for SPDK\n");
        return 1;
    }
    c.wait_for_all();
    return compare_failed_g ? 1 : 0;
}
#endif

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mbuff|--copy|--compare [--aio|--uring] [options]\n"
        "  --compare      run both interfaces alternately and check that the\n"
        "                 zero-copy one is not slower; non-zero exit on failure\n"
        "  --mbuff        zero-copy Mbuff KV interface (get/put_test_thin_mbuff)\n"
        "  --copy         non-zero-copy raw buffer KV interface (get/put_test_thin)\n"
        "  --aio          TRT AIO backend (default)\n"
        "  --uring        TRT io_uring backend\n"
        "  --spdk         TRT SPDK backend (needs BUILD_SPDK=1)\n"
        "  --nvmef A:P:NQN  probe an NVMe-oF target instead of local\n"
        "                 PCIe NVMe, so SPDK can run without hardware\n"
        "  -f FILE        KV store file (default: /tmp/io-layer-bench.udepot)\n"
        "  -n NOPS        operations per phase (default: %lu)\n"
        "  --val-size N   value size in bytes (default: %u)\n"
        "  --trt-ntasks N tasks per scheduler (default: %zu)\n"
        "  -i N           paired iterations for --compare (default: %zu)\n"
        "  --grain-size N grain size in bytes (default: %lu; must be >= the\n"
        "                 device sector size for O_DIRECT backends)\n"
        "  --segment-size N segment size in grains (default: %lu)\n",
        prog, (unsigned long)bconf_g.nops, bconf_g.val_size, bconf_g.ntasks,
        bconf_g.iterations, (unsigned long)bconf_g.grain_size,
        (unsigned long)bconf_g.segment_size);
    exit(1);
}

int main(int argc, char *argv[])
{
    bool mode_set = false;
    bool compare = false;
    enum { BE_AIO, BE_URING, BE_SPDK } backend = BE_AIO;
    std::string nvmef;  // traddr:trsvcid:subnqn
    std::string fname = "/tmp/io-layer-bench.udepot";
    bool fname_set = false;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--mbuff") {
            bconf_g.mode = Mode::MBUFF; mode_set = true;
        } else if (a == "--copy") {
            bconf_g.mode = Mode::COPY;  mode_set = true;
        } else if (a == "--compare") {
            compare = true; mode_set = true;
        } else if (a == "--aio") {
            backend = BE_AIO;
        } else if (a == "--uring") {
            backend = BE_URING;
        } else if (a == "--spdk") {
            backend = BE_SPDK;
        } else if (a == "--nvmef" && i + 1 < argc) {
            nvmef = argv[++i];
        } else if (a == "-f" && i + 1 < argc) {
            fname = argv[++i];
            fname_set = true;
        } else if (a == "-n" && i + 1 < argc) {
            bconf_g.nops = std::stoul(argv[++i]);
        } else if (a == "--val-size" && i + 1 < argc) {
            bconf_g.val_size = (u32)std::stoul(argv[++i]);
        } else if (a == "--trt-ntasks" && i + 1 < argc) {
            bconf_g.ntasks = std::stoul(argv[++i]);
        } else if (a == "-i" && i + 1 < argc) {
            bconf_g.iterations = std::stoul(argv[++i]);
        } else if (a == "--grain-size" && i + 1 < argc) {
            bconf_g.grain_size = std::stoul(argv[++i]);
        } else if (a == "--segment-size" && i + 1 < argc) {
            bconf_g.segment_size = std::stoul(argv[++i]);
        } else {
            usage(argv[0]);
        }
    }

    if (!mode_set)
        usage(argv[0]);

    // SPDK addresses a namespace, not a path: there is no file to unlink and
    // nothing to ftruncate, so the store takes the device's own size. An empty
    // name selects the first namespace found (local PCIe or fabrics).
    const bool spdk_dev = (backend == BE_SPDK);
    if (spdk_dev) {
        if (!fname_set)
            fname.clear();
    } else {
        unlink(fname.c_str());
    }

    KV_conf conf(fname,
                 spdk_dev ? 0 : (1048576UL + 4096UL) * 1024UL + 1UL, /* size */
                 true,                  /* force destroy */
                 bconf_g.grain_size,    /* grain size, bytes */
                 bconf_g.segment_size   /* segment size, grains */);
    conf.type_m = (backend == BE_URING) ? KV_conf::KV_UDEPOT_SALSA_TRT_URING
                                       : KV_conf::KV_UDEPOT_SALSA_TRT_AIO;
    conf.thread_nr_m = 1;
    conf.validate_and_sanitize_parameters();

    printf("f:%s mode:%s backend:%s nops:%lu val_size:%u\n",
           fname.c_str(),
           compare ? "compare" : (bconf_g.mode == Mode::MBUFF ? "mbuff" : "copy"),
           backend == BE_SPDK ? "spdk" : (backend == BE_URING ? "uring" : "aio"),
           (unsigned long)bconf_g.nops, bconf_g.val_size);
    fflush(stdout);

    int err;
    if (backend == BE_SPDK) {
#if defined(UDEPOT_TRT_SPDK)
        // SPDK has no pathname: an empty device string takes the first
        // namespace found, whether local PCIe or an nvmf target.
        conf.type_m = KV_conf::KV_UDEPOT_SALSA_TRT_SPDK;
        if (!nvmef.empty()) {
            const size_t c1 = nvmef.find(':');
            const size_t c2 = nvmef.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) {
                fprintf(stderr,
                        "--nvmef expects traddr:trsvcid:subnqn (got '%s')\n",
                        nvmef.c_str());
                return 1;
            }
            TrtSpdkIO::add_nvmef_target(TrtSpdkIO::NvmefTransport::TCP,
                                        nvmef.substr(0, c1),
                                        nvmef.substr(c1 + 1, c2 - c1 - 1),
                                        nvmef.substr(c2 + 1));
        }
        err = run_trt_spdk<RuntimeTrtSpdk>(&conf, compare);
#else
        fprintf(stderr, "--spdk requires a BUILD_SPDK=1 build\n");
        return 77;  // distinct status: not built, as opposed to failed
#endif
    } else if (backend == BE_URING) {
        err = run_trt<RuntimeTrtUring>(&conf, compare);
    } else {
        err = run_trt<RuntimeTrt>(&conf, compare);
    }

    if (!spdk_dev)
        unlink(fname.c_str());
    return err;
}
