/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

/* vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4: */

#ifndef TRT_SPDK_HH__
#define TRT_SPDK_HH__

#include <coroutine>
#include <sys/uio.h>

#include "trt_util/spdk.hh"
#include "trt/uapi/trt.hh"
#include "trt/local_single_sync.hh"

#include <vector>
#include <string>

namespace trt {

class Scheduler;
extern __thread Scheduler *localScheduler__;

SpdkState *get_tls_SpdkState__();

// ─────────────────────────────────────────────────────────────────────────────
// Awaitable return types — declared before SPDK so factory methods can name them
// ─────────────────────────────────────────────────────────────────────────────

// SpdkRawAwaitable: awaitable for SPDK::read() / SPDK::write() (raw LBA ops)
struct SpdkRawAwaitable {
    enum class Op { READ, WRITE };

    SpdkQpair          *qp_;
    SpdkPtr            &buff_;   // caller-owned SPDK buffer (by reference)
    uint64_t            lba_;
    uint32_t            lba_cnt_;
    Op                  op_;
    LocalSingleAsyncObj lsao_{};
    int                 rc_    = 0;
    bool                ready_ = false;

    SpdkRawAwaitable(SpdkQpair *qp, SpdkPtr &buff,
                     uint64_t lba, uint32_t lba_cnt, Op op)
        : qp_(qp), buff_(buff), lba_(lba), lba_cnt_(lba_cnt), op_(op) {}

    SpdkRawAwaitable(const SpdkRawAwaitable &) = delete;
    SpdkRawAwaitable &operator=(const SpdkRawAwaitable &) = delete;
    SpdkRawAwaitable(SpdkRawAwaitable &&) = delete;

    bool    await_ready()   noexcept;
    bool    await_suspend(std::coroutine_handle<> h) noexcept;
    ssize_t await_resume()  noexcept;
};

// SpdkIOAwaitable: awaitable for SPDK::pread/pwrite/preadv/pwritev
// Handles buffer allocation, LBA-alignment, copy, and free internally.
struct SpdkIOAwaitable {
    enum class Op { PREAD, PWRITE, PREADV, PWRITEV };

    SpdkQpair          *qp_;
    Op                  op_;
    void               *buf_    = nullptr;   // used by pread/pwrite
    size_t              len_    = 0;         // used by pread/pwrite
    const struct iovec *iov_    = nullptr;   // used by preadv/pwritev
    int                 iovcnt_ = 0;         // used by preadv/pwritev
    off_t               off_    = 0;

    // filled in await_ready():
    uint64_t            lba_start_ = 0;
    uint64_t            nlbas_     = 0;
    size_t              iovlen_    = 0;      // total iovec length
    SpdkPtr             spdk_buff_;          // RAII: freed explicitly in await_resume
    LocalSingleAsyncObj lsao_{};
    int                 rc_        = 0;
    bool                ready_     = false;

    // pread / pwrite constructor
    SpdkIOAwaitable(SpdkQpair *qp, Op op, void *buf, size_t len, off_t off)
        : qp_(qp), op_(op), buf_(buf), len_(len), off_(off) {}

    // preadv / pwritev constructor
    SpdkIOAwaitable(SpdkQpair *qp, Op op,
                    const struct iovec *iov, int iovcnt, off_t off)
        : qp_(qp), op_(op), iov_(iov), iovcnt_(iovcnt), off_(off) {}

    SpdkIOAwaitable(const SpdkIOAwaitable &) = delete;
    SpdkIOAwaitable &operator=(const SpdkIOAwaitable &) = delete;
    SpdkIOAwaitable(SpdkIOAwaitable &&) = delete;

    bool    await_ready()   noexcept;
    bool    await_suspend(std::coroutine_handle<> h) noexcept;
    ssize_t await_resume()  noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// SPDK class
// ─────────────────────────────────────────────────────────────────────────────
class SPDK {
    friend SpdkRawAwaitable;
    friend SpdkIOAwaitable;

public:
    static inline void init(SpdkGlobalState &gs) {
        get_tls_SpdkState__()->init(gs);
    }

    static inline std::shared_ptr<SpdkQpair>
    getQpair(std::string a) {
        return get_tls_SpdkState__()->getQpair(a);
    }

    static inline std::shared_ptr<SpdkQpair>
    getQpair_by_prefix(std::string a) {
        return get_tls_SpdkState__()->getQpair_by_prefix(a);
    }

    static std::vector<std::string>
    getNamespaceNames() {
        return get_tls_SpdkState__()->getNamespaceNames();
    }

    static void stop() { get_tls_SpdkState__()->stop(); }

    // Awaitable IO operations — must be co_await'd inside a TRT task.
    static SpdkRawAwaitable read(SpdkQpair *qp, SpdkPtr &buff,
                                 uint64_t lba, uint32_t lba_cnt) {
        return {qp, buff, lba, lba_cnt, SpdkRawAwaitable::Op::READ};
    }
    static SpdkRawAwaitable write(SpdkQpair *qp, SpdkPtr &buff,
                                  uint64_t lba, uint32_t lba_cnt) {
        return {qp, buff, lba, lba_cnt, SpdkRawAwaitable::Op::WRITE};
    }

    static SpdkIOAwaitable pread(SpdkQpair *qp, void *buff,
                                 size_t len, off_t off) {
        return {qp, SpdkIOAwaitable::Op::PREAD, buff, len, off};
    }
    static SpdkIOAwaitable pwrite(SpdkQpair *qp, const void *buff,
                                  size_t len, off_t off) {
        return {qp, SpdkIOAwaitable::Op::PWRITE,
                const_cast<void *>(buff), len, off};
    }
    static SpdkIOAwaitable preadv(SpdkQpair *qp, const struct iovec *iov,
                                  size_t iovcnt, off_t off) {
        return {qp, SpdkIOAwaitable::Op::PREADV, iov, (int)iovcnt, off};
    }
    static SpdkIOAwaitable pwritev(SpdkQpair *qp, const struct iovec *iov,
                                   int iovcnt, off_t off) {
        return {qp, SpdkIOAwaitable::Op::PWRITEV, iov, iovcnt, off};
    }

    static CoroTask poller_task(void *arg);

protected:
    static void spdk_io_cb(void *, const struct spdk_nvme_cpl *);
};

class RteController : public ControllerBase {
public:
    void spawn_scheduler(TaskFn main_fn, TaskFnArg main_arg,
                         TaskType main_type,
                         unsigned lcore);
    void wait_for_all();
    virtual ~RteController();
};

} // end namespace trt

#endif /* TRT_SPDK_HH__ */
