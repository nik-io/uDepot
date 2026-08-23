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

#include <cstdlib> // getenv

#include "uDepot/io/trt-spdk.hh"
#include "trt_backends/trt_spdk.hh"
#include "util/debug.h"

namespace udepot {

static SpdkGlobalState                         SpdkGlobalState__;
static std::string                             SpdkNamespaceStr__;
static thread_local bool                       SpdkThreadInitialized__ = false;
static thread_local std::shared_ptr<SpdkQpair> SpdkThreadQP__;


void TrtSpdkIO::add_nvmef_target(NvmefTransport transport,
                                 const std::string &traddr,
                                 const std::string &trsvcid,
                                 const std::string &subnqn) {
	NvmefTarget t;
	t.transport = (transport == NvmefTransport::RDMA) ? NvmefTarget::RDMA
	                                                  : NvmefTarget::TCP;
	t.traddr  = traddr;
	t.trsvcid = trsvcid;
	t.subnqn  = subnqn;
	SpdkGlobalState__.add_nvmef_target(std::move(t));
}

// Register any NVMe-oF targets named in the UDEPOT_NVMEF environment variable
// before controllers are probed. This is what lets the SPDK backend run against
// a software fabrics target (e.g. a loopback nvmf_tgt in CI) with no NVMe
// hardware, without any command-line change to the drivers (udepot-test etc.).
//
// Format: one or more TCP targets, ';'-separated, each traddr:trsvcid:subnqn.
// The subnqn itself contains ':' (e.g. nqn.2016-06.io.spdk:cnode1), so each
// entry is split on its first two ':' only and the remainder is the nqn.
//   UDEPOT_NVMEF=127.0.0.1:4420:nqn.2016-06.io.spdk:cnode1
static void add_env_nvmef_targets(void) {
	const char *env = getenv("UDEPOT_NVMEF");
	if (env == nullptr || env[0] == '\0')
		return;

	const std::string spec(env);
	size_t pos = 0;
	while (pos < spec.size()) {
		size_t sep = spec.find(';', pos);
		const std::string entry =
			spec.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
		pos = (sep == std::string::npos) ? spec.size() : sep + 1;
		if (entry.empty())
			continue;

		const size_t c1 = entry.find(':');
		const size_t c2 = entry.find(':', c1 + 1);
		if (c1 == std::string::npos || c2 == std::string::npos) {
			UDEPOT_ERR("UDEPOT_NVMEF entry '%s' is not traddr:trsvcid:subnqn; ignoring\n",
			           entry.c_str());
			continue;
		}
		const std::string traddr  = entry.substr(0, c1);
		const std::string trsvcid = entry.substr(c1 + 1, c2 - c1 - 1);
		const std::string subnqn  = entry.substr(c2 + 1);
		UDEPOT_MSG("UDEPOT_NVMEF: adding TCP target %s:%s (%s)\n",
		           traddr.c_str(), trsvcid.c_str(), subnqn.c_str());
		TrtSpdkIO::add_nvmef_target(TrtSpdkIO::NvmefTransport::TCP,
		                            traddr, trsvcid, subnqn);
	}
}

void TrtSpdkIO::global_init(void) {
	add_env_nvmef_targets();
	SpdkGlobalState__.init();
}

void TrtSpdkIO::thread_init(void) {
	assert(SpdkThreadInitialized__ == false);
	trt_dmsg("Initializing SPDK\n");
	trt::SPDK::init(SpdkGlobalState__);
	trt_dmsg("Spawining SPDK poller\n");
	// thread_init() is a plain function, not a coroutine: T::spawn() returns a
	// SpawnAwaitable that only enqueues the task when co_awaited, so calling it
	// here silently dropped the poller and it was never scheduled -- I/O
	// submitted on the qpair then never had its completions reaped and the
	// store hung right after init. spawn_detached_no_wait() enqueues without
	// suspending and is the API the AIO/io_uring pollers already use from their
	// own thread_init().
	trt::T::spawn_detached_no_wait(trt::SPDK::poller_task, nullptr, trt::TaskType::TASK);
	trt_dmsg("init done\n");
	SpdkThreadInitialized__ = true;
}

void TrtSpdkIO::thread_exit(void) {
	trt::SPDK::stop();
}

// set the global string of which namespace to use.
static int
set_SpdkNamespaceStr(std::string ns_name) {
	assert(SpdkNamespaceStr__.empty());
	std::vector<std::string> namespaces = trt::SPDK::getNamespaceNames();
	if (namespaces.begin() == namespaces.end()) {
		trt_err("No SPDK namespaces found\n");
		errno = EINVAL;
		return -1;
	}

	auto match_ns = [&ns_name] (std::string const&ns) -> bool {
		return ns.find(ns_name) != std::string::npos;
	};

	auto ns_found = std::find_if(namespaces.begin(), namespaces.end(), match_ns);
	if (ns_found != namespaces.end()) {
		SpdkNamespaceStr__ = *ns_found;
		trt_msg("Found a match for user-provided string namespace: %s. Using: %s\n", ns_name.c_str(), SpdkNamespaceStr__.c_str());
	} else {
		// sort namespaces so we get some consistency across executions
		std::sort(namespaces.begin(), namespaces.end());
		SpdkNamespaceStr__ = *namespaces.begin();
		trt_msg("User-provided string namespace %s not found. Using: %s\n", ns_name.c_str(), SpdkNamespaceStr__.c_str());
	}

	return 0;
}

static int
setThreadQP(void) {
	assert(SpdkThreadInitialized__);
	assert(SpdkThreadQP__ == nullptr);
	SpdkThreadQP__ = trt::SPDK::getQpair(SpdkNamespaceStr__);
	if (SpdkThreadQP__ != nullptr)
		return 0;
	trt_err("Unable to get an SPDK queue pair for namespace \"%s\"\n", SpdkNamespaceStr__.c_str());
	errno = EIO;
	return -1;
}

static void setThreadQP_if_needed(void) {
	if (SpdkThreadQP__ != nullptr)
		return;
	int err = setThreadQP();
	if (err) {
		UDEPOT_ERR("setThreadQP failed");
		abort();
	}
}

int TrtSpdkIO::open(const char *pathname, int flags, mode_t mode) {
	int ret;

	assert(SpdkThreadInitialized__);
	if (SpdkNamespaceStr__.size() > 0) {
		UDEPOT_ERR("%s: already opened: %s\n", __PRETTY_FUNCTION__, SpdkNamespaceStr__.c_str());
		abort();
	}

	ret = set_SpdkNamespaceStr(std::string(pathname));
	if (ret < 0)
		return ret;
	// initialize this threads' queue pair, just to get an early error if
	// something goes wrong.
	return setThreadQP();
}

int TrtSpdkIO::close() {
	if (SpdkThreadQP__ == nullptr)
		return EIO;
	SpdkThreadQP__ = nullptr;
	return 0;
}

SpdkQpair *TrtSpdkIO::getThreadQP(void) {
	setThreadQP_if_needed();
	return SpdkThreadQP__.get();
}

trt::CoroTask
TrtSpdkIO::pread_native(Ptr buff, size_t len, off_t off) {
	SpdkQpair *qp = getThreadQP();
	size_t bsize = qp->get_sector_size();

	if (len % bsize != 0) {
		UDEPOT_ERR("len (%zd) not aligned to block size (%zd)", len, bsize);
		co_return (trt::RetT)-EINVAL;
	}
	if (off % bsize != 0) {
		UDEPOT_ERR("offset (%zd) not aligned to block size (%zd)", off, bsize);
		co_return (trt::RetT)-EINVAL;
	}

	uint64_t lba_start = off / bsize;
	uint64_t nlbas = len / bsize;

	ssize_t ret = (ssize_t)(co_await trt::SPDK::read(qp, buff, lba_start, nlbas));
	if (ret == -1) {
		errno = EIO;
		co_return (trt::RetT)-1;
	}

	co_return (trt::RetT)(ret * (ssize_t)bsize);
}

trt::CoroTask
TrtSpdkIO::pwrite_native(Ptr buff, size_t len, off_t off) {
	SpdkQpair *qp = getThreadQP();
	size_t bsize = qp->get_sector_size();

	if (len % bsize != 0) {
		UDEPOT_ERR("len (%zd) not aligned to block size (%zd)", len, bsize);
		co_return (trt::RetT)-EINVAL;
	}
	if (off % bsize != 0) {
		UDEPOT_ERR("offset (%zd) not aligned to block size (%zd)", off, bsize);
		co_return (trt::RetT)-EINVAL;
	}

	uint64_t lba_start = off / bsize;
	uint64_t nlbas = len / bsize;

	ssize_t ret = (ssize_t)(co_await trt::SPDK::write(qp, buff, lba_start, nlbas));
	if (ret == -1) {
		errno = EIO;
		co_return (trt::RetT)-1;
	}

	co_return (trt::RetT)(ret * (ssize_t)bsize);
}

trt::CoroTask
TrtSpdkIO::preadv_native(IoVec<Ptr> iov_, off_t off) {
	ssize_t tot = 0;
	int iovcnt = iov_.iov_cnt_m;
	struct iovec *iov = iov_.iov_m;

	for (int i=0; i < iovcnt; i++) {
		void *iov_base = iov[i].iov_base;
		size_t iov_len = iov[i].iov_len;
		auto ptr = Ptr(iov_base, iov_len);
		ssize_t ret = (ssize_t)(co_await trt::SPDK::read(
			getThreadQP(), ptr,
			(off + tot) / getThreadQP()->get_sector_size(),
			iov_len / getThreadQP()->get_sector_size()));
		if (ret == -1)
			co_return (trt::RetT)-1;
		ssize_t bytes = ret * (ssize_t)getThreadQP()->get_sector_size();
		tot += bytes;
		if (static_cast<size_t>(bytes) != iov_len)
			break;
	}

	co_return (trt::RetT)tot;
}

trt::CoroTask
TrtSpdkIO::pwritev_native(IoVec<Ptr> iov_, off_t off) {
	ssize_t tot = 0;
	int iovcnt = iov_.iov_cnt_m;
	struct iovec *iov = iov_.iov_m;

	for (int i=0; i < iovcnt; i++) {
		void *iov_base = iov[i].iov_base;
		size_t iov_len = iov[i].iov_len;
		auto ptr = Ptr(iov_base, iov_len);
		ssize_t ret = (ssize_t)(co_await trt::SPDK::write(
			getThreadQP(), ptr,
			(off + tot) / getThreadQP()->get_sector_size(),
			iov_len / getThreadQP()->get_sector_size()));
		if (ret == -1)
			co_return (trt::RetT)-1;
		ssize_t bytes = ret * (ssize_t)getThreadQP()->get_sector_size();
		tot += bytes;
		if (static_cast<size_t>(bytes) != iov_len)
			break;
	}

	co_return (trt::RetT)tot;
}

void *
TrtSpdkIO::mmap(void *addr, size_t len, int prot, int flags, off_t off) {
	std::cerr << "---------------------------------------------------------"
	          << std::endl
	          << "Doing anonymous mmap(). Data will be flushed to the drive at shutdown."
	          << std::endl
	          << "---------------------------------------------------------"
	          << std::endl;
	void *const ptr = ::mmap(addr, len, prot|PROT_WRITE, flags | MAP_ANONYMOUS, -1, off);
	if (MAP_FAILED == ptr)
		return ptr;
	ssize_t rc = 0, tot = 0;
	for (size_t i = 0; i < len; tot += rc) {
		const size_t l = std::min((len - i), (size_t) 8192U);
		rc = pread((void *) ((char *) ptr + i), l, off + i);
		if ((ssize_t) l != rc)
			break;
		i += l;
	}
	if ((ssize_t) len != tot) {
		const int rc2 __attribute__((unused)) = ::munmap(ptr, len);
		assert(0 == rc2);
		errno = EIO;
		return MAP_FAILED;
	}

       mtx_m.lock();
       mmap_off_m.emplace(ptr, (mregion)  { off, len});
       mtx_m.unlock();
	return ptr;
}

int
TrtSpdkIO::msync(void *addr, size_t len, int flags) {
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator it = mmap_off_m.begin();
	mtx_m.lock();
	for (; it != mmap_off_m.end(); ++it) {
		const uintptr_t start = (uintptr_t) it->first;
		const uintptr_t end = start + it->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	mtx_m.unlock();
	if (it == mmap_off_m.end()) {
		errno = EINVAL;
		return -1;
	}

	ssize_t rc = 0, tot = 0;
	const off_t off = it->second.off;
	for (size_t i = 0; i < len; tot += rc) {
		const size_t l = std::min((len - i), (size_t) 8192U);
		rc = pwrite((void *) ((char *) addr + i), l, off + i);
		if ((ssize_t) l != rc)
			return rc;
		i += l;
	}
	return tot == (ssize_t) len ? 0 : ({errno = EIO; -1;});
}

int
TrtSpdkIO::munmap(void *addr, size_t len) {
	const uintptr_t loc = (uintptr_t)addr;
	std::map<void *, mregion>::iterator it = mmap_off_m.begin();
	mtx_m.lock();
	for (; it != mmap_off_m.end(); ++it) {
		const uintptr_t start = (uintptr_t) it->first;
		const uintptr_t end = start + it->second.len;
		if (start <= loc && loc < end) {
			break;
		}
	}
	if (it == mmap_off_m.end()) {
		mtx_m.unlock();
		errno = EINVAL;
		return -1;
	}
	mmap_off_m.erase(it);
	mtx_m.unlock();
	return ::munmap(addr, len);
}

} // end namespace udepot
