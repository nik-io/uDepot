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

// Trt support for using epoll()

#ifndef TRT_EPOLL_HH_
#define TRT_EPOLL_HH_

#include <coroutine>
#include <functional>
#include <unordered_map>
#include <tuple>
#include <sys/socket.h>
#include <sys/epoll.h>

extern "C" {
    #include "trt_util/misc.h"  // spinlock_t
}
#include "trt_util/deque_mt.hh"
#include "trt/local_single_sync.hh"
#include "trt/task.hh"           // CoroTask, TaskFn, TaskFnArg, TaskType

namespace trt {

class Scheduler;
extern __thread Scheduler *localScheduler__;

class EpollState;

// ─────────────────────────────────────────────────────────────────────────────
// EpollOpType — direction of an epoll-backed IO operation
// ─────────────────────────────────────────────────────────────────────────────
enum class EpollOpType { IN, OUT };

// ─────────────────────────────────────────────────────────────────────────────
// EpollSpawnPolicy — where to spawn an accepted connection's handler task
// ─────────────────────────────────────────────────────────────────────────────
enum class EpollSpawnPolicy { Local, Distribute };

// ─────────────────────────────────────────────────────────────────────────────
// EpollOpAwaitable
//
// Wraps a single retried-on-EAGAIN socket operation in a C++20 awaitable.
// The caller writes:  ssize_t r = co_await Epoll::recv(fd, buf, len, 0);
//
// Lifecycle:
//  1. await_ready():   try the syscall. If it succeeds (or fails with a hard
//                      error), cache the result and return true (no suspend).
//                      If EAGAIN/EWOULDBLOCK, return false.
//  2. await_suspend(): register this awaitable's lsao_ into the EpollState fd
//                      map so the poller can wake us, then set the current TRT
//                      task as waiter on lsao_.  Always returns true (suspend).
//  3. await_resume():  if we didn't suspend (ready_==true) just return cached
//                      ret_.  Otherwise decrement pending_waits_, check for
//                      shutdown, and retry the syscall once (level-triggered
//                      epoll guarantees the fd is ready at this point).
//                      Calls register_fd on the resulting fd when reg_==true
//                      (accept path).
// ─────────────────────────────────────────────────────────────────────────────
struct EpollOpAwaitable {
    EpollState              *es_;
    int                      fd_;
    EpollOpType              ty_;
    std::function<ssize_t()> syscall_;  // captures all per-op arguments
    LocalSingleAsyncObj      lsao_{};
    ssize_t                  ret_   = -1;
    bool                     ready_ = false;  // true when no suspension needed
    bool                     reg_   = false;  // register new fd after accept

    EpollOpAwaitable(EpollState *es, int fd, EpollOpType ty,
                     std::function<ssize_t()> fn, bool reg = false)
        : es_(es), fd_(fd), ty_(ty), syscall_(std::move(fn)), reg_(reg) {}

    // Non-copyable, non-movable: lsao_ must stay at a fixed address while
    // it is registered as the async object for an fd in EpollState::fds_.
    EpollOpAwaitable(const EpollOpAwaitable &) = delete;
    EpollOpAwaitable &operator=(const EpollOpAwaitable &) = delete;
    EpollOpAwaitable(EpollOpAwaitable &&) = delete;

    bool    await_ready()  noexcept;
    bool    await_suspend(std::coroutine_handle<> h) noexcept;
    ssize_t await_resume() noexcept;
};

// ─────────────────────────────────────────────────────────────────────────────
// EpollState — per-scheduler epoll context (internal implementation)
// ─────────────────────────────────────────────────────────────────────────────
class EpollState {
    friend EpollOpAwaitable;

    enum class State { UNINITIALIZED, READY, DRAINING, DONE };
    State ep_state_;
    int   ep_fd_;
    int   ep_wait_timeout_;

    struct FdInfo {
        uint32_t event_mask;
        int      old_flags;
        LocalSingleAsyncObj *ao_in_, *ao_out_;

        FdInfo(uint32_t mask, int fl)
            : event_mask(mask), old_flags(fl)
            , ao_in_(nullptr), ao_out_(nullptr) {}
    };
    std::unordered_map<int, FdInfo> fds_;
    size_t pending_waits_;

    struct EpollSpawn {
        int       reg_fd;
        uint32_t  reg_mask;
        TaskFn    spawn_fn;
        TaskFnArg spawn_arg;

        EpollSpawn(int fd, uint32_t m, TaskFn fn, TaskFnArg arg)
            : reg_fd(fd), reg_mask(m), spawn_fn(fn), spawn_arg(arg) {}
        EpollSpawn() : EpollSpawn(-1, 0, nullptr, nullptr) {}
    };
    deque_mt<EpollSpawn> epoll_spawn_deque_;

public:
    EpollState();

    EpollState(EpollState const &) = delete;
    void operator=(EpollState const &) = delete;

    void register_fd(int fd, uint32_t event_mask);
    int  deregister_fd(int fd);

    void init();
    void stop();

    CoroTask poller_task(void *arg);

    void set_timeout_param(int t) { ep_wait_timeout_ = t; }
    int  get_timeout_param()      { return ep_wait_timeout_; }
    int  get_timeout() {
        return T::io_npending_get() == 0 ? get_timeout_param() : 0;
    }

private:
    void notify_maybe(int fd, EpollOpType ty);
    void shutdown_all();

    EpollState *es_get_rr();

public:
    int listen(int sockfd, int backlog);

    // These return EpollOpAwaitable — must be co_await'd.
    EpollOpAwaitable accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    EpollOpAwaitable accept_ll(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
    EpollOpAwaitable recv(int fd, void *buff, size_t len, int flags);
    EpollOpAwaitable send(int fd, const void *buff, size_t len, int flags);
    EpollOpAwaitable sendmsg(int fd, const struct msghdr *msg, int flags);
    EpollOpAwaitable recvmsg(int fd, struct msghdr *msg, int flags);

    void register_and_spawn(int fd, uint32_t m, TaskFn fn, TaskFnArg arg,
                            EpollSpawnPolicy p);
};

// ─────────────────────────────────────────────────────────────────────────────
// EpollGlobalState — registry of all per-scheduler EpollStates
// ─────────────────────────────────────────────────────────────────────────────
class EpollGlobalState {
    static const size_t STATES_NR_ = 128;
    EpollState *states_[STATES_NR_];
    size_t      states_next_;
    spinlock_t  lock_;

public:
    EpollGlobalState() : states_next_(0) {
        for (size_t i = 0; i < STATES_NR_; i++)
            states_[i] = nullptr;
        spinlock_init(&lock_);
    }

    inline void lock()   { spin_lock(&lock_); }
    inline void unlock() { spin_unlock(&lock_); }

    void register_epoll_state(EpollState *st) {
        lock();
        assert(st != nullptr);
        if (states_next_ == STATES_NR_) {
            fprintf(stderr, "More states than max=%zd. Dying ungracefully.", STATES_NR_);
            abort();
        }
        states_[states_next_++] = st;
        unlock();
    }

    void deregister_epoll_state(EpollState *st) {
        lock();
        for (size_t i = 0; i < STATES_NR_; i++) {
            if (states_[i] == st) {
                for (;;) {
                    states_[i] = (i < STATES_NR_ - 1) ? states_[i + 1] : nullptr;
                    if (states_[i] == nullptr) break;
                    i++;
                }
                states_next_--;
                unlock();
                return;
            }
        }
        unlock();
        fprintf(stderr, "%s:%d: state %p not found", __PRETTY_FUNCTION__, __LINE__, st);
        abort();
    }

    EpollState *es_get_mod(size_t idx) {
        EpollState *ret;
        lock();
        ret = states_[idx % states_next_];
        unlock();
        return ret;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Public Epoll API (thin wrappers around the thread-local EpollState__)
// ─────────────────────────────────────────────────────────────────────────────
#if !defined(TRT_EPOLL_SELF)
extern thread_local EpollState EpollState__;

struct Epoll {
    static void init()  { EpollState__.init(); }
    static void stop()  { EpollState__.stop(); }

    static CoroTask poller_task(void *arg) {
        return EpollState__.poller_task(arg);
    }

    static int listen(int fd, int backlog) {
        return EpollState__.listen(fd, backlog);
    }

    // accept: waits for a new connection and registers the new fd.
    static EpollOpAwaitable accept(int sockfd, struct sockaddr *addr,
                                   socklen_t *addrlen) {
        return EpollState__.accept(sockfd, addr, addrlen);
    }
    // accept_ll: waits but does NOT register the new fd (caller must register).
    static EpollOpAwaitable accept_ll(int sockfd, struct sockaddr *addr,
                                      socklen_t *addrlen) {
        return EpollState__.accept_ll(sockfd, addr, addrlen);
    }

    static EpollOpAwaitable recv(int fd, void *buf, size_t len, int flags) {
        return EpollState__.recv(fd, buf, len, flags);
    }
    static EpollOpAwaitable send(int fd, const void *buf, size_t len, int flags) {
        return EpollState__.send(fd, buf, len, flags);
    }
    static EpollOpAwaitable sendmsg(int fd, const struct msghdr *msg, int flags) {
        return EpollState__.sendmsg(fd, msg, flags);
    }
    static EpollOpAwaitable recvmsg(int fd, struct msghdr *msg, int flags) {
        return EpollState__.recvmsg(fd, msg, flags);
    }

    static int close(int fd) {
        int err = EpollState__.deregister_fd(fd);
        if (err)
            fprintf(stderr, "%s:%d: deregister_fd returned error\n",
                    __PRETTY_FUNCTION__, __LINE__);
        return ::close(fd);
    }

    using SpawnPolicy = EpollSpawnPolicy;
    static void register_and_spawn(int fd, uint32_t m, TaskFn fn, TaskFnArg arg,
                                   EpollSpawnPolicy p) {
        return EpollState__.register_and_spawn(fd, m, fn, arg, p);
    }

    static void set_wait_timeout(int x) { EpollState__.set_timeout_param(x); }
    static int  get_wait_timeout()      { return EpollState__.get_timeout_param(); }

    static void *epoll_handle() { return static_cast<void *>(&EpollState__); }
};
#endif

} // namespace trt

#endif /* TRT_EPOLL_HH_ */
