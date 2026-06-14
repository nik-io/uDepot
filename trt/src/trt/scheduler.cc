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

#include "trt/sync_base_types.hh"
#include "trt/task_base.hh"
#include "trt/local_single_sync.hh"
#include "trt/scheduler.hh"
#include "trt/uapi/trt.hh"

#define xdbg_print_str__ "S%-10ld %20s()"
#define xdbg_print_arg__ trt::T::sid(), __FUNCTION__
#define xdbg_print(msg, fmt, args...) \
    printf(xdbg_print_str__ " " msg fmt , xdbg_print_arg__ , ##args)

#if !defined(NDEBUG)
    #define dmsg(fmt,args...) xdbg_print("",fmt, ##args)
#else
    #define dmsg(fmt,args...) do { } while (0)
#endif


namespace trt {

__thread Scheduler *localScheduler__ = nullptr;

#if !defined(NDEBUG)
uint64_t dbg_sched_id(void) {
    static std::atomic<uint64_t> cnt(0);
    return cnt.fetch_add(1);
}

uint64_t dbg_task_id(void) {
    static std::atomic<uint64_t> cnt(0);
    return cnt.fetch_add(1);
}
#endif

Scheduler::Scheduler(ControllerBase &controller, cpu_set_t cpuset,
                     TaskFn main_fn, TaskFnArg main_arg, TaskType main_type)
    : s_state_(State::INIT),
      s_current_(nullptr),
      s_cpuset_(cpuset),
      s_controller_(controller),
      s_rand_eng_(time(NULL)),
      s_io_npending_(0) {
    pthread_barrier_init(&s_barrier_, NULL, 2);

    #if !defined(NDEBUG)
    s_dbg_id_ = dbg_sched_id();
    #endif

    Task *main_task;
    task_alloc(&main_task, main_fn, main_arg, nullptr, nullptr, true, main_type);
    if (main_task == nullptr) {
        fprintf(stderr, "Could not allocate main task. Dying ungracfully.\n");
        abort();
    }
    main_task->set_state(Task::State::READY);
    push_task_front(*main_task);
}

Scheduler::~Scheduler() {}

void Scheduler::schedule_task_(TaskBase *t) {
    assert(t != nullptr);
    s_current_ = t;
    t->t_last_scheduler = this;
    t->t_current_coro_.resume(); // resume leaf coroutine; runs until next true suspension or co_return
    s_current_ = nullptr;
    if (t->t_handle_.done())
        handle_task_done_(t);
}

// Called after a task's coroutine body has completed (final_suspend reached).
void Scheduler::handle_task_done_(TaskBase *t_base) {
    Task *t = static_cast<Task *>(t_base);
    RetT ret = t->t_ret_;

    // Destroy the coroutine frame (frame is alive until explicitly destroyed).
    std::coroutine_handle<> h = std::exchange(t->t_handle_, {});
    h.destroy();

    if (t->t_detached_) {
        // Fast path: detached task — just free it.
        task_free(t);
        return;
    }
    // Non-detached: notify parent waitset with the return value.
    notify_(&t->t_ret_ao_, ret, NotifyPolicy::LocalSched);
    // The task memory is freed via the AsyncObj's dealloc callback
    // (Task::dealloc_task__) once all references to t_ret_ao_ are dropped.
}

void Scheduler::notify_(AsyncObjBase *ao, RetT val, NotifyPolicy p) {
    FutureBase::AoList fl = ao->set_ready(val);
    while (fl.size() > 0) {
        FutureBase &f = fl.front();
        fl.pop_front();
        TaskBase *t = f.set_ready();
        if (t == nullptr)
            continue;

        switch (p) {
            case NotifyPolicy::LastTaskScheduler: {
                if (t->t_last_scheduler == this) {
                    push_task_front(*t);
                    break;
                }
                bool ok = t->t_last_scheduler->remote_push_task_front(*t);
                if (ok)
                    break;
                [[gnu::fallthrough]];
            }
            case NotifyPolicy::LocalSched:
                push_task_back(*t);
                break;

            default:
                abort();
        }
    }
}

void Scheduler::sched_setaffinity() {
    int err = ::sched_setaffinity(0, sizeof(s_cpuset_), &s_cpuset_);
    if (err) {
        perror("sched_setaffinity");
        exit(1);
    }
}

void Scheduler::start_() {
    assert(s_state_ == State::RUNNING);
    auto &task_q   = get_tq_(TaskType::TASK);
    auto &poll_q   = get_tq_(TaskType::POLL);
    auto &remote_q = s_remote_tq_;

    const bool print_ctx_switches = false;

    while (true) {
        remote_q.push_to_queue_back(task_q);

        const size_t ntasks_low = 20, ntasks_high = 25;
        if (task_q.size() < ntasks_low) {
            for (size_t p = 0; p < poll_q.size(); ++p) {
                TaskBase *t = poll_q.pop_front();
                if (t == nullptr) break;
                if (print_ctx_switches)
                    dmsg("Scheduling polling task: %lu (%p)\n", t->t_dbg_id_, t);
                schedule_task_(t);
                if (task_q.size() >= ntasks_high) break;
            }
        }

        TaskBase *t = task_q.pop_front();
        if (t != nullptr) {
            if (print_ctx_switches)
                dmsg("Scheduling work task: %lu (%p)\n", t->t_dbg_id_, t);
            schedule_task_(t);
        } else if (poll_q.size() == 0 && s_ctl_.exit_.load()) {
            t = task_q.pop_front_or_stop();
            if (t != nullptr) {
                if (print_ctx_switches)
                    dmsg("Scheduling work task: %lu (%p)\n", t->t_dbg_id_, t);
                schedule_task_(t);
            } else {
                #if !defined(NDEBUG)
                s_state_ = State::DONE;
                printf("S%zd: Nothing to schedule, exiting\n", s_dbg_id_);
                #endif
                break;
            }
        }
    }
}

void Scheduler::thread_init(Scheduler *s) {
    assert(localScheduler__ == nullptr);
    localScheduler__ = s;
    s->s_self_tid_ = pthread_self();
}

// ─────────────────────────────────────────────────────────────────────────────
// Awaitable implementations (need full Scheduler definition)
// ─────────────────────────────────────────────────────────────────────────────

void YieldAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    Scheduler *s = localScheduler__;
    s->s_current_->set_current_coro(h);
    s->push_task_back(*s->s_current_);
}

void SpawnAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    Scheduler *s = localScheduler__;
    TaskBase *prev = s->s_current_;
    prev->set_current_coro(h);
    new_task_->set_state(Task::State::READY);

    switch (prev->get_type()) {
        case TaskType::TASK: s->push_task_front(*prev); break;
        case TaskType::POLL: s->push_task_back(*prev);  break;
        default: abort();
    }
    s->push_task_front(*new_task_);
}

void SpawnManyAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    Scheduler *s = localScheduler__;
    TaskBase *prev = s->s_current_;
    prev->set_current_coro(h);
    auto &q = s->get_tq_(TaskType::TASK);
    q.push_back(tl_);
    s->push_task_back(*prev);
}

bool LsaoAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    Scheduler *s = localScheduler__;
    s->s_current_->set_current_coro(h);
    lsao_->set_waiter(s->s_current_);
    return true; // always suspend; woken up by the poller via direct push
}

bool WaitAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    // Loop to handle REDO races: if SCANNING→WAITING fails, re-scan.
    while (true) {
        if (ws_->try_set_state_to_waiting()) {
            localScheduler__->s_current_->set_current_coro(h);
            return true;  // successfully sleeping
        }
        result_ = ws_->try_wait_();
        if (result_)
            return false; // found a result; don't suspend
        // REDO race happened again: loop
    }
}

void LsnSubmitAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    Scheduler *s = localScheduler__;
    s->s_current_->set_current_coro(h);
    // Push current task to back so it runs after woken tasks.
    s->push_task_back(*s->s_current_);
    // Process the LSN batch: set values and push woken tasks.
    for (size_t i = 0; i < s->s_lsn_batch_.nargs; i++) {
        auto &[lsao, val] = s->s_lsn_batch_.args[i];
        lsao->set_val(val);
        TaskBase *waiter = lsao->take_waiter();
        if (waiter != nullptr)
            s->push_task_front(*waiter);
    }
    s->s_lsn_batch_.nargs = 0;
}

void NotifySubmitAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    Scheduler *s = localScheduler__;
    s->s_current_->set_current_coro(h);
    s->push_task_back(*s->s_current_);
    for (size_t i = 0; i < s->s_notify_batch_.nargs; i++) {
        auto &[ao, val, p] = s->s_notify_batch_.args[i];
        s->notify_(ao, val, p);
    }
    s->s_notify_batch_.nargs = 0;
}

} // end namespace trt
