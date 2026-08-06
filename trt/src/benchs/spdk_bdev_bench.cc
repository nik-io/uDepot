/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Performance benchmark for SPDK bdev_malloc (in-memory block device).
 *  Measures random 4K read throughput through the bdev abstraction layer.
 *  Output format matches trt_aio_bench / trt_uring_bench for the regression
 *  test harness.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cinttypes>

extern "C" {
#include <spdk/stdinc.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/thread.h>
#include <bdev_malloc.h>
}

static const uint64_t NUM_BLOCKS   = 32768;  // 128 MiB
static const uint32_t BLOCK_SIZE   = 4096;
static const uint64_t NOPS         = 1000000;
static const uint64_t WARMUP_OPS   = 100000;
static const uint32_t QUEUE_DEPTH  = 64;

struct IoSlot;

struct BenchCtx {
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *ch;

    IoSlot *slots;
    uint32_t queue_depth;
    uint32_t buf_size;
    uint64_t num_blocks;
    uint64_t nops;
    uint64_t warmup_nops;
    bool warming_up;

    uint64_t ops_submitted;
    uint64_t ops_completed;

    uint32_t *free_stack;
    uint32_t free_top;

    struct timespec start_time;
    unsigned int rand_state;
};

struct IoSlot {
    BenchCtx *ctx;
    uint32_t idx;
    char *buf;
};

static void event_cb(enum spdk_bdev_event_type, struct spdk_bdev *, void *) {}

static void cleanup_and_stop(BenchCtx *ctx, int rc) {
    if (ctx->slots) {
        for (uint32_t i = 0; i < ctx->queue_depth; i++) {
            if (ctx->slots[i].buf) spdk_dma_free(ctx->slots[i].buf);
        }
        delete[] ctx->slots;
    }
    delete[] ctx->free_stack;
    if (ctx->ch) spdk_put_io_channel(ctx->ch);
    if (ctx->desc) spdk_bdev_close(ctx->desc);
    spdk_app_stop(rc);
}

static void submit_ios(BenchCtx *ctx);

static void io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    auto *slot = static_cast<IoSlot *>(cb_arg);
    BenchCtx *ctx = slot->ctx;
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        fprintf(stderr, "I/O error at op %" PRIu64 "\n", ctx->ops_completed);
        cleanup_and_stop(ctx, -1);
        return;
    }

    ctx->free_stack[ctx->free_top++] = slot->idx;
    ctx->ops_completed++;

    if (ctx->warming_up) {
        if (ctx->ops_completed == ctx->warmup_nops) {
            ctx->ops_submitted = 0;
            ctx->ops_completed = 0;
            ctx->warming_up = false;
            fprintf(stderr, "Warmup complete (%" PRIu64 " ops), measuring...\n",
                    ctx->warmup_nops);
            clock_gettime(CLOCK_MONOTONIC, &ctx->start_time);
        }
        submit_ios(ctx);
        return;
    }

    if (ctx->ops_completed == ctx->nops) {
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        double secs = static_cast<double>(end.tv_sec - ctx->start_time.tv_sec) +
                      static_cast<double>(end.tv_nsec - ctx->start_time.tv_nsec) / 1e9;

        double kops = static_cast<double>(ctx->nops) / (secs * 1000.0);
        double mibs = static_cast<double>(ctx->nops) * ctx->buf_size / (secs * 1024 * 1024);

        printf("time=%.6fs Kops/sec=%.2f MiB/sec=%.2f\n", secs, kops, mibs);
        cleanup_and_stop(ctx, 0);
        return;
    }

    submit_ios(ctx);
}

static void submit_ios(BenchCtx *ctx) {
    uint64_t target = ctx->warming_up ? ctx->warmup_nops : ctx->nops;
    while (ctx->free_top > 0 && ctx->ops_submitted < target) {
        uint32_t slot_idx = ctx->free_stack[--ctx->free_top];
        IoSlot *slot = &ctx->slots[slot_idx];

        uint64_t block = rand_r(&ctx->rand_state) % ctx->num_blocks;
        uint64_t offset = block * ctx->buf_size;

        int rc = spdk_bdev_read(ctx->desc, ctx->ch, slot->buf,
                                offset, ctx->buf_size, io_complete, slot);
        if (rc == -ENOMEM) {
            ctx->free_stack[ctx->free_top++] = slot_idx;
            break;
        } else if (rc) {
            fprintf(stderr, "spdk_bdev_read error: %d\n", rc);
            cleanup_and_stop(ctx, -1);
            return;
        }

        ctx->ops_submitted++;
    }
}

static void bench_start(void *arg) {
    auto *ctx = static_cast<BenchCtx *>(arg);

    struct malloc_bdev_opts mopts = {};
    char name[] = "BenchMalloc0";
    mopts.name = name;
    mopts.num_blocks = ctx->num_blocks;
    mopts.block_size = BLOCK_SIZE;

    int rc = create_malloc_disk(&ctx->bdev, &mopts);
    if (rc) {
        fprintf(stderr, "create_malloc_disk failed: %d\n", rc);
        spdk_app_stop(-1);
        return;
    }

    rc = spdk_bdev_open_ext("BenchMalloc0", false, event_cb, nullptr, &ctx->desc);
    if (rc) {
        fprintf(stderr, "spdk_bdev_open_ext failed: %d\n", rc);
        spdk_app_stop(-1);
        return;
    }

    ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
    if (!ctx->ch) {
        fprintf(stderr, "could not get I/O channel\n");
        spdk_bdev_close(ctx->desc);
        spdk_app_stop(-1);
        return;
    }

    uint32_t align = spdk_bdev_get_buf_align(ctx->bdev);
    ctx->slots = new IoSlot[ctx->queue_depth]();
    ctx->free_stack = new uint32_t[ctx->queue_depth];
    ctx->free_top = ctx->queue_depth;

    for (uint32_t i = 0; i < ctx->queue_depth; i++) {
        ctx->slots[i].ctx = ctx;
        ctx->slots[i].idx = i;
        ctx->slots[i].buf = static_cast<char *>(
            spdk_dma_zmalloc(ctx->buf_size, align, nullptr));
        if (!ctx->slots[i].buf) {
            fprintf(stderr, "buffer allocation failed at slot %u\n", i);
            cleanup_and_stop(ctx, -1);
            return;
        }
        ctx->free_stack[i] = i;
    }

    fprintf(stderr, "SPDK bdev_malloc benchmark: %" PRIu64 " ops, %u bytes, "
            "QD %u, device %" PRIu64 " MiB, warmup %" PRIu64 " ops\n",
            ctx->nops, ctx->buf_size, ctx->queue_depth,
            ctx->num_blocks * BLOCK_SIZE / (1024 * 1024),
            ctx->warmup_nops);

    ctx->rand_state = 42;
    ctx->warming_up = true;
    submit_ios(ctx);
}

int main(int argc, char *argv[]) {
    struct spdk_app_opts opts = {};
    BenchCtx ctx = {};

    ctx.num_blocks = NUM_BLOCKS;
    ctx.nops = NOPS;
    ctx.warmup_nops = WARMUP_OPS;
    ctx.queue_depth = QUEUE_DEPTH;
    ctx.buf_size = BLOCK_SIZE;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "spdk_bdev_bench";
    opts.rpc_addr = nullptr;
    opts.no_pci = true;
    opts.no_huge = true;
    opts.mem_size = 1024;

    struct spdk_iobuf_opts iobuf_opts = {};
    spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));
    iobuf_opts.small_pool_count = 256;
    iobuf_opts.large_pool_count = 64;
    spdk_iobuf_set_opts(&iobuf_opts);

    int rc = spdk_app_start(&opts, bench_start, &ctx);
    spdk_app_fini();
    return rc;
}
