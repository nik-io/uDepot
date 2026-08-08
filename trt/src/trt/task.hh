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

#ifndef TRT_TASK_HH_
#define TRT_TASK_HH_

#include <coroutine>
#include <setjmp.h> // used for sigjmp_buf (rwlpf_rb_jmp)

#include "trt/task_base.hh"
#include "trt/waitset.hh"
#include "trt/async_obj.hh"

namespace trt {

class Scheduler;
class AsyncObj;

// CoroTask is the coroutine return type for all TRT task functions.
// Task functions must be declared as: CoroTask my_task(void *arg) { ... co_return val; }
//
// [[nodiscard]] is load-bearing, not hygiene. initial_suspend() is
// suspend_always, so *calling* a CoroTask function only builds the frame --
// none of the body runs until something resumes it. Discarding the result is
// therefore always a bug: the call silently does nothing and leaks the frame.
// This is exactly how uDepotLock::lock() came to be a no-op at four call
// sites, leaving the shared Mbuff cache unlocked (docs/concurrent-get-fix.md).
// Non-coroutine callers want lock_blocking(); coroutine callers must co_await.
class [[nodiscard]] CoroTask {
public:
    struct promise_type {
        std::coroutine_handle<> continuation_ = nullptr; // set when co_await'd by outer
        TaskBase *task_    = nullptr; // back-pointer set by Task constructor
        RetT      ret_val_ = 0;       // stores return value regardless of task_

        CoroTask get_return_object() noexcept {
            return CoroTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // Start suspended: scheduler or co_await drives the first resume().
        std::suspend_always initial_suspend() noexcept { return {}; }

        // On completion: resume the outer coroutine if one is waiting (via co_await),
        // otherwise stay suspended so the scheduler can detect done() == true.
        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> h) noexcept {
                if (auto cont = h.promise().continuation_)
                    return cont;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_value(RetT val) noexcept {
            ret_val_ = val;
            if (task_) task_->set_ret(val);
        }

        void unhandled_exception() noexcept { std::terminate(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    CoroTask() = default;
    explicit CoroTask(Handle h) noexcept : handle_(h) {}
    CoroTask(CoroTask &&o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    CoroTask &operator=(CoroTask &&o) noexcept {
        handle_ = std::exchange(o.handle_, {});
        return *this;
    }
    CoroTask(const CoroTask &) = delete;
    CoroTask &operator=(const CoroTask &) = delete;

    // No destroy in destructor: ownership is transferred to Task (or run_sync destroys it).
    ~CoroTask() = default;

    // Awaitable interface: allows co_await CoroTask within another coroutine.
    // Uses symmetric transfer — inner coroutine runs and resumes outer on completion.
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer) noexcept {
        handle_.promise().continuation_ = outer;
        return handle_; // symmetric transfer: start/resume the inner coroutine
    }

    RetT await_resume() noexcept {
        return handle_.promise().ret_val_;
    }

    // Drive synchronously to completion (only valid for coroutines that never truly suspend).
    // With symmetric transfer, all synchronous work completes in one resume() call.
    RetT run_to_completion() {
        assert(handle_);
        while (!handle_.done()) handle_.resume();
        return handle_.promise().ret_val_;
    }

    // Like run_to_completion() but also destroys the frame.
    // Use when the CoroTask is NOT transferred to a Task (e.g., non-TRT init contexts).
    RetT run_sync() noexcept {
        assert(handle_);
        while (!handle_.done()) handle_.resume();
        RetT val = handle_.promise().ret_val_;
        handle_.destroy();
        handle_ = {};
        return val;
    }

    Handle handle_;
};

// TaskFnArg and TaskFn types.
using TaskFnArg = void *;
using TaskFn    = CoroTask (*)(TaskFnArg);

class Task : public TaskBase {
    friend Scheduler;
    friend TaskQueueThreadUnsafe;
    friend T;

   private:
    spinlock_t t_lock_;
   protected:
    Waitset  t_ws_;
    AsyncObj t_ret_ao_;
    Future   t_parent_future_; // parent's future

   // rwlock_pagefault_trt fields
   public:
    int        rwpf_rb_set;
    sigjmp_buf rwpf_rb_jmp;

   public:
    Task() = delete;
    Task(Task const &) = delete;
    void operator=(Task const &) = delete;

    static void dealloc_task__(AsyncObj *ao, void *t);

    Task(TaskFn fn, TaskFnArg arg, void *caller_ctx = nullptr, Task *t_parent = nullptr,
         bool t_detached = false, TaskType type = TaskType::TASK)
     : TaskBase(t_parent, t_detached, type)
     , t_ws_(*this)
     , t_ret_ao_(dealloc_task__, this)
     , t_parent_future_(t_detached_ ? nullptr : &t_ret_ao_,
                        t_detached_ ? nullptr : &t_parent->t_ws_,
                        caller_ctx)
     , rwpf_rb_set(0) {
        spinlock_init(&t_lock_);
        // Instantiate the coroutine: suspends at initial_suspend().
        CoroTask ct = fn(arg);
        t_handle_ = ct.handle_;            // coroutine_handle<promise_type> → coroutine_handle<>
        t_current_coro_ = t_handle_;       // initially the leaf is the task itself
        ct.handle_.promise().task_ = this; // back-pointer for return_value
        ct.handle_ = {};                   // disarm ct so destructor is a no-op
    }

    virtual ~Task() {
        if (t_handle_) t_handle_.destroy();
    }

    AsyncObjBase *get_ret_ao(void) override { return &t_ret_ao_; }
};

} // end namespace trt

#endif // TRT_TASK_HH_
