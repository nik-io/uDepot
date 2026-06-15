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

#ifndef TRT_AIO_HH_
#define TRT_AIO_HH_

#include <coroutine>
#include <inttypes.h>
#include <string.h> // memset()
#include <sys/uio.h> // iovec
#include <functional>
#include <iostream>

#include "trt/local_single_sync.hh"
#include "trt_util/aio.hh"

namespace trt {

AioState *get_tls_AioState__();

class AIO;
class Scheduler;
extern __thread Scheduler *localScheduler__;


// base class for an AIO operation
class AioOpBase {
    friend AIO;

public:
    enum class State {
        INVALID, INITIALIZED, IO_SUBMITTED, IO_ERROR, IO_DONE,
    };

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

protected:
    struct iocb  aio_iocb_;
    struct iocb *aio_iocbp_;
    State        aio_state_;

public:
    AioOpBase(const AioOpBase&) = delete;
    AioOpBase& operator=(const AioOpBase&) = delete;

    AioOpBase(AioOpBase &&op)
        : aio_iocb_(std::move(op.aio_iocb_))
        , aio_iocbp_(&aio_iocb_)
        , aio_state_(std::move(op.aio_state_)) {}

    void operator=(AioOpBase &&op) {
        std::swap(aio_iocb_, op.aio_iocb_);
        aio_iocbp_ = &aio_iocb_;
        std::swap(aio_state_, op.aio_state_);
    }

    virtual ~AioOpBase() {
        if (aio_state_ == State::IO_SUBMITTED) {
            fprintf(stderr, "[%s +%d] ***** destructor called while IO_SUBMITTED\n",
                    __FILE__, __LINE__);
            abort();
        }
    };

    AioOpBase() : aio_state_(State::INVALID) {
        memset(&aio_iocb_, 0, sizeof(aio_iocb_));
        aio_iocbp_ = &aio_iocb_;
    }

    AioOpBase(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset)
    : AioOpBase() { init_op(opcode, fd, buff, nbytes, offset); }

    __attribute__((warn_unused_result))
    int submit();

    virtual void init_op(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset) {
        if (aio_state_ == State::IO_SUBMITTED) {
            fprintf(stderr, "[%s +%d] ***** init_op while IO_SUBMITTED\n", __FILE__, __LINE__);
            abort();
        }
        aio_state_ = State::INITIALIZED;
        memset(&aio_iocb_, 0, sizeof(aio_iocb_));
        aio_iocb_.aio_fildes     = fd;
        aio_iocb_.aio_lio_opcode = opcode;
        aio_iocb_.aio_reqprio    = 0;
        aio_iocb_.aio_buf        = (uintptr_t)buff;
        aio_iocb_.aio_nbytes     = nbytes;
        aio_iocb_.aio_offset     = offset;
        aio_iocb_.aio_data       = (uintptr_t)this;
        aio_iocbp_               = &aio_iocb_;
    }

    size_t nbytes() const {
        assert(aio_state_ != State::INVALID);
        assert(aio_iocb_.aio_lio_opcode == IOCB_CMD_PREAD ||
               aio_iocb_.aio_lio_opcode == IOCB_CMD_PWRITE);
        return aio_iocb_.aio_nbytes;
    }

    State state()          { return aio_state_; }
    bool is_invalid()      { return aio_state_ == State::INVALID; }
    bool is_initialized()  { return aio_state_ == State::INITIALIZED; }
    bool is_io_submitted() { return aio_state_ == State::IO_SUBMITTED; }
    bool is_io_done()      { return aio_state_ == State::IO_DONE; }
    bool is_in_error()     { return aio_state_ == State::IO_ERROR; }

    void expect_state(State s) {
#if !defined(NDEBUG)
        if (s != aio_state_) {
            std::cerr << "Expected state:" << state_to_str(s)
                      << " but got:" << state_to_str(aio_state_) << std::endl;
            abort();
        }
#endif
    }
    void expect_invalid()  { expect_state(State::INVALID); }
    void expect_io_done()  { expect_state(State::IO_DONE); }

    void set_error() { aio_state_ = State::IO_ERROR; }
    void set_done()  { aio_state_ = State::IO_DONE; }
    virtual void complete(RetT val) = 0;

    virtual bool is_ready() { return is_io_done(); }

    // in some cases, we might want to perform a "dummy" read, i.e., memcpy data
    // from a buffer instead of actually doing IO.
    void fake_read(const void *src, size_t src_nbytes);
};

// AIO operation that completes via a LocalSingleAsyncObj.
// Awaitable: after submit(), wait with co_await T::local_single_wait(&aio_lsaobj_).
class AioOp : public AioOpBase {
    friend AIO;
    friend struct AioOpAwaitable;

protected:
    LocalSingleAsyncObj aio_lsaobj_;

public:
    AioOp(const AioOp&) = delete;
    AioOp& operator=(const AioOp&) = delete;

    AioOp(AioOp &&op)
    : AioOpBase(std::move(op))
    , aio_lsaobj_(std::move(op.aio_lsaobj_)) {}

    void operator=(AioOp &&op) {
        aio_lsaobj_ = std::move(op.aio_lsaobj_);
        AioOpBase::operator=(std::move(op));
    }

    AioOp(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset)
    : AioOpBase(opcode, fd, buff, nbytes, offset), aio_lsaobj_() {}

    AioOp() : AioOpBase(), aio_lsaobj_() {}

    virtual void init_op(uint16_t opcode, int fd, void *buff, size_t nbytes, off_t offset) override {
        AioOpBase::init_op(opcode, fd, buff, nbytes, offset);
        new (&aio_lsaobj_) LocalSingleAsyncObj();
    }

    // Called by the poller when IO completes: directly pushes waiting task.
    virtual void complete(RetT val) override;

    virtual bool is_ready() {
        bool ret = aio_lsaobj_.is_ready();
        assert(ret == AioOpBase::is_ready());
        return ret;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Awaitable returned by AIO::pread/pwrite/preadv/pwritev.
// The AioOp lives here on the caller's coroutine frame.
// ─────────────────────────────────────────────────────────────────────────────
struct AioOpAwaitable {
    uint16_t opcode_;
    int      fd_;
    void    *buff_;
    size_t   nbytes_;
    off_t    off_;
    AioOp    aio_op_; // default constructed here; init_op() called in await_ready()
    bool     submit_failed_ = false;

    AioOpAwaitable(uint16_t op, int fd, void *buff, size_t n, off_t off) noexcept
        : opcode_(op), fd_(fd), buff_(buff), nbytes_(n), off_(off) {}

    bool await_ready() noexcept {
        // init_op() here so aio_iocb_.aio_data = (uintptr_t)this is correct.
        aio_op_.init_op(opcode_, fd_, buff_, nbytes_, off_);
        int ret = aio_op_.submit();
        if (ret == -1) { submit_failed_ = true; return true; }
        return aio_op_.aio_lsaobj_.is_ready();
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        TaskBase *t = localScheduler__->current_task();
        t->set_current_coro(h);
        aio_op_.aio_lsaobj_.set_waiter(t);
        return true;
    }

    ssize_t await_resume() noexcept {
        if (submit_failed_) return (ssize_t)-1;
        return (ssize_t)aio_op_.aio_lsaobj_.get_ret();
    }
};

// AIO operation that completes via a callback (no LSAO, no awaitable wait).
class AioOpCallback : public AioOpBase {
    friend AIO;
    using CallBackFn = std::function<void(AioOpCallback *op, RetT ret)>;
    CallBackFn aio_callback_;

public:
    using AioOpBase::AioOpBase;

    template<typename... Args>
    AioOpCallback(CallBackFn cb, Args... args)
    : AioOpBase(std::forward<Args>(args)...)
    , aio_callback_(cb) {}

    virtual void complete(RetT val) override;

    void set_callback(CallBackFn cbfn) { aio_callback_ = cbfn; }
};

class AIO {
   public:
    static inline bool is_initialized() { return get_tls_AioState__()->is_initialized(); }
    static inline bool is_done()        { return get_tls_AioState__()->is_done(); }
    static inline void init()           { get_tls_AioState__()->init(); }
    static void stop()                  { get_tls_AioState__()->stop(); }

    // Awaitable-returning IO operations.
    // Usage: ssize_t ret = co_await AIO::pread(fd, buff, nbytes, off);
    static AioOpAwaitable pread(int fd, void *buff, size_t nbytes, off_t off) {
        return AioOpAwaitable{IOCB_CMD_PREAD, fd, buff, nbytes, off};
    }
    static AioOpAwaitable pwrite(int fd, const void *buff, size_t nbytes, off_t off) {
        return AioOpAwaitable{IOCB_CMD_PWRITE, fd, (void *)buff, nbytes, off};
    }
    static AioOpAwaitable preadv(int fd, const struct iovec *iov, int iovcnt, off_t off) {
        return AioOpAwaitable{IOCB_CMD_PREADV, fd, (void *)iov, (size_t)iovcnt, off};
    }
    static AioOpAwaitable pwritev(int fd, const struct iovec *iov, int iovcnt, off_t off) {
        return AioOpAwaitable{IOCB_CMD_PWRITEV, fd, (void *)iov, (size_t)iovcnt, off};
    }

    // Coroutine poller task (must be spawned as a POLL task).
    static CoroTask poller_task(void *unused);
};

} // end namespace trt

#endif /* TRT_AIO_HH_ */
