/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Benchmarks the uDepot I/O layer in two modes:
 *    --mbuff: zero-copy via io_pread_mbuff_append() into Mbuff segments
 *    --copy:  non-zero-copy via io_pread_mbuff_append() + copy_to_buffer()
 *
 *  Both paths perform I/O through the Mbuff API (matching udepot-test.cc).
 *  The only difference is the extra copy_to_buffer() in the non-zero-copy
 *  path, mirroring the KV store's internal copy from its IO Mbuff to the
 *  user's raw buffer in the non-mbuff get() interface.
 *
 *  Supports --aio (default) and --uring backends.
 *  Output format matches trt_aio_bench / trt_uring_bench for the
 *  regression test harness.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <typeindex>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"
#include "trt_backends/trt_uring.hh"

#include "uDepot/mbuff.hh"
#include "uDepot/io.hh"
#include "uDepot/io/helpers.hh"
#include "uDepot/io/trt-aio.hh"
#include "uDepot/io/trt-uring.hh"

using namespace trt;
using IO = udepot::TrtFileIO;
using IO_Uring = udepot::TrtFileIOUring;

#define FILE_SIZE   (128*1024*1024)
#define BUFF_SIZE   (4096)
#define NOPS_TOTAL  (1024*1024)
#define NOPS_WARMUP 100000
#define NOPS_BATCH  64

enum class Mode { MBUFF, COPY };
enum class Backend { AIO, URING };

struct targ;

struct io_slot {
    struct targ *targ;
    udepot::Mbuff *mb;
    char *io_buf;
    char *dst_buf;
    uint32_t idx;
};

struct targ {
    IO *io;
    Mode mode;
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
    fd = mkostemp(tmpname, O_DIRECT);
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

// Zero-copy path: read directly into the user's Mbuff.
// Mirrors get_test_thin_mbuff in udepot-test.cc — the KV store reads
// directly into the user-provided Mbuff, no intermediate copy.
CoroTask t_io_mbuff(void *arg__) {
    struct io_slot *slot = (struct io_slot *)arg__;
    struct targ *arg = slot->targ;
    const size_t nchunks = arg->file_size / arg->buff_size;
    size_t chunk = rand() % nchunks;

    slot->mb->reslice(0);

    ssize_t ret = (ssize_t)(co_await udepot::io_pread_mbuff_append(
        *arg->io, *slot->mb, arg->buff_size, (off_t)(chunk * arg->buff_size)));
    if (ret < 0) {
        fprintf(stderr, "io_pread_mbuff_append err=%zd\n", ret);
        exit(1);
    }
    assert(ret == (ssize_t)arg->buff_size);

    arg->slot_stack[arg->slot_top++] = slot->idx;
    arg->io_completed++;
    co_return 0;
}

// Non-zero-copy path: read into an internal Mbuff, then copy out to the
// user's raw buffer.
// Mirrors get_test_thin in udepot-test.cc — the KV store reads into its
// own internal Mbuff, then copies the value into the caller's char* buffer.
CoroTask t_io_copy(void *arg__) {
    struct io_slot *slot = (struct io_slot *)arg__;
    struct targ *arg = slot->targ;
    const size_t nchunks = arg->file_size / arg->buff_size;
    size_t chunk = rand() % nchunks;

    slot->mb->reslice(0);

    ssize_t ret = (ssize_t)(co_await udepot::io_pread_mbuff_append(
        *arg->io, *slot->mb, arg->buff_size, (off_t)(chunk * arg->buff_size)));
    if (ret < 0) {
        fprintf(stderr, "io_pread_mbuff_append err=%zd\n", ret);
        exit(1);
    }
    assert(ret == (ssize_t)arg->buff_size);

    slot->mb->copy_to_buffer(0, slot->dst_buf, arg->buff_size);

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

    auto io_fn = (arg->mode == Mode::MBUFF) ? t_io_mbuff : t_io_copy;

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
            co_await T::spawn(io_fn, &arg->slots[idx], nullptr, true);
            nops_submitted++;
        }
    }
    while (arg->io_completed < nops_warmup) {
        co_await T::yield();
    }
    fprintf(stderr, "Warmup complete (%zu ops), measuring...\n", nops_warmup);

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
            co_await T::spawn(io_fn, &arg->slots[idx], nullptr, true);
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

    co_return 0;
}

CoroTask t_init_aio(void *arg__) {
    IO::thread_init();

    struct targ *arg_ = (struct targ *)arg__;
    static struct targ arg = *arg_;
    for (uint32_t i = 0; i < arg.pool_size; i++)
        arg.slots[i].targ = &arg;

    co_await T::spawn(t_main_detached, &arg, nullptr, false, TaskType::POLL);
    co_await T::task_wait();

    IO::thread_exit();
    co_return 0;
}

CoroTask t_init_uring(void *arg__) {
    IO_Uring::thread_init();

    struct targ *arg_ = (struct targ *)arg__;
    static struct targ arg = *arg_;
    for (uint32_t i = 0; i < arg.pool_size; i++)
        arg.slots[i].targ = &arg;

    co_await T::spawn(t_main_detached, &arg, nullptr, false, TaskType::POLL);
    co_await T::task_wait();

    IO_Uring::thread_exit();
    co_return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s --mbuff|--copy [--uring] [file]\n", prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    Mode mode = Mode::MBUFF;
    Backend backend = Backend::AIO;
    char *file_arg = nullptr;
    bool mode_set = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mbuff") == 0) {
            mode = Mode::MBUFF;
            mode_set = true;
        } else if (strcmp(argv[i], "--copy") == 0) {
            mode = Mode::COPY;
            mode_set = true;
        } else if (strcmp(argv[i], "--aio") == 0) {
            backend = Backend::AIO;
        } else if (strcmp(argv[i], "--uring") == 0) {
            backend = Backend::URING;
        } else if (argv[i][0] != '-') {
            file_arg = argv[i];
        } else {
            usage(argv[0]);
        }
    }

    if (!mode_set)
        usage(argv[0]);

    char tempname[] = "/tmp/io-layer-bench-XXXXXX";
    char *fname;
    size_t file_size;
    bool created_temp = false;

    if (!file_arg) {
        fname = tempname;
        make_tempfile(fname, FILE_SIZE, BUFF_SIZE);
        file_size = FILE_SIZE;
        created_temp = true;
    } else {
        fname = file_arg;
        struct stat st;
        if (stat(fname, &st) != 0) { perror(fname); exit(1); }
        file_size = st.st_size;
    }

    IO *io_aio = nullptr;
    IO_Uring *io_uring = nullptr;
    IO *io_base = nullptr;

    if (backend == Backend::AIO) {
        io_aio = new IO();
        int err = io_aio->open(fname, O_RDONLY | O_DIRECT, 0);
        if (err) { fprintf(stderr, "open: %s\n", strerror(err)); exit(1); }
        io_base = io_aio;
    } else {
        io_uring = new IO_Uring();
        int err = io_uring->open(fname, O_RDONLY | O_DIRECT, 0);
        if (err) { fprintf(stderr, "open: %s\n", strerror(err)); exit(1); }
        io_base = io_uring;
    }

    struct targ arg = {};
    arg.io = io_base;
    arg.mode = mode;
    arg.file_size = file_size;
    arg.buff_size = BUFF_SIZE;
    arg.pool_size = NOPS_BATCH;
    arg.slots = new io_slot[NOPS_BATCH]();
    arg.slot_stack = new uint32_t[NOPS_BATCH];
    arg.slot_top = NOPS_BATCH;

    for (uint32_t i = 0; i < NOPS_BATCH; i++) {
        arg.slots[i].idx = i;
        arg.slot_stack[i] = i;

        if (posix_memalign((void **)&arg.slots[i].io_buf, 4096, BUFF_SIZE) != 0) {
            perror("posix_memalign"); exit(1);
        }

        // Both paths use Mbuff for I/O (matching udepot-test.cc pattern)
        auto *mb = new udepot::Mbuff(std::type_index(typeid(IO::Ptr)));
        IO::Ptr ioptr(arg.slots[i].io_buf, BUFF_SIZE);
        mb->add_iobuff(ioptr, BUFF_SIZE);
        arg.slots[i].mb = mb;

        if (mode == Mode::COPY) {
            arg.slots[i].dst_buf = (char *)malloc(BUFF_SIZE);
            if (!arg.slots[i].dst_buf) { perror("malloc"); exit(1); }
        }
    }

    const char *mode_str = (mode == Mode::MBUFF) ? "mbuff" : "copy";
    const char *backend_str = (backend == Backend::AIO) ? "aio" : "uring";
    printf("f:%s s:%zu mode:%s backend:%s\n", fname, file_size, mode_str, backend_str);

    auto init_fn = (backend == Backend::AIO) ? t_init_aio : t_init_uring;

    Controller c;
    c.spawn_scheduler(init_fn, &arg, TaskType::TASK, 2);
    c.set_exit_all();
    c.wait_for_all();

    for (uint32_t i = 0; i < NOPS_BATCH; i++) {
        arg.slots[i].mb->reset(
            [](void *) { /* io_buf freed below */ });
        delete arg.slots[i].mb;
        if (mode == Mode::COPY)
            free(arg.slots[i].dst_buf);
        free(arg.slots[i].io_buf);
    }
    delete[] arg.slots;
    delete[] arg.slot_stack;

    delete io_aio;
    delete io_uring;

    if (created_temp)
        unlink(fname);

    return 0;
}
