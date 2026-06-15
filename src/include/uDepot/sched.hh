/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
 *           Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

#ifndef	_UDEPOT_SCHED_H_
#define	_UDEPOT_SCHED_H_

#include <coroutine>
#include <pthread.h>
#include "trt/uapi/trt.hh"

namespace udepot {

class PthreadSched {
public:
	// Returns std::suspend_never so co_await PthreadSched::yield() compiles
	// without actually suspending the coroutine (pthread_yield is synchronous).
	static inline std::suspend_never yield(void) { sched_yield(); return {}; }
};

class TrtSched {
public:
	static inline auto yield(void) { return trt::T::yield(); }
};

}


#endif /* _UDEPOT_SCHED_H_ */

