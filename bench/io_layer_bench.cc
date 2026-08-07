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
 */

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "kv.hh"
#include "trt_util/timer.h"
#include "util/types.h"
#include "uDepot/backend.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"
#include "uDepot/mbuff.hh"
#include "uDepot/udepot-lsa.hh"

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
};

static bench_conf bconf_g;

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

template<typename RT>
static int run_trt(KV_conf *conf)
{
    main_arg arg;
    arg.kv   = nullptr;
    arg.conf = conf;

    trt::Controller c;
    c.spawn_scheduler(t_main<RT>, &arg, trt::TaskType::TASK);
    c.wait_for_all();
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mbuff|--copy [--aio|--uring] [options]\n"
        "  --mbuff        zero-copy Mbuff KV interface (get/put_test_thin_mbuff)\n"
        "  --copy         non-zero-copy raw buffer KV interface (get/put_test_thin)\n"
        "  --aio          TRT AIO backend (default)\n"
        "  --uring        TRT io_uring backend\n"
        "  -f FILE        KV store file (default: /tmp/io-layer-bench.udepot)\n"
        "  -n NOPS        operations per phase (default: %lu)\n"
        "  --val-size N   value size in bytes (default: %u)\n"
        "  --trt-ntasks N tasks per scheduler (default: %zu)\n",
        prog, (unsigned long)bconf_g.nops, bconf_g.val_size, bconf_g.ntasks);
    exit(1);
}

int main(int argc, char *argv[])
{
    bool mode_set = false;
    bool use_uring = false;
    std::string fname = "/tmp/io-layer-bench.udepot";

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--mbuff") {
            bconf_g.mode = Mode::MBUFF; mode_set = true;
        } else if (a == "--copy") {
            bconf_g.mode = Mode::COPY;  mode_set = true;
        } else if (a == "--aio") {
            use_uring = false;
        } else if (a == "--uring") {
            use_uring = true;
        } else if (a == "-f" && i + 1 < argc) {
            fname = argv[++i];
        } else if (a == "-n" && i + 1 < argc) {
            bconf_g.nops = std::stoul(argv[++i]);
        } else if (a == "--val-size" && i + 1 < argc) {
            bconf_g.val_size = (u32)std::stoul(argv[++i]);
        } else if (a == "--trt-ntasks" && i + 1 < argc) {
            bconf_g.ntasks = std::stoul(argv[++i]);
        } else {
            usage(argv[0]);
        }
    }

    if (!mode_set)
        usage(argv[0]);

    unlink(fname.c_str());

    KV_conf conf(fname,
                 (1048576UL + 4096UL) * 1024UL + 1UL, /* size */
                 true,   /* force destroy */
                 32,     /* grain size */
                 4096    /* segment size */);
    conf.type_m = use_uring ? KV_conf::KV_UDEPOT_SALSA_TRT_URING
                            : KV_conf::KV_UDEPOT_SALSA_TRT_AIO;
    conf.thread_nr_m = 1;
    conf.validate_and_sanitize_parameters();

    printf("f:%s mode:%s backend:%s nops:%lu val_size:%u\n",
           fname.c_str(),
           bconf_g.mode == Mode::MBUFF ? "mbuff" : "copy",
           use_uring ? "uring" : "aio",
           (unsigned long)bconf_g.nops, bconf_g.val_size);
    fflush(stdout);

    int err = use_uring ? run_trt<RuntimeTrtUring>(&conf)
                        : run_trt<RuntimeTrt>(&conf);

    unlink(fname.c_str());
    return err;
}
