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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <assert.h>

#include "trt/uapi/trt.hh"
#include "trt/scheduler.hh"

#define TRT_EPOLL_SELF
#include "trt_backends/trt_epoll.hh"

#define MAX_NEVENTS 128

namespace trt {

thread_local EpollState EpollState__;
EpollGlobalState        EpollGlobalState__;

// ─────────────────────────────────────────────────────────────────────────────
// EpollState constructor / init / stop
// ─────────────────────────────────────────────────────────────────────────────
EpollState::EpollState()
    : ep_state_(State::UNINITIALIZED)
    , ep_fd_(-1)
    , ep_wait_timeout_(0)
    , pending_waits_(0) {}

void EpollState::init() {
    if (ep_state_ != State::UNINITIALIZED) {
        fprintf(stderr, "%s: Invalid state\n", __PRETTY_FUNCTION__);
        abort();
    }
    ep_fd_ = epoll_create(MAX_NEVENTS);
    if (ep_fd_ == -1) { perror("epoll_create"); exit(1); }
    EpollGlobalState__.register_epoll_state(this);
    ep_state_ = State::READY;
}

void EpollState::stop() {
    if (ep_state_ != State::READY) {
        fprintf(stderr, "%s: Invalid state\n", __PRETTY_FUNCTION__);
        abort();
    }
    EpollGlobalState__.deregister_epoll_state(this);
    ep_state_ = State::DRAINING;
    shutdown_all();
    ::close(ep_fd_);
    ep_fd_ = -1;
    ep_state_ = State::DONE;
}

// ─────────────────────────────────────────────────────────────────────────────
// fd registration helpers
// ─────────────────────────────────────────────────────────────────────────────
static int
setnonblocking(int fd, int &flags) {
    if (-1 == (flags = fcntl(fd, F_GETFL, 0))) flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int setnonblocking(int fd) {
    int unused;
    return setnonblocking(fd, unused);
}

int EpollState::deregister_fd(int fd) {
    auto iter = fds_.find(fd);
    if (iter == fds_.end())
        return ep_state_ == State::DONE ? 0 : 1;

    if (iter->second.ao_in_) {
        fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
        abort();
    }
    if (iter->second.ao_out_) {
        fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
        abort();
    }

    fds_.erase(iter);

    int ret = epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, NULL);
    if (ret == -1)
        trt_dmsg("epoll_ctl: EPOLL_CTL_DEL returned %d (%s)\n", errno, strerror(errno));
    return 0;
}

void EpollState::register_fd(int fd, uint32_t event_mask) {
    int old_flags, ret;
    struct epoll_event ev;

    setnonblocking(fd, old_flags);
    assert(fds_.find(fd) == fds_.end());
    fds_.emplace(std::piecewise_construct,
                 std::make_tuple(fd),
                 std::make_tuple(event_mask, old_flags));
    ev.events   = event_mask;
    ev.data.fd  = fd;
    ret = epoll_ctl(ep_fd_, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) { perror("epoll_ctl"); abort(); }
}

int EpollState::listen(int sockfd, int backlog) {
    int ret = ::listen(sockfd, backlog);
    if (ret < 0) return ret;
    setnonblocking(sockfd);
    register_fd(sockfd, EPOLLIN);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Shutdown helper — directly notifies all pending waiters with ESHUTDOWN
// ─────────────────────────────────────────────────────────────────────────────
void EpollState::shutdown_all() {
    for (auto x = fds_.begin(); x != fds_.end(); ) {
        FdInfo *fd_i = &x->second;
        if (fd_i->ao_in_)  T::local_single_notify(fd_i->ao_in_,  (RetT)ESHUTDOWN);
        if (fd_i->ao_out_) T::local_single_notify(fd_i->ao_out_, (RetT)ESHUTDOWN);
        epoll_ctl(ep_fd_, EPOLL_CTL_DEL, x->first, NULL);
        x = fds_.erase(x);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// notify_maybe — called by the poller to wake a waiting task for a given fd
// ─────────────────────────────────────────────────────────────────────────────
void EpollState::notify_maybe(int fd, EpollOpType ty) {
    auto entry = fds_.find(fd);
    assert(entry != fds_.end());

    LocalSingleAsyncObj **lsao_pptr;
    switch (ty) {
        case EpollOpType::OUT: lsao_pptr = &entry->second.ao_out_; break;
        case EpollOpType::IN:  lsao_pptr = &entry->second.ao_in_;  break;
        default: abort();
    }

    LocalSingleAsyncObj *lsao = *lsao_pptr;
    if (lsao == nullptr) return;
    *lsao_pptr = nullptr;
    T::local_single_notify(lsao, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// round-robin EpollState selection (for Distribute spawn policy)
// ─────────────────────────────────────────────────────────────────────────────
EpollState *EpollState::es_get_rr() {
    static size_t idx = 0;
    return EpollGlobalState__.es_get_mod(idx++);
}

void EpollState::register_and_spawn(int fd, uint32_t m, TaskFn fn,
                                    TaskFnArg arg, EpollSpawnPolicy p) {
    EpollState *es;
    if (p == EpollSpawnPolicy::Local || (es = es_get_rr()) == &EpollState__) {
        register_fd(fd, m);
        T::spawn_detached_no_wait(fn, arg, TaskType::TASK);
    } else {
        es->epoll_spawn_deque_.emplace_back(fd, m, fn, arg);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Poller task — processes epoll events and yields cooperatively
// ─────────────────────────────────────────────────────────────────────────────
CoroTask EpollState::poller_task(void *unused) {
    struct epoll_event *ep_events;
    const size_t max_nevents = MAX_NEVENTS;

    trt_dmsg("Starting %s\n", __PRETTY_FUNCTION__);
    assert(ep_state_ == State::READY);
    ep_events = static_cast<struct epoll_event *>(
        malloc(sizeof(struct epoll_event) * max_nevents));
    if (!ep_events) { perror("malloc"); exit(1); }

    for (;;) {
        int nevents = 0;

        if (ep_fd_ == -1) {
            trt_msg("TRT Epoll poller exiting\n");
            break;
        }

        if (pending_waits_ > 0) {
            int timeout = get_timeout();
            nevents = epoll_wait(ep_fd_, ep_events, max_nevents, timeout);
            if (nevents < 0) { perror("epoll_wait"); exit(1); }
        }

        // drain the remote spawn deque (one entry per poller iteration)
        EpollSpawn s;
        if (epoll_spawn_deque_.pop_front(s)) {
            register_fd(s.reg_fd, s.reg_mask);
            T::spawn_detached_no_wait(s.spawn_fn, s.spawn_arg, TaskType::TASK);
        }

        if (nevents == 0) {
            co_await T::yield();
            continue;
        }

        for (int i = 0; i < nevents; i++) {
            struct epoll_event *ev = &ep_events[i];
            int fd = ev->data.fd;
            if (ev->events & EPOLLIN)  notify_maybe(fd, EpollOpType::IN);
            if (ev->events & EPOLLOUT) notify_maybe(fd, EpollOpType::OUT);
        }
        co_await T::yield();
    }

    free(ep_events);
    co_return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// EpollOpAwaitable implementation
// ─────────────────────────────────────────────────────────────────────────────
bool EpollOpAwaitable::await_ready() noexcept {
    if (es_->ep_state_ == EpollState::State::DRAINING) {
        errno = ESHUTDOWN;
        ret_   = -1;
        ready_ = true;
        return true;
    }

    ret_ = syscall_();
    if (ret_ != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        // success or hard error — no need to suspend
        if (ret_ > 0 && reg_) es_->register_fd((int)ret_, EPOLLIN);
        ready_ = true;
        return true;
    }
    return false;  // EAGAIN: suspend and wait for epoll event
}

bool EpollOpAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    assert(es_->ep_state_ == EpollState::State::READY);

    auto entry = es_->fds_.find(fd_);
    assert(entry != es_->fds_.end());

    LocalSingleAsyncObj **lsao_pptr;
    switch (ty_) {
        case EpollOpType::OUT: lsao_pptr = &entry->second.ao_out_; break;
        case EpollOpType::IN:  lsao_pptr = &entry->second.ao_in_;  break;
        default: abort();
    }
    assert(*lsao_pptr == nullptr);
    *lsao_pptr = &lsao_;
    es_->pending_waits_++;

    TaskBase *t = localScheduler__->current_task();
    t->set_current_coro(h);
    lsao_.set_waiter(t);
    return true;
}

ssize_t EpollOpAwaitable::await_resume() noexcept {
    if (ready_) return ret_;  // returned without suspending

    es_->pending_waits_--;
    RetT val = lsao_.get_ret();
    if (val != 0) {
        // woken by shutdown_all
        errno = ESHUTDOWN;
        return -1;
    }
    // epoll fired: retry the syscall (level-triggered guarantees readiness)
    ret_ = syscall_();
    if (ret_ > 0 && reg_) es_->register_fd((int)ret_, EPOLLIN);
    return ret_;
}

// ─────────────────────────────────────────────────────────────────────────────
// EpollState IO factory methods
// ─────────────────────────────────────────────────────────────────────────────
EpollOpAwaitable EpollState::recv(int fd, void *buff, size_t len, int flags) {
    return {this, fd, EpollOpType::IN,
            [=]() -> ssize_t { return ::recv(fd, buff, len, flags); }};
}

EpollOpAwaitable EpollState::send(int fd, const void *buff, size_t len, int flags) {
    return {this, fd, EpollOpType::OUT,
            [=]() -> ssize_t { return ::send(fd, buff, len, flags); }};
}

EpollOpAwaitable EpollState::accept_ll(int sockfd, struct sockaddr *addr,
                                       socklen_t *addrlen) {
    if (ep_state_ == State::DRAINING) {
        // Return an awaitable that immediately reports ESHUTDOWN
        return {this, sockfd, EpollOpType::IN, [=]() -> ssize_t {
            errno = ESHUTDOWN; return -1;
        }};
    }
    return {this, sockfd, EpollOpType::IN,
            [=]() -> ssize_t { return (ssize_t)::accept(sockfd, addr, addrlen); }};
}

EpollOpAwaitable EpollState::accept(int sockfd, struct sockaddr *addr,
                                    socklen_t *addrlen) {
    if (ep_state_ == State::DRAINING) {
        return {this, sockfd, EpollOpType::IN, [=]() -> ssize_t {
            errno = ESHUTDOWN; return -1;
        }};
    }
    // reg_=true: register the new fd after the accept completes
    return {this, sockfd, EpollOpType::IN,
            [=]() -> ssize_t { return (ssize_t)::accept(sockfd, addr, addrlen); },
            /* reg= */ true};
}

EpollOpAwaitable EpollState::sendmsg(int fd, const struct msghdr *msg, int flags) {
    return {this, fd, EpollOpType::OUT,
            [=]() -> ssize_t { return ::sendmsg(fd, msg, flags); }};
}

EpollOpAwaitable EpollState::recvmsg(int fd, struct msghdr *msg, int flags) {
    return {this, fd, EpollOpType::IN,
            [=]() -> ssize_t { return ::recvmsg(fd, msg, flags); }};
}

} // end namespace trt
