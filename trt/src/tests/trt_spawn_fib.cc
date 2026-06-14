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
#include "trt/uapi/trt.hh"

using namespace trt;

CoroTask fib(void *arg) {
    uintptr_t x = (uintptr_t)arg;
    RetT ret;

    if (x == 1 || x == 2) co_return 1;

    co_await T::spawn(fib, (void *)(x - 1));
    co_await T::spawn(fib, (void *)(x - 2));

    ret  = std::get<0>(co_await T::task_wait());
    ret += std::get<0>(co_await T::task_wait());

    co_return ret;
}

CoroTask start(void *arg) {
    printf("%s: %lu\n", __FUNCTION__, (uintptr_t)arg);
    co_await T::spawn(fib, arg, (void *)0xf11f11);
    auto [ret, ctx] = co_await T::task_wait();
    assert(ctx == (void *)0xf11f11);
    printf("%s: ret  : %llu\n", __FUNCTION__, (unsigned long long)ret);
    co_return 0;
}

int main(int argc, char *argv[]) {
    Controller c;
    c.spawn_scheduler(start, (void *)10, TaskType::TASK);

    return 0;
}
