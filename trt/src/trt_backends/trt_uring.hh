/*
 *  Copyright (c) 2020,2022 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *           Nikolas Ioannou (nio@zurich.ibm.com, nicioan@gmail.com)
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#ifndef TRT_IOU_HH_
#define TRT_IOU_HH_

#include <coroutine>
#include <inttypes.h>
#include <string.h> // memset()
#include <sys/uio.h> // iovec
#include <functional>
#include <iostream>

#include "trt/local_single_sync.hh"
#include "trt_util/uring.hh"

namespace trt {

UringState *get_tls_IouState__();

class IOU;
class Scheduler;
extern __thread Scheduler *localScheduler__;


struct IouOp {
public:
    IouOp(const IouOp&) = delete;
    IouOp& operator=(const IouOp&) = delete;
    IouOp(IouOp &&op) = delete;

    enum State { INVALID, INITIALIZED, IO_SUBMITTED, IO_ERROR, IO_DONE, };
    enum IouOpCode { INVALID_OP, PREADV, PWRITEV };

    IouOp() : iou_lsaobj_(), state_(State::INVALID), op_code_(IouOpCode::INVALID_OP) {}

    IouOp(IouOpCode op, int fd, struct iovec *iov, int iovcnt, off_t off)
      : iou_lsaobj_(), state_(State::INITIALIZED),
        op_code_(op), fd_(fd), iovec_(iov), iovec_cnt_(iovcnt), offset_(off) {
        switch(op_code_) {
        case IouOpCode::PREADV:
        case IouOpCode::PWRITEV: break;
        default:
            fprintf(stderr, "invalid IOU OP Code=(%d)\n", op);
            abort();
        };
    }

    ~IouOp() {
        if (state_ == State::IO_SUBMITTED) {
            fprintf(stderr, "[%s +%d] ***** destructor called while IO_SUBMITTED\n",
                    __FILE__, __LINE__);
            abort();
        }
    };

    bool is_invalid()      { return state_ == State::INVALID; }
    bool is_initialized()  { return state_ == State::INITIALIZED; }
    bool is_io_submitted() { return state_ == State::IO_SUBMITTED; }
    bool is_io_done()      { return state_ == State::IO_DONE; }
    bool is_in_error()     { return state_ == State::IO_ERROR; }

    void expect_state(State s) {
#if !defined(NDEBUG)
        if (s != state_) {
            std::cerr << "Expected state:" << state_to_str(s)
                      << " but got:" << state_to_str(state_) << std::endl;
            abort();
        }
#endif
    }
    void expect_invalid() { expect_state(State::INVALID); }
    void expect_io_done() { expect_state(State::IO_DONE); }

    void set_error() { state_ = State::IO_ERROR; }
    void set_done()  { state_ = State::IO_DONE; }
    void complete(RetT val);
    int submit();
    bool is_ready() { return is_io_done(); }

    // Fake a read completion (for cache hits).
    void fake_read(const void *src, size_t src_nbytes);

    static std::string state_to_str(State &s) {
        switch (s) {
        case State::INVALID:      return "INVALID";
        case State::INITIALIZED:  return "INITIALIZED";
        case State::IO_SUBMITTED: return "IO_SUBMITTED";
        case State::IO_ERROR:     return "IO_ERROR";
        case State::IO_DONE:      return "IO_DONE";
        default:                  return "__INVALID__";
        }
    }

    State state() { return state_; }

// public to allow IouOpAwaitable to access lsaobj_
public:
    LocalSingleAsyncObj iou_lsaobj_;
private:
    State     state_;
    IouOpCode op_code_;
    int       fd_;
    struct iovec *iovec_;
    int       iovec_cnt_;
    int64_t   offset_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Awaitable returned by IOU::pread/pwrite/preadv/pwritev.
// The IouOp lives on the caller's coroutine frame (inside this struct).
//
// For pread/pwrite (single buffer), single_iov_ is embedded here so the
// iovec pointer remains valid for the lifetime of the IO.
// For preadv/pwritev, iov_ points directly to the caller-supplied array
// (which lives on the caller's coroutine frame and outlives the await).
// ─────────────────────────────────────────────────────────────────────────────
struct IouOpAwaitable {
    IouOp::IouOpCode op_;
    int              fd_;
    off_t            off_;
    struct iovec     single_iov_;       // used by pread/pwrite; zeroed otherwise
    const struct iovec *iov_;           // points to single_iov_ or caller's array
    int              iovcnt_;
    IouOp            iou_op_;           // default-constructed; set up in await_ready()
    bool             submit_failed_ = false;

    // Non-movable, non-copyable: iov_ may point into this struct.
    IouOpAwaitable(const IouOpAwaitable &) = delete;
    IouOpAwaitable &operator=(const IouOpAwaitable &) = delete;
    IouOpAwaitable(IouOpAwaitable &&) = delete;
    IouOpAwaitable &operator=(IouOpAwaitable &&) = delete;

    // preadv/pwritev: caller provides iov array (lives on their frame)
    IouOpAwaitable(IouOp::IouOpCode op, int fd,
                   const struct iovec *iov, int iovcnt, off_t off) noexcept
        : op_(op), fd_(fd), off_(off), single_iov_{},
          iov_(iov), iovcnt_(iovcnt) {}

    // pread/pwrite: single buffer; iov embedded in this struct
    IouOpAwaitable(IouOp::IouOpCode op, int fd,
                   void *buf, size_t nbytes, off_t off) noexcept
        : op_(op), fd_(fd), off_(off), single_iov_{buf, nbytes},
          iov_(&single_iov_), iovcnt_(1) {}

    bool await_ready() noexcept {
        // Construct IouOp here: iov_ address is final (we're on caller's frame).
        new (&iou_op_) IouOp(op_, fd_, (struct iovec *)iov_, iovcnt_, off_);
        int ret = iou_op_.submit();
        if (ret == -1) { submit_failed_ = true; return true; }
        return iou_op_.iou_lsaobj_.is_ready();
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        TaskBase *t = localScheduler__->current_task();
        t->set_current_coro(h);
        iou_op_.iou_lsaobj_.set_waiter(t);
        return true;
    }

    ssize_t await_resume() noexcept {
        if (submit_failed_) return (ssize_t)-1;
        return (ssize_t)iou_op_.iou_lsaobj_.get_ret();
    }
};

class IOU {
   public:
    static inline bool is_initialized() { return get_tls_IouState__()->is_initialized(); }
    static inline bool is_done()        { return get_tls_IouState__()->is_done(); }
    static inline void init()           { get_tls_IouState__()->init(); }
    static void stop()                  { get_tls_IouState__()->stop(); }

    // Awaitable-returning IO operations.
    // Usage: ssize_t ret = co_await IOU::pread(fd, buff, nbytes, off);
    static IouOpAwaitable pread(int fd, void *buff, size_t nbytes, off_t off) {
        return IouOpAwaitable{IouOp::IouOpCode::PREADV, fd, buff, nbytes, off};
    }
    static IouOpAwaitable pwrite(int fd, const void *buff, size_t nbytes, off_t off) {
        return IouOpAwaitable{IouOp::IouOpCode::PWRITEV, fd, (void *)buff, nbytes, off};
    }
    static IouOpAwaitable preadv(int fd, const struct iovec *iov, int iovcnt, off_t off) {
        return IouOpAwaitable{IouOp::IouOpCode::PREADV, fd, iov, iovcnt, off};
    }
    static IouOpAwaitable pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off) {
        return IouOpAwaitable{IouOp::IouOpCode::PWRITEV, fd, iov, iovcnt, off};
    }

    // Coroutine poller task.
    static CoroTask poller_task(void *unused);
};

} // end namespace trt

#endif /* TRT_IOU_HH_ */
