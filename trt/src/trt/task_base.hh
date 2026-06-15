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

#ifndef TRT_TASK_BASE_HH
#define TRT_TASK_BASE_HH

#include <coroutine>
#include <cstdint>
#include <boost/intrusive/list.hpp>
namespace bi = boost::intrusive;

namespace trt {

class Scheduler;
class AsyncObjBase;
class TaskQueueThreadUnsafe;
class T;

using RetT = uint64_t;

// Task types
// POLL:   Tasks that poll I/O devices (e.g., network or storage).
// TASK:   short-running tasks, typically created to serve requests
enum class TaskType {POLL, TASK, NR_};

constexpr size_t nTaskTypes() { return static_cast<size_t>(TaskType::NR_); }

class TaskBase {
    friend Scheduler;
    friend T;
    friend TaskQueueThreadUnsafe;

public:
    enum class State {
        INITIALIZED = 1,
        READY,
        WAITING,
        FINISHED,
    };

protected:
    bi::list_member_hook<> t_lhook_; // list hook for scheduler
    TaskType t_type_;
    Scheduler *t_last_scheduler;     // last scheduler that scheduled the task
    std::coroutine_handle<> t_handle_;        // top-level coroutine frame
    std::coroutine_handle<> t_current_coro_; // leaf coroutine to resume (updated at each true suspension)
    TaskBase *t_parent_;             // parent task (or NULL)
    bool t_detached_;                // is task detached?
    RetT t_ret_ = 0;                 // return value set by promise_type::return_value

    #if !defined(NDEBUG)
    State t_state_;
    uint64_t t_dbg_id_;
    #endif

public:
    using List = bi::list<TaskBase,
                          bi::member_hook<TaskBase,
                                          bi::list_member_hook<>,
                                          &TaskBase::t_lhook_>>;

    void inline set_state(State s) {
        #if !defined(NDEBUG)
        t_state_ = s;
        #endif
    }

    void set_ret(RetT val) noexcept { t_ret_ = val; }
    TaskType get_type() const noexcept { return t_type_; }
    void set_current_coro(std::coroutine_handle<> h) noexcept { t_current_coro_ = h; }

protected:
    TaskBase(TaskBase *parent, bool t_detached, TaskType type);
public:
    TaskBase() = delete;
    TaskBase(TaskBase const &) = delete;
    void operator=(TaskBase const &) = delete;

    virtual ~TaskBase();
    virtual AsyncObjBase *get_ret_ao() = 0;
};

} // end trt namespace

#endif /* ifndef TRT_TASK_BASE_HH */
