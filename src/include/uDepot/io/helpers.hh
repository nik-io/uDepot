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

#ifndef _UDEPOT_IO_HELPERS_
#define _UDEPOT_IO_HELPERS_

#include "trt/uapi/trt.hh"

namespace udepot {

// IO helper functions (coroutine versions)
//
// All functions return CoroTask and must be co_await'd from within TRT tasks.
// Callers receive the result (ssize_t cast to RetT) from the co_await expression.

#include "uDepot/mbuff.hh"
#include "uDepot/io.hh"
#include "util/debug.h"

static inline size_t
iovec_len(const struct iovec *iov, unsigned iovcnt)
{
	size_t ret = 0;
	for (unsigned i=0; i < iovcnt; i++) {
		ret += iov[i].iov_len;
	}
	return ret;
}

// Append data to mbuff by calling pread_native() on io.
//
// Returns bytes read (positive) or -error (negative) via co_return.
template<typename IO>
static inline
trt::CoroTask
io_pread_mbuff_append(IO &io, Mbuff &mb, size_t io_len, off_t io_off) {
	// eagerly check if there is enough size for the operation
	if (mb.append_avail_size() < io_len) {
		UDEPOT_ERR("mbuff: not enough free space for operation (get_free_size(): %zd, io_len: %zd)", mb.get_free_size(), io_len);
		co_return (trt::RetT)(ssize_t)(-ENOSPC);
	}

	// check that mbuff has (proper) native IO buffers
	assert(mb.ioptr_compatible<typename IO::Ptr>());

	// build an appropriate iovec for the operation
	const size_t iov_size = 16;
	struct iovec iov[iov_size];
	size_t remaining = io_len;
	size_t iov_cnt = 0;
	auto append_fn =
		[&remaining, &iov, &iov_size, &iov_cnt]
		(unsigned char *b, size_t len) -> size_t {
			if (iov_cnt == iov_size || remaining == 0)
				return 0;
			size_t min_len = std::min(remaining, len);
			iov[iov_cnt].iov_base = b;
			iov[iov_cnt].iov_len = min_len;
			iov_cnt++;
			remaining -= min_len;
			return min_len;
		};
	mb.append(std::ref(append_fn));
	size_t op_bytes = io_len - remaining;
	assert(iovec_len(iov, iov_cnt) == op_bytes);

	ssize_t io_ret;
	if (iov_cnt == 1) { // This is silly, but it seems to improve ubench perf a bit
		typename IO::Ptr io_ptr(iov[0].iov_base, iov[0].iov_len);
		io_ret = (ssize_t)(co_await io.pread_native(std::move(io_ptr), iov[0].iov_len, io_off));
	} else {
		IoVec<typename IO::Ptr> iov_ptr(iov, iov_cnt);
		io_ret = (ssize_t)(co_await io.preadv_native(iov_ptr, io_off));
	}

	// in case of an error or a partial read, invalidate the mbuff area that was
	// not written
	if (io_ret < 0) {
		mb.reslice(mb.get_valid_size() - op_bytes);
		io_ret = -errno;
	} else if (static_cast<size_t>(io_ret) < op_bytes) {
		mb.reslice(mb.get_valid_size() - op_bytes + io_ret);
	}

	co_return (trt::RetT)io_ret;
}

// Returns via output params: err (0 or errno), bytes_read.
// co_return 0 on success, co_return errno on error.
template<typename IO>
static inline
trt::CoroTask
io_pread_mbuff_append_full(IO &io, Mbuff &mb, size_t io_len, off_t io_off,
                           int *err_out, size_t *bytes_out) {
	size_t bytes_read = 0;
	int err = 0;

	if (io_len == 0) {
		UDEPOT_DBG("Request to read %zd bytes\n", io_len);
		*err_out = 0;
		*bytes_out = 0;
		co_return 0;
	}

	while (bytes_read < io_len) {
		const size_t op_len = io_len - bytes_read;
		const off_t  op_off = io_off + bytes_read;

		// Inlined io_pread_mbuff_append(): folding the per-chunk read into this
		// loop removes one coroutine layer (and its resume hop) from every KV
		// read. Backend-agnostic: still dispatches through io.{pread,preadv}_native,
		// so aio / io_uring / spdk are covered by their own IO::*_native impls.
		if (mb.append_avail_size() < op_len) {
			UDEPOT_ERR("mbuff: not enough free space (avail %zd, io_len %zd)", mb.append_avail_size(), op_len);
			err = ENOSPC;
			break;
		}
		assert(mb.ioptr_compatible<typename IO::Ptr>());
		const size_t iov_size = 16;
		struct iovec iov[iov_size];
		size_t remaining = op_len;
		size_t iov_cnt = 0;
		auto append_fn =
			[&remaining, &iov, &iov_size, &iov_cnt]
			(unsigned char *b, size_t len) -> size_t {
				if (iov_cnt == iov_size || remaining == 0)
					return 0;
				size_t min_len = std::min(remaining, len);
				iov[iov_cnt].iov_base = b;
				iov[iov_cnt].iov_len = min_len;
				iov_cnt++;
				remaining -= min_len;
				return min_len;
			};
		mb.append(std::ref(append_fn));
		const size_t op_bytes = op_len - remaining;
		assert(iovec_len(iov, iov_cnt) == op_bytes);

		ssize_t ret;
		if (iov_cnt == 1) {
			typename IO::Ptr io_ptr(iov[0].iov_base, iov[0].iov_len);
			ret = (ssize_t)(co_await io.pread_native(std::move(io_ptr), iov[0].iov_len, op_off));
		} else {
			IoVec<typename IO::Ptr> iov_ptr(iov, iov_cnt);
			ret = (ssize_t)(co_await io.preadv_native(iov_ptr, op_off));
		}
		if (ret < 0) {
			mb.reslice(mb.get_valid_size() - op_bytes);
			ret = -errno;
		} else if (static_cast<size_t>(ret) < op_bytes) {
			mb.reslice(mb.get_valid_size() - op_bytes + ret);
		}

		if (ret < 0) {
			err = -(static_cast<int>(ret));
			break;
		} else if (ret == 0) {
			err = ENODATA;
			break;
		}

		bytes_read += ret;
	}

	*err_out = err;
	*bytes_out = bytes_read;
	co_return (trt::RetT)(err ? err : 0);
}

// Write from mbuff: returns bytes written (positive) or -error via co_return.
template<typename IO>
static inline
trt::CoroTask
io_pwrite_mbuff(IO &io, size_t io_len, off_t io_off, Mbuff &mb, size_t mbuff_off,
                ssize_t *result_out) {
	size_t op_bytes;
	int iov_cnt;

	// build an appropriate iovec for the operation
	const size_t iov_size = 16;
	struct iovec iov[iov_size];
	std::tie(iov_cnt, op_bytes) = mb.fill_iovec_valid(mbuff_off, io_len, iov, iov_size);
	if (iov_cnt < 0) {
		UDEPOT_ERR("fill_iovec_valid failed with%s", strerror(-iov_cnt));
		*result_out = iov_cnt;
		co_return (trt::RetT)(ssize_t)iov_cnt;
	}
	assert(iovec_len(iov, iov_cnt) == op_bytes);
	IoVec<typename IO::Ptr> iov_ptr(iov, iov_cnt);
	ssize_t io_ret = (ssize_t)(co_await io.pwritev_native(iov_ptr, io_off));
	*result_out = io_ret;
	co_return (trt::RetT)io_ret;
}

// Returns via output params: err, bytes_written.
template<typename IO>
static inline
trt::CoroTask
io_pwrite_mbuff_full(IO &io, size_t io_len, off_t io_off, Mbuff &mb,
                     int *err_out, ssize_t *bytes_out, size_t mbuff_off = 0) {
	size_t bytes_written = 0;
	int err = 0;
	while (bytes_written < io_len) {
		size_t op_len = io_len - bytes_written;
		size_t op_off = io_off + bytes_written;
		ssize_t ret;
		co_await io_pwrite_mbuff(io, op_len, op_off, mb, mbuff_off + bytes_written, &ret);
		if (ret < 0) {
			err = -(static_cast<int>(ret));
			break;
		}
		assert(ret != 0);
		bytes_written += ret;
	}

	*err_out = err;
	*bytes_out = (ssize_t)bytes_written;
	co_return (trt::RetT)(err ? err : 0);
}

}

#endif /* ifndef _UDEPOT_IO_HELPERS_ */
