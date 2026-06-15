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

trt::CoroTask
waiter(void *arg) {
	trt::LocalSingleAsyncObj *ao = static_cast<trt::LocalSingleAsyncObj *>(arg);
	printf("%s: Calling wait\n", __PRETTY_FUNCTION__);
	trt::RetT ret = co_await trt::T::local_single_wait(ao);
	printf("%s: 0x%lx\n", __PRETTY_FUNCTION__, ret);
	co_return ret;
}

trt::CoroTask
notifier(void *arg) {
	trt::LocalSingleAsyncObj *ao = static_cast<trt::LocalSingleAsyncObj *>(arg);
	printf("%s: Calling notify\n", __PRETTY_FUNCTION__);
	trt::T::local_single_notify(ao, 0xbeed);
	co_return 0;
}

trt::CoroTask
main_task(void *arg) {
	{
		trt::LocalSingleAsyncObj ao;
		co_await trt::T::spawn(waiter, static_cast<void *>(&ao));
		co_await trt::T::spawn(notifier, static_cast<void *>(&ao));
		co_await trt::T::task_wait();
		co_await trt::T::task_wait();
	}
	printf("--\n");
	{
		trt::LocalSingleAsyncObj ao;
		co_await trt::T::spawn(notifier, static_cast<void *>(&ao));
		co_await trt::T::spawn(waiter, static_cast<void *>(&ao));
		co_await trt::T::task_wait();
		co_await trt::T::task_wait();
	}
	co_return 0;
}

int main(int argc, char *argv[])
{
	trt::Controller c;

	c.spawn_scheduler(main_task, NULL, trt::TaskType::TASK);
	return 0;
}
