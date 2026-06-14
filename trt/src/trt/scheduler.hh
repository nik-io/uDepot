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

#ifndef TRT_SCHEDULER_HH_
#define TRT_SCHEDULER_HH_

#include <atomic>
#include <cinttypes>
#include <random>
#include <tuple>

#include "trt/sync_base_types.hh"
#include "trt/local_single_sync.hh"
#include "trt/task_queue.hh"
#include "trt/task_alloc.hh"
#include "trt/task.hh"
#include "trt/scheduler_cmd.hh"

#define TRT_MAX_NOTIFY_ARGS 128

namespace trt {

// forward declarations for befriending
class ControllerBase;
class Waitset;
class LocalWaitset;
class Task;
class T;

// (global) ids for schedulers and tasks (debug only)
#if !defined(NDEBUG)
uint64_t dbg_sched_id(void);
uint64_t dbg_task_id(void);
#endif

struct SchedCtl {
    std::atomic<bool> exit_;
    std::atomic<bool> app_done_;

    SchedCtl() : exit_(false), app_done_(false) {}

    void set_exit(void)     { exit_.store(true); }
};

// Batch for LocalSingleAsyncObj notifications (single-core, single-waiter path).
struct LsnBatch {
    std::tuple<LocalSingleAsyncObj *, RetT> args[TRT_MAX_NOTIFY_ARGS];
    size_t nargs = 0;
};

// Batch for regular AsyncObj notifications (multi-core, multi-waiter path).
struct NotifyBatch {
    std::tuple<AsyncObjBase *, RetT, NotifyPolicy> args[TRT_MAX_NOTIFY_ARGS];
    size_t nargs = 0;
};

class Scheduler {
    friend ControllerBase;
    friend Waitset;
    friend LocalWaitset;
    friend Task;
    friend T;
    friend struct YieldAwaitable;
    friend struct SpawnAwaitable;
    friend struct SpawnManyAwaitable;
    friend struct LsaoAwaitable;
    friend struct WaitAwaitable;
    friend struct LsnSubmitAwaitable;
    friend struct NotifySubmitAwaitable;

   public:
    enum class State { INIT = 1, RUNNING, DONE };

   protected:
    State s_state_;

    // per-type run queues (single-core, no synchronization)
    TaskQueueThreadUnsafe s_tqs_[nTaskTypes()];
    // remote queue (multi-core, with locking)
    TaskQueueRemote       s_remote_tq_;

    SchedCtl   s_ctl_;
    TaskBase  *s_current_;

    #if !defined(NDEBUG)
    uint64_t s_dbg_id_;
    #endif

    pthread_barrier_t s_barrier_;
    pthread_t         s_self_tid_;
    cpu_set_t         s_cpuset_;

    TaskAllocationQueue s_task_allocq_;
    ControllerBase     &s_controller_;

    std::default_random_engine          s_rand_eng_;
    std::uniform_int_distribution<size_t> s_rand_dist_;

    size_t s_io_npending_;

    // Pending notification batches (filled by T::xxx_notify_add; flushed by awaitables)
    LsnBatch    s_lsn_batch_;
    NotifyBatch s_notify_batch_;

   public:
    Scheduler(const Scheduler &) = delete;
    void operator=(const Scheduler &) = delete;
    Scheduler() = delete;

    Scheduler(ControllerBase &controller, cpu_set_t cpuset,
              TaskFn main_fn, TaskFnArg main_arg, TaskType main_type);

    ~Scheduler();

    void start_(void);
    State get_state(void) { return s_state_; }

    void set_state_running() {
        assert(s_state_ == State::INIT);
        s_state_ = State::RUNNING;
    }
    static void thread_init(Scheduler *s);
    void sched_setaffinity(void);
    pthread_t get_pthread_tid(void) { return s_self_tid_; }
    void pthread_barrier_wait(void) {
        int err = ::pthread_barrier_wait(&s_barrier_);
        if (err != 0 && err != PTHREAD_BARRIER_SERIAL_THREAD) {
            fprintf(stderr, "pthread_barrier returned: %d\n", err);
            abort();
        }
    }
    cpu_set_t get_cpuset() { return s_cpuset_; }

    // Public API for IO backend completions (AioOp, IouOp) and awaitables.
    void wake_task(TaskBase &t) { push_task_front(t); }
    TaskBase *current_task() const noexcept { return s_current_; }

    #if !defined(NDEBUG)
    uint64_t getId(void) const { return s_dbg_id_; }
    #endif

   private:
    TaskQueueThreadUnsafe &get_tq_(TaskType t) { return s_tqs_[static_cast<size_t>(t)]; }

    void schedule_task_(TaskBase *t);
    void handle_task_done_(TaskBase *t);

    void push_task_front(TaskBase &t) { get_tq_(t.t_type_).push_front(t); }
    void push_task_back(TaskBase &t)  { get_tq_(t.t_type_).push_back(t);  }

   public:
    __attribute__((warn_unused_result))
    bool remote_push_task_front(TaskBase &t) {
        assert(t.t_type_ == TaskType::TASK);
        return s_remote_tq_.push_back_if_running(t);
    }

   protected:
    template<typename T, typename... Args>
    void task_alloc(T **retp, Args &&...args) {
        *retp = s_task_allocq_.alloc<T, Args...>(std::forward<Args>(args)...);
    }

    void task_free(TaskBase *t) { s_task_allocq_.free(t); }

   private:
    void notify_(AsyncObjBase *aio, RetT val, NotifyPolicy p);

   public:
    size_t rand(void) { return s_rand_dist_(s_rand_eng_); }

   public:
    #if !defined(NDEBUG)
    size_t rwpf_rb_count;
    #endif
};

} // end namespace trt

// Include awaitables here (after Scheduler is fully declared) so that
// the awaitable implementations can access Scheduler internals.
#include "trt/coro_awaitables.hh"

#endif // TRT_SCHEDULER_HH_
