/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <algorithm>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_uring.hh"

using namespace trt;

#define FILE_SIZE (128*1024*1024)
#define FILE_FLAGS (O_DIRECT)
#define BUFF_SIZE (4096)

#define NOPS_TOTAL  (1024*1024)
#define NOPS_WARMUP 100000
#define NOPS_BATCH  64

struct targ;

struct io_slot {
    struct targ *targ;
    char *buf;
    uint32_t idx;
};

struct targ {
    int fd;
    bool verify;
    size_t file_size;
    size_t buff_size;
    size_t io_completed;

    struct io_slot *slots;
    uint32_t *slot_stack;
    uint32_t slot_top;
    uint32_t pool_size;
};

static int
make_tempfile(char *tmpname, size_t file_size, size_t buff_size) {
    int fd;
    char *buff;

    printf("Creating random file\n");
    fd = mkostemp(tmpname, FILE_FLAGS);
    if (fd < 0) { perror("mkostemp"); exit(1); }

    int ret = ftruncate(fd, file_size);
    if (ret < 0) { perror("ftruncate"); exit(1); }

    if (posix_memalign((void **)&buff, 4096, buff_size) != 0) {
        perror("posix_memalign");
        exit(1);
    }

    for (size_t off = 0; off < file_size; off += buff_size) {
        for (size_t i = 0; i < buff_size; i++)
            buff[i] = 'a' + ((off / buff_size) % ('z' + 1));
        ret = pwrite(fd, buff, buff_size, off);
        if (ret != (int)buff_size) { perror("pwrite"); exit(1); }
    }

    free(buff);
    return fd;
}

CoroTask t_io(void *arg__) {
    struct io_slot *slot = (struct io_slot *)arg__;
    struct targ *arg = slot->targ;
    const size_t nchunks = arg->file_size / arg->buff_size;
    size_t chunk = rand() % nchunks;

    int ret = co_await IOU::pread(arg->fd, slot->buf, arg->buff_size,
                                  chunk * arg->buff_size);
    if (ret < 0) {
        fprintf(stderr, "err=%d\n", ret);
        perror("iou_pread");
        exit(1);
    }
    assert(ret == (int)arg->buff_size);

    if (arg->verify) {
        const char c = 'a' + (chunk % ('z' + 1));
        for (size_t i = 0; i < arg->buff_size; i++) {
            if (slot->buf[i] != c) { fprintf(stderr, "Error!"); exit(1); }
        }
    }

    arg->slot_stack[arg->slot_top++] = slot->idx;
    arg->io_completed++;
    co_return 0;
}

CoroTask t_main_detached(void *arg__) {
    struct targ *arg = (struct targ *)arg__;
    const size_t nops_total = NOPS_TOTAL;
    const size_t nops_warmup = NOPS_WARMUP;
    const size_t nops_batch = NOPS_BATCH;
    size_t nops_submitted;

    // Warmup phase — exercise I/O path and page cache before timing
    nops_submitted = 0;
    while (nops_submitted < nops_warmup) {
        if (arg->slot_top == 0) {
            co_await T::yield();
            continue;
        }
        size_t batch = std::min({(size_t)arg->slot_top, nops_batch,
                                 nops_warmup - nops_submitted});
        for (size_t i = 0; i < batch; i++) {
            uint32_t idx = arg->slot_stack[--arg->slot_top];
            co_await T::spawn(t_io, &arg->slots[idx], nullptr, true);
            nops_submitted++;
        }
    }
    while (arg->io_completed < nops_warmup) {
        co_await T::yield();
    }
    fprintf(stderr, "Warmup complete (%zu ops), measuring...\n", nops_warmup);

    // Measurement phase
    arg->io_completed = 0;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    nops_submitted = 0;
    while (nops_submitted < nops_total) {
        if (arg->slot_top == 0) {
            co_await T::yield();
            continue;
        }
        size_t batch = std::min({(size_t)arg->slot_top, nops_batch,
                                 nops_total - nops_submitted});
        for (size_t i = 0; i < batch; i++) {
            uint32_t idx = arg->slot_stack[--arg->slot_top];
            co_await T::spawn(t_io, &arg->slots[idx], nullptr, true);
            nops_submitted++;
        }
    }
    while (arg->io_completed < nops_total) {
        co_await T::yield();
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double s = (double)(ts_end.tv_sec - ts_start.tv_sec) +
               (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    printf("time=%lfs Kops/sec=%lf MiB/sec=%lf\n", s,
           nops_total / (s * 1000),
           (1.0 * nops_total * BUFF_SIZE) / (s * 1024 * 1024));

    co_await T::yield();
    IOU::stop();
    trt_dmsg("%s: DONE\n", __FUNCTION__);
    co_return 0;
}

CoroTask t_init(void *arg__)
{
    IOU::init();
    struct targ *arg_ = (struct targ *)arg__;
    static struct targ arg = *arg_;
    for (uint32_t i = 0; i < arg.pool_size; i++)
        arg.slots[i].targ = &arg;
    trt_dmsg("%s\n", __FUNCTION__);

    trt_dmsg("spawning iou_poller\n");
    T::spawn_detached_no_wait(IOU::poller_task, nullptr, TaskType::TASK);
    trt_dmsg("spawning t_main_detached\n");
    co_await T::spawn(t_main_detached, &arg, nullptr, false, TaskType::POLL);
    trt_dmsg("waiting main\n");
    co_await T::task_wait();

    trt_dmsg("%s: DONE\n", __FUNCTION__);
    co_return 0;
}

int main(int argc, char *argv[])
{
    struct targ arg = {};
    char tempname[] = "/tmp/trt-iou-bench-test-XXXXXX";
    char *fname;
    bool created_temp = false;

    if (argc < 2) {
        fname = tempname;
        int fd = make_tempfile(fname, FILE_SIZE, BUFF_SIZE);
        arg.fd        = fd;
        arg.file_size = FILE_SIZE;
        arg.verify    = true;
        created_temp  = true;
    } else {
        fname = argv[1];
        int fd = open(fname, O_RDONLY | FILE_FLAGS);
        if (fd == -1) { perror(fname); exit(1); }

        size_t fsize = lseek64(fd, 0, SEEK_END);
        if (fsize == (size_t)-1) { perror("lseek64"); exit(1); }
        lseek(fd, 0, SEEK_SET);

        arg.fd        = fd;
        arg.file_size = fsize;
        arg.verify    = false;
    }

    arg.buff_size = BUFF_SIZE;
    arg.pool_size = NOPS_BATCH;
    arg.slots = new io_slot[NOPS_BATCH]();
    arg.slot_stack = new uint32_t[NOPS_BATCH];
    arg.slot_top = NOPS_BATCH;
    for (uint32_t i = 0; i < NOPS_BATCH; i++) {
        arg.slots[i].idx = i;
        if (posix_memalign((void **)&arg.slots[i].buf, 4096, BUFF_SIZE) != 0) {
            perror("posix_memalign");
            exit(1);
        }
        arg.slot_stack[i] = i;
    }

    printf("f:%s s:%zd verify:%u\n", fname, arg.file_size, arg.verify);

    Controller c;
    c.spawn_scheduler(t_init, &arg, TaskType::TASK, 2);
    c.set_exit_all();
    c.wait_for_all();

    close(arg.fd);
    for (uint32_t i = 0; i < NOPS_BATCH; i++)
        free(arg.slots[i].buf);
    delete[] arg.slots;
    delete[] arg.slot_stack;
    if (created_temp)
        unlink(fname);

    return 0;
}
