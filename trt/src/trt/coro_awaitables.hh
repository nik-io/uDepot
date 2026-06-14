/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *           Nikolas Ioannou (nio@zurich.ibm.com, nicioan@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop=4 shiftwidth=4:

#ifndef TRT_CORO_AWAITABLES_HH_
#define TRT_CORO_AWAITABLES_HH_

#include <coroutine>
#include <tuple>

#include "trt/task_base.hh"
#include "trt/local_single_sync.hh"
#include "trt/sync_base_types.hh"

// Forward declarations — full definitions come from including scheduler.hh
// (which includes this header indirectly via task.hh).
namespace trt {

class Scheduler;
class Task;

// Defined in scheduler.cc (TLS pointer to current scheduler)
extern __thread Scheduler *localScheduler__;

// ─────────────────────────────────────────────────────────────────────────────
// YieldAwaitable
// Puts the current task at the back of the run queue and suspends.
// ─────────────────────────────────────────────────────────────────────────────
struct YieldAwaitable {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SpawnAwaitable
// Spawns a new Task and re-queues the current task according to its type.
// ─────────────────────────────────────────────────────────────────────────────
struct SpawnAwaitable {
    Task *new_task_;
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// ─────────────────────────────────────────────────────────────────────────────
// SpawnManyAwaitable
// Spawns a list of tasks and puts the current task at the back of the queue.
// ─────────────────────────────────────────────────────────────────────────────
struct SpawnManyAwaitable {
    TaskBase::List &tl_;
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// ─────────────────────────────────────────────────────────────────────────────
// LsaoAwaitable
// Waits for a LocalSingleAsyncObj to become ready.
// If already ready, doesn't suspend.
// ─────────────────────────────────────────────────────────────────────────────
struct LsaoAwaitable {
    LocalSingleAsyncObj *lsao_;

    bool await_ready() noexcept { return lsao_->is_ready(); }

    // Sets t_current_coro_ to h and the current task as waiter; suspends.
    bool await_suspend(std::coroutine_handle<> h) noexcept;

    RetT await_resume() noexcept { return lsao_->get_ret(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// WaitAwaitable
// Waits until a WaitsetBase has a ready FutureBase.
//
// await_ready():   calls ws->try_wait_(); returns true if a future is ready.
// await_suspend(): loops: tries SCANNING→WAITING; on REDO race re-scans.
//                  Returns true (suspend) when successfully sleeping,
//                  returns false (don't suspend) when result found via REDO loop.
// await_resume():  if woken from sleep, re-scans to obtain the ready future.
// ─────────────────────────────────────────────────────────────────────────────
struct WaitAwaitable {
    WaitsetBase *ws_;
    FutureBase  *result_ = nullptr;

    bool await_ready() noexcept {
        result_ = ws_->try_wait_();
        return result_ != nullptr;
    }

    bool await_suspend(std::coroutine_handle<> h) noexcept;

    FutureBase *await_resume() noexcept {
        if (result_) return result_;
        // Woken from WAITING state: at least one future must be ready now.
        result_ = ws_->try_wait_();
        return result_;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// LsnSubmitAwaitable
// Submits the LocalSingleNotify batch: pushes woken tasks to queue and yields.
// Replaces T::local_single_notify_submit().
// ─────────────────────────────────────────────────────────────────────────────
struct LsnSubmitAwaitable {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NotifySubmitAwaitable
// Submits the regular Notify batch and yields.
// Replaces T::notify_submit().
// ─────────────────────────────────────────────────────────────────────────────
struct NotifySubmitAwaitable {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept {}
};

// ─────────────────────────────────────────────────────────────────────────────
// TaskWaitAwaitable
// Convenience wrapper around WaitAwaitable that resumes with
// std::tuple<RetT, void*> instead of FutureBase*.
// Usage: auto [ret, ctx] = co_await T::task_wait();
// ─────────────────────────────────────────────────────────────────────────────
struct TaskWaitAwaitable {
    WaitAwaitable inner_;

    explicit TaskWaitAwaitable(WaitsetBase *ws) : inner_{ws} {}

    bool await_ready() noexcept { return inner_.await_ready(); }

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        return inner_.await_suspend(h);
    }

    std::tuple<RetT, void *> await_resume() noexcept {
        FutureBase *f = inner_.await_resume();
        if (!f) return {0, nullptr};
        return {f->get_val(), f->get_ctx()};
    }
};

} // namespace trt

#endif // TRT_CORO_AWAITABLES_HH_
