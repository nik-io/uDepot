/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Unit test for SPDK bdev API using bdev_malloc (in-memory block device).
 *  Verifies write/read correctness through the bdev abstraction layer
 *  without requiring real NVMe hardware.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <spdk/stdinc.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/thread.h>
#include <bdev_malloc.h>
}

static const uint64_t NUM_BLOCKS = 256;
static const uint32_t BLOCK_SIZE = 4096;
static const char *BDEV_NAME = "TestMalloc0";

struct TestCtx {
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *ch;
    char *buf;
    uint32_t buf_size;
    int phase;
    int result;
    uint64_t block_idx;
};

static void cleanup_and_stop(TestCtx *ctx) {
    if (ctx->buf) spdk_dma_free(ctx->buf);
    if (ctx->ch) spdk_put_io_channel(ctx->ch);
    if (ctx->desc) spdk_bdev_close(ctx->desc);
    spdk_app_stop(ctx->result);
}

static void event_cb(enum spdk_bdev_event_type, struct spdk_bdev *, void *) {}

static void run_write(TestCtx *ctx);
static void run_read_verify(TestCtx *ctx);

static void read_verify_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    auto *ctx = static_cast<TestCtx *>(cb_arg);
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        fprintf(stderr, "FAIL: read failed at block %lu\n", ctx->block_idx);
        ctx->result = 1;
        cleanup_and_stop(ctx);
        return;
    }

    uint8_t expected;
    if (ctx->phase == 1) expected = 0xAB;
    else if (ctx->phase == 2) expected = 0xCD;
    else expected = 0xEF;

    for (uint32_t i = 0; i < ctx->buf_size; i++) {
        if (static_cast<uint8_t>(ctx->buf[i]) != expected) {
            fprintf(stderr, "FAIL: data mismatch at block %lu byte %u: "
                    "expected 0x%02X got 0x%02X\n",
                    ctx->block_idx, i, expected,
                    static_cast<uint8_t>(ctx->buf[i]));
            ctx->result = 1;
            cleanup_and_stop(ctx);
            return;
        }
    }

    ctx->block_idx++;

    if (ctx->phase == 1) {
        printf("  PASS: single block write/read (pattern 0xAB)\n");
        ctx->phase = 2;
        ctx->block_idx = 0;
        run_write(ctx);
    } else if (ctx->phase == 2) {
        if (ctx->block_idx < 16) {
            run_read_verify(ctx);
        } else {
            printf("  PASS: multi-block write/read (16 blocks, pattern 0xCD)\n");
            ctx->phase = 3;
            ctx->block_idx = 0;
            run_write(ctx);
        }
    } else {
        printf("  PASS: overwrite verification (pattern 0xEF)\n");
        printf("\nAll tests passed.\n");
        ctx->result = 0;
        cleanup_and_stop(ctx);
    }
}

static void run_read_verify(TestCtx *ctx) {
    memset(ctx->buf, 0, ctx->buf_size);
    uint64_t offset = ctx->block_idx * ctx->buf_size;
    int rc = spdk_bdev_read(ctx->desc, ctx->ch, ctx->buf, offset,
                            ctx->buf_size, read_verify_cb, ctx);
    if (rc) {
        fprintf(stderr, "FAIL: spdk_bdev_read returned %d\n", rc);
        ctx->result = 1;
        cleanup_and_stop(ctx);
    }
}

static void write_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    auto *ctx = static_cast<TestCtx *>(cb_arg);
    spdk_bdev_free_io(bdev_io);

    if (!success) {
        fprintf(stderr, "FAIL: write failed at block %lu\n", ctx->block_idx);
        ctx->result = 1;
        cleanup_and_stop(ctx);
        return;
    }

    if (ctx->phase == 1) {
        run_read_verify(ctx);
    } else if (ctx->phase == 2) {
        ctx->block_idx++;
        if (ctx->block_idx < 16) {
            run_write(ctx);
        } else {
            ctx->block_idx = 0;
            run_read_verify(ctx);
        }
    } else {
        run_read_verify(ctx);
    }
}

static void run_write(TestCtx *ctx) {
    uint8_t pattern;
    if (ctx->phase == 1) pattern = 0xAB;
    else if (ctx->phase == 2) pattern = 0xCD;
    else pattern = 0xEF;

    memset(ctx->buf, pattern, ctx->buf_size);
    uint64_t offset = ctx->block_idx * ctx->buf_size;
    int rc = spdk_bdev_write(ctx->desc, ctx->ch, ctx->buf, offset,
                             ctx->buf_size, write_cb, ctx);
    if (rc) {
        fprintf(stderr, "FAIL: spdk_bdev_write returned %d\n", rc);
        ctx->result = 1;
        cleanup_and_stop(ctx);
    }
}

static void test_start(void *arg) {
    auto *ctx = static_cast<TestCtx *>(arg);

    printf("=== SPDK bdev_malloc unit test ===\n\n");

    struct malloc_bdev_opts mopts = {};
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s", BDEV_NAME);
    mopts.name = name_buf;
    mopts.num_blocks = NUM_BLOCKS;
    mopts.block_size = BLOCK_SIZE;

    struct spdk_bdev *bdev = nullptr;
    int rc = create_malloc_disk(&bdev, &mopts);
    if (rc) {
        fprintf(stderr, "FAIL: create_malloc_disk returned %d\n", rc);
        spdk_app_stop(-1);
        return;
    }
    ctx->bdev = bdev;
    printf("Created malloc bdev: %s (%lu blocks x %u bytes = %lu KiB)\n",
           BDEV_NAME, NUM_BLOCKS, BLOCK_SIZE,
           NUM_BLOCKS * BLOCK_SIZE / 1024);

    rc = spdk_bdev_open_ext(BDEV_NAME, true, event_cb, nullptr, &ctx->desc);
    if (rc) {
        fprintf(stderr, "FAIL: spdk_bdev_open_ext returned %d\n", rc);
        spdk_app_stop(-1);
        return;
    }

    ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
    if (!ctx->ch) {
        fprintf(stderr, "FAIL: could not get I/O channel\n");
        spdk_bdev_close(ctx->desc);
        spdk_app_stop(-1);
        return;
    }

    ctx->buf_size = BLOCK_SIZE;
    uint32_t align = spdk_bdev_get_buf_align(ctx->bdev);
    ctx->buf = static_cast<char *>(spdk_dma_zmalloc(ctx->buf_size, align, nullptr));
    if (!ctx->buf) {
        fprintf(stderr, "FAIL: spdk_dma_zmalloc failed\n");
        ctx->result = 1;
        cleanup_and_stop(ctx);
        return;
    }

    ctx->phase = 1;
    ctx->block_idx = 0;
    ctx->result = 1;
    run_write(ctx);
}

int main(int argc, char *argv[]) {
    struct spdk_app_opts opts = {};
    TestCtx ctx = {};

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "spdk_bdev_test";
    opts.rpc_addr = nullptr;
    opts.no_pci = true;
    opts.no_huge = true;
    opts.mem_size = 1024;

    struct spdk_iobuf_opts iobuf_opts = {};
    spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));
    iobuf_opts.small_pool_count = 256;
    iobuf_opts.large_pool_count = 64;
    spdk_iobuf_set_opts(&iobuf_opts);

    int rc = spdk_app_start(&opts, test_start, &ctx);
    spdk_app_fini();
    return rc ? rc : ctx.result;
}
