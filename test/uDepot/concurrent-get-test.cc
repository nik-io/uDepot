/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 */

// Reproducer for the concurrent get() assertion on the O_DIRECT backend.
//
// Drives the store exactly the way the pyudepot bindings do: a plain KV
// obtained from KV_factory, with get()/put() called via run_sync() from
// ordinary pthreads that never register with the runtime. See
// docs/concurrent-get-fix.md.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

#include "kv.hh"
#include "uDepot/kv-conf.hh"
#include "uDepot/kv-factory.hh"

namespace {

constexpr size_t KEY_NR      = 512;
constexpr size_t VAL_SIZE    = 4096;
constexpr size_t ITERATIONS  = 40;

std::string key_of(size_t i) { return "concurrent-get-key-" + std::to_string(i); }

void fill_val(char *buf, size_t i)
{
	memset(buf, (int)(i & 0xff), VAL_SIZE);
}

std::atomic<size_t> errors_g {0};

void reader(KV *kv, size_t tid, size_t nthreads)
{
	std::vector<char> val(VAL_SIZE);
	std::vector<char> expected(VAL_SIZE);

	for (size_t it = 0; it < ITERATIONS; ++it) {
		for (size_t i = tid; i < KEY_NR; i += nthreads) {
			const std::string k = key_of(i);
			size_t val_size_read = 0, val_size = 0;
			int rc = (int)kv->get(k.c_str(), k.size(), val.data(), val.size(),
			                      val_size_read, val_size).run_sync();
			if (0 != rc) {
				fprintf(stderr, "GET %s failed: %s\n", k.c_str(), strerror(rc));
				errors_g.fetch_add(1);
				continue;
			}
			fill_val(expected.data(), i);
			if (val_size != VAL_SIZE || val_size_read != VAL_SIZE ||
			    0 != memcmp(val.data(), expected.data(), VAL_SIZE)) {
				fprintf(stderr, "GET %s returned wrong data "
				        "(read=%zu size=%zu)\n", k.c_str(), val_size_read, val_size);
				errors_g.fetch_add(1);
			}
		}
	}
}

} // namespace

int main(int argc, char *argv[])
{
	const char *path = 2 <= argc ? argv[1] : "/tmp/udepot-concurrent-get-test";
	const size_t nthreads = 3 <= argc ? strtoul(argv[2], nullptr, 0)
	                                  : std::max(4u, std::thread::hardware_concurrency());
	const uint64_t store_size = 1UL << 30;

	udepot::KV_conf conf(path, store_size, 1 /* force destroy */, 4096 /* grain */,
	                     (1 << 19) /* segment */);
	conf.type_m = udepot::KV_conf::KV_UDEPOT_SALSA_O_DIRECT;

	KV *const kv = udepot::KV_factory::KV_new(conf);
	if (nullptr == kv) {
		fprintf(stderr, "failed to create uDepot instance\n");
		return 1;
	}
	int rc = kv->init();
	if (0 != rc) {
		fprintf(stderr, "failed to init uDepot: %s\n", strerror(rc));
		delete kv;
		return 1;
	}

	// Populate single-threaded -- the bug is on the read path.
	std::vector<char> val(VAL_SIZE);
	for (size_t i = 0; i < KEY_NR; ++i) {
		const std::string k = key_of(i);
		fill_val(val.data(), i);
		rc = (int)kv->put(k.c_str(), k.size(), val.data(), val.size()).run_sync();
		if (0 != rc) {
			fprintf(stderr, "PUT %s failed: %s\n", k.c_str(), strerror(rc));
			kv->shutdown();
			delete kv;
			return 1;
		}
	}

	printf("populated %zu keys, reading from %zu threads x %zu iterations\n",
	       KEY_NR, nthreads, ITERATIONS);

	std::vector<std::thread> threads;
	for (size_t t = 0; t < nthreads; ++t)
		threads.emplace_back(reader, kv, t, nthreads);
	for (auto &t : threads)
		t.join();

	kv->shutdown();
	delete kv;
	unlink(path);

	const size_t errors = errors_g.load();
	if (errors) {
		fprintf(stderr, "FAILED: %zu errors\n", errors);
		return 1;
	}
	printf("OK\n");
	return 0;
}
