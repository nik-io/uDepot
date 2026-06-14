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

#ifndef TRT_UAPI_TRT_HH_
#define TRT_UAPI_TRT_HH_

#include "trt/task_base.hh"
#include "trt/async_obj.hh"
#include "trt/task.hh"
#include "trt/scheduler.hh"
#include "trt/controller.hh"
#include "trt/local_sync.hh"

namespace trt {

// task interface
//
// NB: using static functions in a class instead of a namespace to allow for
// easy befriending.
class T {
    T() = delete;
    T(T const &) = delete;
    void operator=(T const &) = delete;

public:
    // ── Awaitables (must be co_await'd in task coroutines) ───────────────────

    // Yield: puts current task at back of queue; scheduler runs other tasks.
    static YieldAwaitable yield();

    // Spawn a single task; re-queues the current task.
    static SpawnAwaitable spawn_task(Task *t);

    // Allocate + spawn in one step.
    static SpawnAwaitable spawn(TaskFn fn, TaskFnArg arg,
                                void *caller_ctx = nullptr,
                                bool detached = false,
                                TaskType type = TaskType::TASK);

    // Spawn many tasks at once.
    static SpawnManyAwaitable spawn_many(Task::List &tl);

    // Wait on an arbitrary WaitsetBase; co_await yields Future*.
    static WaitAwaitable wait_(WaitsetBase *ws);

    // Wait on the current task's own waitset; co_await yields Future*.
    static WaitAwaitable task_wait_();

    // Convenience: co_await yields std::tuple<RetT, void*> (return_val, caller_ctx).
    static TaskWaitAwaitable task_wait();

    // Convenience: co_await T::wait(&ws) yields std::tuple<RetT, void*>.
    static TaskWaitAwaitable wait(WaitsetBase *ws);

    // Wait on a LocalSingleAsyncObj; co_await yields RetT.
    static LsaoAwaitable local_single_wait(LocalSingleAsyncObj *lsao);

    // Submit the pending LSN notification batch and yield.
    static LsnSubmitAwaitable local_single_notify_submit();

    // Submit the pending regular notification batch and yield.
    static NotifySubmitAwaitable notify_submit();

    // ── Synchronous helpers (no suspension) ──────────────────────────────────

    // Allocate a Task (does not spawn it yet).
    static Task *alloc_task(TaskFn fn, TaskFnArg arg,
                            void *caller_ctx = nullptr, bool detached = false,
                            TaskType type = TaskType::TASK);

    static void free_task(Task *t);

    // Notify an async object directly (no suspension needed).
    static void notify(AsyncObjBase *ao, RetT val,
                       NotifyPolicy p = NotifyPolicy::LocalSched);

    // Batched notify (init → add… → notify_submit).
    static void notify_init();
    static bool notify_add(AsyncObjBase *ao, RetT val, NotifyPolicy p = NotifyPolicy::LocalSched);

    // Batched LSN notify (init → add… → local_single_notify_submit).
    static void local_single_notify_init();
    static bool local_single_notify_add(LocalSingleAsyncObj *lsao, RetT val);

    // Single-shot LSN notify (no suspension; directly pushes woken task).
    static void local_single_notify(LocalSingleAsyncObj *lsao, RetT val);

    // ── Misc ─────────────────────────────────────────────────────────────────
    static size_t rand();
    static Task &self();

    #if !defined(NDEBUG)
    static uint64_t tid();  // current task debug id
    static uint64_t sid();  // scheduler debug id
    #endif

    static void set_exit_all();
    static Scheduler *getS();
    static bool in_trt() { return getS() != nullptr; }

    static void io_npending_inc(size_t x)  { getS()->s_io_npending_ += x; }
    static void io_npending_dec(size_t x)  {
        assert(getS()->s_io_npending_ >= x);
        getS()->s_io_npending_ -= x;
    }
    static size_t io_npending_get()        { return getS()->s_io_npending_; }

    // Spawn a detached task and push it to the front of the run queue without
    // suspending the caller. Safe to call from non-coroutine init code.
    static void spawn_detached_no_wait(TaskFn fn, TaskFnArg arg = nullptr,
                                       TaskType type = TaskType::POLL);

    // Remote spawn (NYI)
    static void remote_spawn(TaskFn fn, TaskFnArg, void *caller_ctx = nullptr,
                             bool detached = false, TaskType ty = TaskType::TASK,
                             RemoteSpawnPolicy p = RemoteSpawnPolicy::RoundRobin);
};

} // end namespace trt

#if !defined(NDEBUG)
#define trt_dbg_print_str__ "S%-4ld:T%-4ld %20s()"
#define trt_dbg_print_arg__ ::trt::T::sid(), ::trt::T::tid(), __FUNCTION__
#else
#define trt_dbg_print_str__ "%20s()"
#define trt_dbg_print_arg__ __FUNCTION__
#endif
#define trt_dbg_print(msg, fmt, args...) \
    printf(trt_dbg_print_str__ " " msg fmt , trt_dbg_print_arg__ , ##args)

#if !defined(NDEBUG)
    #define trt_dmsg(fmt,args...) trt_dbg_print("", fmt, ##args)
#else
    #define trt_dmsg(fmt,args...) do { } while (0)
#endif
#define trt_msg(fmt,args...) trt_dbg_print("", fmt, ##args)
#define trt_err(fmt,args...) \
    fprintf(stderr, trt_dbg_print_str__ " " fmt , trt_dbg_print_arg__ , ##args)

#endif // TRT_UAPI_TRT_HH_
