/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include <memory>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_spdk.hh"

#include <rte_config.h>
#include <rte_malloc.h>

using namespace trt;

struct GlobArg {
    SpdkGlobalState g_spdk;
    std::string     g_namepace;
    GlobArg(const char *n) : g_namepace(n) {}
    void init() { g_spdk.init(); }
};

static void
test_buff_and_vecs(char *buff, char **vec_buf, size_t nvecs, size_t vec_size) {
    size_t off = 0;
    for (size_t i = 0; i < nvecs; i++) {
        for (size_t j = 0; j < vec_size; j++, off++) {
            if (buff[off] != vec_buf[i][j]) {
                fprintf(stderr,
                    "Mismatch at vec=%zu byte=%zu (off=%zu): "
                    "buff=0x%02x vec=0x%02x\n",
                    i, j, off,
                    (unsigned char)buff[off],
                    (unsigned char)vec_buf[i][j]);
                abort();
            }
        }
    }
}

CoroTask
t_io(void *arg__) {
    SpdkQpair *qp = static_cast<SpdkQpair *>(arg__);

    const size_t nvecs    = 4;
    const size_t vec_size = qp->sqp_namespace->get_sector_size();
    const ssize_t total_size = nvecs * vec_size;

    char *buff = static_cast<char *>(rte_malloc(nullptr, total_size, 4096));
    assert(buff != nullptr);

    char *vec_buf[nvecs];
    struct iovec iovecs[nvecs];
    for (size_t i = 0; i < nvecs; i++) {
        vec_buf[i] = static_cast<char *>(rte_malloc(nullptr, vec_size, 4096));
        assert(vec_buf[i] != nullptr);
        iovecs[i].iov_base = vec_buf[i];
        iovecs[i].iov_len  = vec_size;
        for (size_t j = 0; j < vec_size; j++)
            vec_buf[i][j] = 'a' + ((i * vec_size + j) % ('z' - 'a' + 1));
    }

    /**
     * Test preadv: pwrite pattern + preadv into iovecs + verify
     */

    for (ssize_t i=0; i<total_size; i++)
        buff[i] = 'a' + (i % ('z' - 'a' + 1));

    trt_dmsg("%s: issuing pwrite()\n", __PRETTY_FUNCTION__);
    if (total_size != co_await SPDK::pwrite(qp, buff, total_size, 0)) {
        perror("pwrite");
        abort();
    }

    trt_dmsg("%s: issuing preadv()\n", __PRETTY_FUNCTION__);
    if (total_size != co_await SPDK::preadv(qp, iovecs, nvecs, 0)) {
        perror("preadv");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PREADV OK\n");

    /**
     * Test preadv: pwrite zeroes + pwritev + pread
     */

    for (ssize_t i=0; i<total_size; i++)
        buff[i] = 0;

    if (total_size != co_await SPDK::pwrite(qp, buff, total_size, 0)) {
        perror("pwrite");
        abort();
    }

    if (total_size != co_await SPDK::pwritev(qp, iovecs, nvecs, 0)) {
        perror("preadv");
        abort();
    }

    if (total_size != co_await SPDK::pread(qp, buff, total_size, 0)) {
        perror("pread");
        abort();
    }

    test_buff_and_vecs(buff, vec_buf, nvecs, vec_size);
    printf("PWRITEV OK\n");

    for (size_t i=0; i<nvecs; i++) {
        rte_free(vec_buf[i]);
    }
    rte_free(buff);

    trt_dmsg("IO task END\n");
    co_return 0;
}


CoroTask
t_init(void *arg__) {

    trt_dmsg("%s: enter\n", __PRETTY_FUNCTION__);
    GlobArg *arg = static_cast<GlobArg *>(arg__);

    SPDK::init(arg->g_spdk);
    std::shared_ptr<SpdkQpair> qp = SPDK::getQpair(arg->g_namepace);

    trt_dmsg("Spawning SPDK poller task\n");
    T::spawn_detached_no_wait(SPDK::poller_task, nullptr, TaskType::TASK);

    trt_dmsg("Spawning IO task\n");
    co_await T::spawn(t_io, qp.get());
    trt_dmsg("Waiting IO task\n");
    co_await T::task_wait();

    trt_dmsg("IO task DONE: Stopping SPDK queues\n");
    SPDK::stop();

    trt_dmsg("Notify scheduler to exit\n");
    T::set_exit_all();
    co_return 0;
}

int main(int argc, char *argv[])
{
    GlobArg g("");
    g.init();
    Controller c;

    c.spawn_scheduler(t_init, &g, TaskType::TASK);
    c.wait_for_all();

    return 0;
}
