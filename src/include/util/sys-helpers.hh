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

#ifndef SYS_HELPERS_HH_
#define SYS_HELPERS_HH_

#include <type_traits> // is_standard_layout
#include <cstring>
#include <sys/socket.h>
#include <cerrno>
#include <tuple>

// system helpers:
//  - recv_full() and send_full() that handle partial success of recv()/send()

template<typename T>
inline std::tuple<int, size_t>
recv_full(int fd, T *buff, size_t buff_size_bytes, int flags)
{
	static_assert(std::is_standard_layout<T>::value, "");
	void *b = static_cast<void *>(buff);
	return recv_full(fd, b, buff_size_bytes, flags);
}

// Deal with partially-succesful recv() calls
//
//   returns error (-1 or 0) and how many data were recv()ed
//
//   If an EOF is received before the whole data is read, this is considered an
//   error. -1 is returned and errno is set to ECONNRESET
template<>
inline std::tuple<int, size_t>
recv_full<void>(int fd, void *buff, size_t buff_size, int flags)
{
	ssize_t ret = 0;
	size_t data_read = 0;
	char *b = static_cast<char *>(buff);
	while (data_read != buff_size) {
		ret = ::recv(fd, b + data_read, buff_size - data_read, flags);
		if (ret < 0) break;
		if (ret == 0) { ret = -1; errno = ECONNRESET; break; }
		data_read += ret;
	}
	return std::make_tuple(ret < 0 ? -1 : 0, data_read);
}

template<typename T>
inline std::tuple<int, size_t>
send_full(int fd, T *buff, size_t buff_size_bytes, int flags)
{
	static_assert(std::is_standard_layout<T>::value, "");
	const void *b = static_cast<const void *>(buff);
	return send_full(fd, b, buff_size_bytes, flags);
}

// Deal with partially-succesful send() calls
//   returns error (-1 or 0) and how many data were sent
template<>
inline std::tuple<int, size_t>
send_full<const void>(int fd, const void *buff, size_t buff_size, int flags)
{
	ssize_t ret = 0;
	size_t data_sent = 0;
	const char *b = static_cast<const char *>(buff);
	while (data_sent != buff_size) {
		ret = ::send(fd, b + data_sent, buff_size - data_sent, flags);
		if (ret < 0) break;
		data_sent += ret;
	}
	return std::make_tuple(ret < 0 ? -1 : 0, data_sent);
}


#endif /* ifndef SYS_HELPERS_HH_ */
