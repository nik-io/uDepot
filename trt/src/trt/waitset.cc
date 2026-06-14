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

#include "trt/task_base.hh"
#include "trt/waitset.hh"
#include "trt/uapi/trt.hh"

namespace trt {

Waitset::Waitset() : Waitset(T::self()) {};

// Called when an async object pointed by a future becomes ready.
// Returns the owner task if it should be woken, nullptr otherwise.
// Races with itself (multiple futures) and try_wait_().
TaskBase *
Waitset::set_ready(void) {
    for (;;) {
        switch (ws_state_.load(std::memory_order_relaxed)) {
            case State::WAITING:
                if (cas_state_(State::WAITING, State::INITIAL))
                    return &ws_owner_;
                break; /* retry */

            case State::SCANNING:
                if (cas_state_(State::SCANNING, State::REDO))
                    return nullptr;
                break; /* retry */

            case State::REDO:
                return nullptr;

            case State::INITIAL:
                return nullptr;
        }
    }
}

// Non-blocking scan: returns a ready Future* or nullptr (caller must sleep).
// Handles REDO races internally by re-scanning. Leaves state as SCANNING on nullptr.
Future *
Waitset::try_wait_() {
    if (nfutures() == 0)
        return nullptr;

redo:
    ws_state_.store(State::SCANNING, std::memory_order_relaxed);

    for (FutureBase &f_base : ws_futures_registered_) {
        Future *f = static_cast<Future *>(&f_base);
        if (f->is_ready()) {
            ws_futures_registered_.erase(ws_futures_registered_.iterator_to(f_base));
            ws_state_.store(State::INITIAL, std::memory_order_relaxed);
            f->wait_completed();
            return f;
        }
    }

    while (ws_futures_unchecked_.size() > 0) {
        FutureBase &f_base = ws_futures_unchecked_.front();
        Future *f = static_cast<Future *>(&f_base);
        ws_futures_unchecked_.pop_front();
        if (f->is_ready_or_register()) {
            ws_state_.store(State::INITIAL, std::memory_order_relaxed);
            f->wait_completed();
            return f;
        } else {
            ws_futures_registered_.push_back(f_base);
        }
    }

    // No ready future found. Check for REDO race.
    if (ws_state_.load(std::memory_order_relaxed) != State::SCANNING) {
        // State changed to REDO while we were scanning; re-scan.
        goto redo;
    }

    // State is still SCANNING: caller should sleep via try_set_state_to_waiting().
    return nullptr;
}

// Full wait using try_wait_() internally.
// In coroutine context this is superseded by co_await T::wait_(ws).
Future *
Waitset::wait_(void) {
    Future *f;
    while (!(f = try_wait_()))
        ; // spin (should not be reached in coroutine-based TRT)
    return f;
}


} // end namespace trt
