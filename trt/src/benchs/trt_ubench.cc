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
extern "C" {
	#include "trt_util/timer.h"
}

#define TRT_MTASKS 20 // million of tasks

// This ubenchmark measures the throughput of spawning tasks. We spawn the tasks
// detached, which is meant to emulate a network server that spawns one new
// task per connection.

trt::CoroTask
t_worker(void *arg) {
	size_t *completed = static_cast<size_t *>(arg);
	*completed += 1;
	co_return 0;
}

static trt::CoroTask
t_spawner(void *arg__)
{
	size_t tasks_spawned          = 0;
	size_t tasks_total            = TRT_MTASKS*1000*1000;
	const size_t tasks_batch      = 64;
	const size_t tasks_runing_max = 1024;
	size_t trt_tasks_completed    = 0;

	while (tasks_spawned < tasks_total) {
		assert(trt_tasks_completed <= tasks_spawned);
		size_t tasks_running = tasks_spawned - trt_tasks_completed;
		if (tasks_running >= tasks_runing_max) {
			co_await trt::T::yield();
			continue;
		}

		trt::Task::List tl;
		for (size_t i = 0; i < tasks_batch; i++) {
			trt::Task *t = trt::T::alloc_task(t_worker, &trt_tasks_completed,
			                                  nullptr, true);
			tl.push_front(*t);
		}
		co_await trt::T::spawn_many(tl);

		tasks_spawned += tasks_batch;
		co_await trt::T::yield();
	}

	while (trt_tasks_completed != tasks_spawned)
		co_await trt::T::yield();

	co_return 0;
}

// main trt task
trt::CoroTask t_main(void *arg__) {

	xtimer_t t; timer_init(&t); timer_start(&t);
    co_await trt::T::spawn(t_spawner, nullptr, nullptr, false, trt::TaskType::POLL);
    co_await trt::T::task_wait();
	timer_pause(&t);
    double s = timer_secs(&t);
    printf("time=%lfs Mtasks/sec=%lf\n", s, TRT_MTASKS/s);
	trt::T::set_exit_all();
	trt_dmsg("%s: DONE\n", __FUNCTION__);
	co_return 0;
}

int main(int argc, char *argv[])
{
	trt::Controller c;
	c.spawn_scheduler(t_main, nullptr, trt::TaskType::TASK);
	c.wait_for_all();

	return 0;
}
