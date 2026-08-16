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

#ifndef	_UDEPOT_SYNC_H_
#define	_UDEPOT_SYNC_H_

#include <pthread.h>

#include "trt/uapi/trt.hh"
#include "util/debug.h"

/**
 * Synchronization backend for uDepot
 *  - locks
 *  - lock tables
 *  - read-write pagefault locks
 */

namespace udepot {

// Abstract base class:
class uDepotLock {
public:
	virtual trt::CoroTask lock() = 0;
	// Non-suspending fast-path acquire: returns true iff the lock was taken
	// without contention. Callers use this to avoid allocating a lock()
	// coroutine frame on the common (uncontended) path, falling back to
	// co_await lock() only when it returns false. See uDepotMap::lock().
	virtual bool try_lock() = 0;
	virtual void lock_blocking() = 0;
	virtual void unlock() = 0;

        u32 ref_cnt() const { return ref_cnt_m.load(); }
        u32 ref_cnt_dec_ret(u32 val) { return ref_cnt_m.fetch_sub(val); }

        uDepotLock(u32 ref_cnt):ref_cnt_m(ref_cnt) {}
	virtual ~uDepotLock() {}
private:
        char pad_[64];
        std::atomic<u32> ref_cnt_m;
};

class PthreadLock : public uDepotLock {
protected:
	pthread_mutex_t lock_m;
public:
	PthreadLock():uDepotLock(1) { pthread_mutex_init(&lock_m, NULL); }
	~PthreadLock() { pthread_mutex_destroy(&lock_m); }
	PthreadLock(PthreadLock const&)   = delete;
	void operator=(PthreadLock const&) = delete;

	trt::CoroTask lock() override {
		int ret = pthread_mutex_lock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error taking lock");
			abort();
		}
		co_return 0;
	}

	bool try_lock() override { return pthread_mutex_trylock(&lock_m) == 0; }

	void lock_blocking() override {
		int ret = pthread_mutex_lock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error taking lock");
			abort();
		}
	}

	void unlock() override {
		int ret = pthread_mutex_unlock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error releasing lock");
			abort();
		}
	}
};

class PthreadSpinLock : public uDepotLock {
public:
	PthreadSpinLock():uDepotLock(1) { pthread_spin_init(&lock_m, PTHREAD_PROCESS_SHARED); }
	~PthreadSpinLock() { pthread_spin_destroy(&lock_m); }
	PthreadSpinLock(PthreadSpinLock const&)   = delete;
	void operator=(PthreadSpinLock const&) = delete;

	trt::CoroTask lock() override {
		pthread_spin_lock(&lock_m);
		co_return 0;
	}

	bool try_lock() override { return pthread_spin_trylock(&lock_m) == 0; }

	void lock_blocking() override {
		pthread_spin_lock(&lock_m);
	}

	void unlock() override {
		const int ret = pthread_spin_unlock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error releasing lock");
			abort();
		}
	}
protected:
	pthread_spinlock_t lock_m;
};

// NB: we can probably do better than the following implementation, but this
// should work for now.
class TrtLock : public uDepotLock {
	pthread_mutex_t lock_m;
public:
	TrtLock():uDepotLock(1) { pthread_mutex_init(&lock_m, NULL); }
	TrtLock(TrtLock const&)    = delete;
	void operator=(TrtLock const&) = delete;

	trt::CoroTask lock() override {
		for (;;) {
			int ret = pthread_mutex_trylock(&lock_m);
			switch (ret) {
				case 0:
				co_return 0;

				case EBUSY:
				co_await trt::T::yield();
				break;

				default:
				UDEPOT_ERR("Error taking lock");
				abort();
			}
		}
	}

	bool try_lock() override { return pthread_mutex_trylock(&lock_m) == 0; }

	void lock_blocking() override {
		for (;;) {
			int ret = pthread_mutex_trylock(&lock_m);
			if (ret == 0) return;
			if (ret != EBUSY) { UDEPOT_ERR("Error taking lock"); abort(); }
			sched_yield();
		}
	}

	void unlock() override {
		int ret = pthread_mutex_unlock(&lock_m);
		if (ret != 0) {
			UDEPOT_ERR("Error releasing lock");
			abort();
		}
	}
};

} // end udepot namespace

#endif /* _UDEPOT_SYNC_H_ */
