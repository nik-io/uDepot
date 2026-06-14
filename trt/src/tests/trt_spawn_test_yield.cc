/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#include "trt/uapi/trt.hh"

using namespace trt;

#define NUM (0x100)

CoroTask t2(void *arg) {
	trt_dmsg("yielding\n");
	co_await T::yield();
	uintptr_t x = (uintptr_t)arg;
	trt_dmsg("returning\n");
	co_return (RetT)(x + 1);
}

CoroTask t1(void *arg)
{
	trt_dmsg("spawning t2\n");
	co_await T::spawn(t2, arg, (void *)0xf11f11);
	trt_dmsg("waiting t2\n");
	auto [ret, ctx] = co_await T::task_wait();
	assert(ret == (NUM + 1));
	assert(ctx == (void *)0xf11f11);
	trt_dmsg("returning\n");
	co_return 0;
}

int main(int argc, char *argv[])
{
	Controller c;
	c.spawn_scheduler(t1, (void *)NUM, TaskType::TASK);

	return 0;
}
