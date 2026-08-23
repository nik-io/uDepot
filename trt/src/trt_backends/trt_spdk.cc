/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_spdk.hh"

#include <rte_lcore.h>

namespace trt {

thread_local SpdkState SpdkState__;

SpdkState *get_tls_SpdkState__() {
    return &SpdkState__;
}

// ─────────────────────────────────────────────────────────────────────────────
// SPDK IO callback — called from execute_completions() in poller_task context.
// Uses direct notify so the woken task is pushed to the run queue immediately.
// ─────────────────────────────────────────────────────────────────────────────
void
SPDK::spdk_io_cb(void *ctx, const struct spdk_nvme_cpl *cpl)
{
    LocalSingleAsyncObj *lsao = static_cast<LocalSingleAsyncObj *>(ctx);
    SpdkQpair *qp = (SpdkQpair *)lsao->lsao_user_data_;

    qp->npending_dec(1);
    T::io_npending_dec(1);

    RetT val = spdk_nvme_cpl_is_error(cpl) ? -1 : 0;
    T::local_single_notify(lsao, val);
}

// ─────────────────────────────────────────────────────────────────────────────
// LBA range helper
// ─────────────────────────────────────────────────────────────────────────────
static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
    size_t ret = 0;
    for (unsigned i = 0; i < iovcnt; i++)
        ret += iov[i].iov_len;
    return ret;
}

static inline std::tuple<uint64_t, uint64_t>
get_lba_range(uint64_t offset, uint64_t length, size_t block_size)
{
    size_t lba_start = offset / block_size;
    size_t lba_end   = (offset + length + block_size - 1) / block_size;
    return {lba_start, lba_end};
}

// ─────────────────────────────────────────────────────────────────────────────
// SpdkRawAwaitable — for SPDK::read / SPDK::write
// ─────────────────────────────────────────────────────────────────────────────
bool SpdkRawAwaitable::await_ready() noexcept
{
    lsao_.lsao_user_data_ = (uintptr_t)(qp_);
    int rc;
    if (op_ == Op::READ)
        rc = qp_->submit_read(buff_, lba_, lba_cnt_, SPDK::spdk_io_cb, &lsao_);
    else
        rc = qp_->submit_write(buff_, lba_, lba_cnt_, SPDK::spdk_io_cb, &lsao_);

    if (rc != 0) { rc_ = rc; ready_ = true; return true; }
    T::io_npending_inc(1);
    ready_ = lsao_.is_ready();
    return ready_;
}

bool SpdkRawAwaitable::await_suspend(std::coroutine_handle<> h) noexcept
{
    TaskBase *t = localScheduler__->current_task();
    t->set_current_coro(h);
    lsao_.set_waiter(t);
    return true;
}

ssize_t SpdkRawAwaitable::await_resume() noexcept
{
    if (rc_ != 0) return -1;
    RetT err = lsao_.get_ret();
    return err ? -1 : (ssize_t)lba_cnt_;
}

// ─────────────────────────────────────────────────────────────────────────────
// SpdkIOAwaitable — for SPDK::pread/pwrite/preadv/pwritev
// ─────────────────────────────────────────────────────────────────────────────
bool SpdkIOAwaitable::await_ready() noexcept
{
    uint64_t b = qp_->get_sector_size();
    bool is_write = (op_ == Op::PWRITE || op_ == Op::PWRITEV);

    // determine total length and lba range
    size_t total_len;
    if (op_ == Op::PREAD || op_ == Op::PWRITE) {
        total_len = len_;
    } else {
        total_len = iovec_len(iov_, iovcnt_);
    }
    iovlen_ = total_len;

    uint64_t lba_end;
    std::tie(lba_start_, lba_end) = get_lba_range(off_, total_len, b);
    nlbas_ = lba_end - lba_start_;

    // allocate SPDK-DMA buffer
    spdk_buff_ = std::move(qp_->alloc_buffer(nlbas_));
    if (!spdk_buff_.ptr_m) { rc_ = -ENOMEM; ready_ = true; return true; }

    // lsao_user_data_ must be set before submit so spdk_io_cb can read it
    lsao_.lsao_user_data_ = (uintptr_t)(qp_);

    // for writes: copy user data into the SPDK buffer
    if (is_write) {
        size_t copy_start = (size_t)(off_ - lba_start_ * b);
        if (copy_start != 0) {
            fprintf(stderr, "SpdkIOAwaitable: NYI: unaligned write (RMW first block)\n");
            abort();
        }
        if ((lba_start_ + nlbas_) * b != (uint64_t)off_ + total_len) {
            fprintf(stderr, "SpdkIOAwaitable: NYI: unaligned write (RMW last block)\n");
            abort();
        }
        if (op_ == Op::PWRITE) {
            struct iovec iov = {buf_, len_};
            spdk_buff_.copy_from_iovec(0, &iov, 1);
        } else {
            spdk_buff_.copy_from_iovec(0, iov_, iovcnt_);
        }
        int rc = qp_->submit_write(spdk_buff_, lba_start_, nlbas_,
                                   SPDK::spdk_io_cb, &lsao_);
        if (rc != 0) {
            qp_->free_buffer(std::move(spdk_buff_));
            rc_ = rc; ready_ = true; return true;
        }
    } else {
        int rc = qp_->submit_read(spdk_buff_, lba_start_, nlbas_,
                                  SPDK::spdk_io_cb, &lsao_);
        if (rc != 0) {
            qp_->free_buffer(std::move(spdk_buff_));
            rc_ = rc; ready_ = true; return true;
        }
    }

    T::io_npending_inc(1);
    ready_ = lsao_.is_ready();
    return ready_;
}

bool SpdkIOAwaitable::await_suspend(std::coroutine_handle<> h) noexcept
{
    TaskBase *t = localScheduler__->current_task();
    t->set_current_coro(h);
    lsao_.set_waiter(t);
    return true;
}

ssize_t SpdkIOAwaitable::await_resume() noexcept
{
    if (rc_ != 0) return -1;

    RetT err = lsao_.get_ret();
    if (err) {
        if (spdk_buff_.ptr_m) qp_->free_buffer(std::move(spdk_buff_));
        return -1;
    }

    ssize_t result;
    bool is_read = (op_ == Op::PREAD || op_ == Op::PREADV);
    if (is_read) {
        // copy from SPDK buffer into user buffer
        uint64_t b = qp_->get_sector_size();
        size_t copy_start = (size_t)(off_ - lba_start_ * b);
        if (op_ == Op::PREAD) {
            struct iovec iov = {buf_, len_};
            spdk_buff_.copy_to_iovec((off_t)copy_start, &iov, 1);
        } else {
            spdk_buff_.copy_to_iovec((off_t)copy_start, iov_, iovcnt_);
        }
    }
    result = (ssize_t)iovlen_;

    if (spdk_buff_.ptr_m) qp_->free_buffer(std::move(spdk_buff_));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// SPDK poller task — drives SPDK completions and yields cooperatively.
// ─────────────────────────────────────────────────────────────────────────────
CoroTask SPDK::poller_task(void *unused)
{
    while (!SpdkState__.is_done()) {
        SpdkState__.execute_completions();  // calls spdk_io_cb → T::local_single_notify
        SpdkState__.process_admin_completions();  // drive fabrics keep-alives (throttled)
        co_await T::yield();
    }
    co_return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// RteController
// ─────────────────────────────────────────────────────────────────────────────

static int
scheduler_rte_thread(void *arg)
{
    Scheduler *scheduler = static_cast<Scheduler *>(arg);
    Scheduler::thread_init(scheduler);
    scheduler->set_state_running();
    scheduler->pthread_barrier_wait();
    scheduler->start_();
    return 0;
}

void
RteController::spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                                TaskType main_type, unsigned lcore)
{
    Scheduler *s;
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(lcore, &cpuset);

    if (0) {
    } else {
        schedulers_.emplace_back(*this, cpuset, main_fn, main_arg, main_type);
        s = &schedulers_.back();
    }
    assert(s != NULL);

    rte_eal_remote_launch(scheduler_rte_thread, s, lcore);
    s->pthread_barrier_wait();
}

unsigned
lcore_from_cpuset(cpu_set_t cpuset) {
    int cnt = CPU_COUNT(&cpuset);
    if (cnt != 1) {
        fprintf(stderr, "Unexpected cpuset\n");
        abort();
    }
    for (unsigned i=0; ; i++) {
        if (CPU_ISSET(i, &cpuset))
            return i;
    }
}

void
RteController::wait_for_all(void) {
    for (auto &s: schedulers_) {
        if (s.get_state() == Scheduler::State::RUNNING) {
            unsigned lcore = lcore_from_cpuset(s.get_cpuset());
            int err = rte_eal_wait_lcore(lcore);
            if (err) {
                fprintf(stderr, "rte_eal_wait_lcore: returned: %d\n", err);
            }
        }
    }
    ctl_done_ = true;
}

RteController::~RteController() {
    if (!ctl_done_) {
        set_exit_all();
        wait_for_all();
    }
}

} // end namespace trt
