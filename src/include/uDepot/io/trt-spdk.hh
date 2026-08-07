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

#ifndef	_UDEPOT_TRT_SPDK_IO_H_
#define	_UDEPOT_TRT_SPDK_IO_H_

#include "uDepot/io.hh"
#include "uDepot/io/spdk-common.hh"

#include "trt_backends/trt_spdk.hh"
#include <errno.h>
#include <iostream>
#include <mutex>
#include <map>
#include <sys/mman.h>

/**
 * TRT SPDK IO backend for uDepot
 */


#include <string>

namespace udepot {

// simple wrapper over trt::SPDK (at least for now)
class TrtSpdkIO final : public uDepotIO_, public RteAlloc<512> {

	static SpdkQpair *getThreadQP();

public:
	// Register an NVMe-over-Fabrics target to probe in addition to any local
	// PCIe NVMe devices. Must be called before global_init(). This is what
	// lets SPDK be exercised on a machine with no NVMe hardware, by pointing
	// at a loopback nvmf target.
	enum class NvmefTransport { TCP, RDMA };
	static void add_nvmef_target(NvmefTransport transport,
	                             const std::string &traddr,
	                             const std::string &trsvcid,
	                             const std::string &subnqn);

	static void global_init();
	static void thread_init();
	static void thread_exit();

	// NB: With SPDK, there is no concept of a pathname. Instead, the string
	// argument identifies a device namespace. If the string is empty, the first
	// available namespace is used.
	//
	// @flags and @mode arguments are currently ignored
	int open(const char *pathname, int flags, mode_t mode) override;
	int close() override;

	// no-ops
	void prefetch(size_t len, off_t off) override { }
	int unlink(const char *pathname) override { return 0; };

	// not supported, will return an error
	int ftruncate(off_t len) override {
		errno = ENOTSUP;
		return -1;
	}

	// Native interface — coroutine-based, co_await from TRT tasks.
	using Ptr = RteAlloc<512>::Ptr;
	trt::CoroTask pread_native(Ptr buff, size_t len, off_t off);
	trt::CoroTask pwrite_native(Ptr buff, size_t len, off_t off);
	trt::CoroTask preadv_native(IoVec<Ptr>  iov, off_t off);
	trt::CoroTask pwritev_native(IoVec<Ptr> iov, off_t off);

	u64 get_size() override {
		return getThreadQP()->get_size();
	}

	ssize_t pread(void *buff, size_t len, off_t off) override {
		return getThreadQP()->read_sync(buff, len, off);
	}

	ssize_t preadv(const struct iovec *iov, int iovcnt, off_t off) override {
		ssize_t tot = 0;
		for (int i = 0; i < iovcnt; i++) {
			ssize_t r = getThreadQP()->read_sync(iov[i].iov_base, iov[i].iov_len, off + tot);
			if (r == -1) return -1;
			tot += r;
			if (static_cast<size_t>(r) != iov[i].iov_len) break;
		}
		return tot;
	}

	ssize_t pwrite(const void *buff, size_t len, off_t off) override {
		return getThreadQP()->write_sync(buff, len, off);
	}

	ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t off) override {
		ssize_t tot = 0;
		for (int i = 0; i < iovcnt; i++) {
			ssize_t r = getThreadQP()->write_sync(iov[i].iov_base, iov[i].iov_len, off + tot);
			if (r == -1) return -1;
			tot += r;
			if (static_cast<size_t>(r) != iov[i].iov_len) break;
		}
		return tot;
	}

	void *mmap(void *addr, size_t len, int prot, int flags, off_t off) override;
	int msync(void *addr, size_t len, int flags);
	int munmap(void *addr, size_t len);

private:
	std::map<void *, mregion> mmap_off_m;
	std::mutex mtx_m;
};

} // end udepot namespace

#endif /* _UDEPOT_TRT_SPDK_IO_H_ */
