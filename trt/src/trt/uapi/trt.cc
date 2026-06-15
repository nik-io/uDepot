/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#include <cassert>

#include "trt/uapi/trt.hh"
#include "trt/scheduler.hh"
#include "trt/controller.hh"
#include "trt/sync_base_types.hh"


namespace trt {

extern __thread Scheduler *localScheduler__;

// ── Awaitables ───────────────────────────────────────────────────────────────

YieldAwaitable T::yield() {
    return YieldAwaitable{};
}

SpawnAwaitable T::spawn_task(Task *t) {
    return SpawnAwaitable{t};
}

SpawnAwaitable T::spawn(TaskFn fn, TaskFnArg arg, void *caller_ctx,
                        bool detached, TaskType type) {
    Task *t = T::alloc_task(fn, arg, caller_ctx, detached, type);
    return SpawnAwaitable{t};
}

SpawnManyAwaitable T::spawn_many(Task::List &tl) {
    return SpawnManyAwaitable{tl};
}

WaitAwaitable T::wait_(WaitsetBase *ws) {
    return WaitAwaitable{ws};
}

WaitAwaitable T::task_wait_() {
    assert(dynamic_cast<Task *>(localScheduler__->s_current_) != nullptr);
    Task *self = static_cast<Task *>(localScheduler__->s_current_);
    return WaitAwaitable{&self->t_ws_};
}

TaskWaitAwaitable T::task_wait() {
    assert(dynamic_cast<Task *>(localScheduler__->s_current_) != nullptr);
    Task *self = static_cast<Task *>(localScheduler__->s_current_);
    return TaskWaitAwaitable{&self->t_ws_};
}

TaskWaitAwaitable T::wait(WaitsetBase *ws) {
    return TaskWaitAwaitable{ws};
}

LsaoAwaitable T::local_single_wait(LocalSingleAsyncObj *lsao) {
    return LsaoAwaitable{lsao};
}

LsnSubmitAwaitable T::local_single_notify_submit() {
    return LsnSubmitAwaitable{};
}

NotifySubmitAwaitable T::notify_submit() {
    return NotifySubmitAwaitable{};
}

// ── Synchronous helpers ───────────────────────────────────────────────────────

Task *
T::alloc_task(TaskFn fn, TaskFnArg arg, void *caller_ctx,
              bool detached, TaskType type) {
    Task *t;
    Scheduler *s = localScheduler__;
    if (s) { // we are in trt context
        TaskBase *parent_base = s->s_current_;
        Task *parent = static_cast<Task *>(parent_base);
        assert(dynamic_cast<Task *>(parent_base) != nullptr);
        s->task_alloc(&t, fn, arg, caller_ctx, parent, detached, type);
    } else {
        // not in trt context: remote task allocation
        Task *parent = nullptr;
        t = TaskAllocationQueue::task_alloc<Task>(fn, arg, caller_ctx, parent, detached, type);
    }
    return t;
}

void
T::free_task(Task *t) {
    Scheduler *s = localScheduler__;
    if (s) {
        s->task_free(t);
    } else {
        TaskAllocationQueue::task_free(t);
    }
}

void T::notify(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
    localScheduler__->notify_(ao, val, p);
}

void T::notify_init() {
    localScheduler__->s_notify_batch_.nargs = 0;
}

bool T::notify_add(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
    auto &batch = localScheduler__->s_notify_batch_;
    size_t idx = batch.nargs;
    if (idx >= TRT_MAX_NOTIFY_ARGS)
        return false;
    batch.args[idx] = std::make_tuple(ao, val, p);
    batch.nargs = idx + 1;
    return true;
}

void T::local_single_notify_init() {
    localScheduler__->s_lsn_batch_.nargs = 0;
}

bool T::local_single_notify_add(LocalSingleAsyncObj *lsao, RetT val) {
    auto &batch = localScheduler__->s_lsn_batch_;
    size_t idx = batch.nargs;
    if (idx >= TRT_MAX_NOTIFY_ARGS)
        return false;
    batch.args[idx] = std::make_tuple(lsao, val);
    batch.nargs = idx + 1;
    return true;
}

void T::local_single_notify(LocalSingleAsyncObj *lsao, RetT val) {
    // Directly set the value and push the waiting task (no batch, no yield).
    Scheduler *s = localScheduler__;
    lsao->set_val(val);
    TaskBase *waiter = lsao->take_waiter();
    if (waiter != nullptr)
        s->push_task_front(*waiter);
}

size_t T::rand() {
    return localScheduler__->rand();
}

Task &T::self() {
    assert(dynamic_cast<Task *>(localScheduler__->s_current_) != nullptr);
    Task *self = static_cast<Task *>(localScheduler__->s_current_);
    return *self;
}

#if !defined(NDEBUG)
uint64_t T::tid() {
    Scheduler *s = localScheduler__;
    if (s == nullptr || s->s_current_ == nullptr)
        return (uint64_t)-1;
    return s->s_current_->t_dbg_id_;
}

uint64_t T::sid() {
    Scheduler *s = localScheduler__;
    return s ? s->s_dbg_id_ : (uint64_t)-1;
}
#endif

void T::set_exit_all() {
    localScheduler__->s_controller_.set_exit_all();
}

Scheduler *T::getS() {
    return localScheduler__;
}

void T::spawn_detached_no_wait(TaskFn fn, TaskFnArg arg, TaskType type) {
    Task *t = alloc_task(fn, arg, nullptr, true, type);
    t->set_state(Task::State::READY);
    localScheduler__->push_task_front(*t);
}

void T::remote_spawn(TaskFn fn, TaskFnArg fn_arg, void *ctx, bool detached,
                     TaskType ty, RemoteSpawnPolicy p) {
    (void)fn; (void)fn_arg; (void)ctx; (void)detached; (void)ty; (void)p;
    fprintf(stderr, "%s:%d: NYI!", __PRETTY_FUNCTION__, __LINE__);
    abort();
}

void Task::dealloc_task__(AsyncObj *unused, void *t__) {
    (void)unused;
    Task *t = static_cast<Task *>(t__);
    localScheduler__->task_free(t);
}

#if !defined(NDEBUG)
extern "C" {
uint64_t trt_dbg_get_tid() { return trt::T::tid(); }
uint64_t trt_dbg_get_sid() { return trt::T::sid(); }
}
#endif

} // end namespace trt
