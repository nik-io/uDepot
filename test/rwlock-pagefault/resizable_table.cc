/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 */

// The example test that src/include/uDepot/rwlock-pagefault.hh points at and
// that has never existed in this repo.
//
// The header says:
//
//     The code here implements (1) by mapping the table RO during the copy,
//     and installing SIGSEGV handlers that will rollback operations in case
//     of a page fault.
//     see test/rwlock-pagefault/resizable_table for an example
//
// `make run_tests` invoked that binary for years. There was no source, no
// build rule, and do_run_test swallowed the resulting command-not-found, so
// the component went uncovered while appearing tested. It is the component
// whose rollback the C++20 coroutine migration removed -- see
// docs/TODO-grow-race.md -- so the absence mattered.
//
// Two orderings are exercised, because uDepot has used both:
//
//   drain-first    write_enter, wait for readers, then mprotect. No reader can
//                  be inside a protected table, so no fault is possible. This
//                  is what uDepotDirectoryMap<RT>::grow() does today.
//
//   protect-first  write_enter, mprotect, copy, then wait. Readers *are*
//                  inside, so faults happen and the rollback has to catch
//                  them. This is the original design, and what the migration
//                  silently disabled by dropping rd_execute__.
//
// Not covered here, deliberately: protect-first *without* the rollback. That
// is the state uDepot's grow path is actually in, and the obvious way to show
// it is a forked child that should die. It does not die cleanly. With
// rwlpf_rb__.rb_set at 0, sigsegv_handler chains to oldact_g.sa_sigaction --
// and once this process has installed the handler, that previous disposition
// is the handler itself, so a fault recurses into it until the stack is gone.
// The child hangs instead of terminating, even under alarm(). Worth knowing
// when reading a crash report from the grow path: docs/TODO-grow-race.md.

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <thread>
#include <vector>

#include "uDepot/rwlock-pagefault.hh"

namespace {

using udepot::rwlock_pagefault;

// A table whose entries are their own index times a constant, so any reader
// can check an entry without coordinating with anyone.
constexpr unsigned long MAGIC = 0x9e3779b9UL;

struct Table {
	unsigned long *data = nullptr;
	size_t entries = 0;
	size_t bytes = 0;
};

Table table_alloc(size_t entries)
{
	Table t;
	t.entries = entries;
	t.bytes = entries * sizeof(unsigned long);
	t.data = (unsigned long *)mmap(nullptr, t.bytes, PROT_READ|PROT_WRITE,
	                               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (MAP_FAILED == t.data) {
		perror("mmap");
		exit(1);
	}
	for (size_t i = 0; i < entries; ++i)
		t.data[i] = i * MAGIC;
	return t;
}

std::atomic<Table *> current_g {nullptr};
std::atomic<bool>    stop_g {false};
// Set by the writer once the old table is unreadable. Readers in the
// protect-first run wait for it before touching the table, which turns "a
// fault might happen if the timing is right" into "a fault happens".
std::atomic<bool>    protect_done_g {false};
std::atomic<bool>    sync_readers_g {false};
std::atomic<size_t>  reads_g {0};
std::atomic<size_t>  rollbacks_g {0};
std::atomic<size_t>  bad_values_g {0};

// Read one entry of whatever table is current and check it. Deliberately
// re-reads the pointer every call: after a rollback the table may have been
// replaced, and a reader that cached it would be reading freed memory.
// Touches many entries rather than one. A single load leaves the reader
// inside the table for a few nanoseconds, which is far too short a window for
// a concurrent mprotect to land on -- an earlier version of this test did
// exactly that and reported zero rollbacks, proving nothing about the
// mechanism it was written to exercise. Scanning gives the writer a window
// wide enough to actually fault a reader.
constexpr size_t SCAN = 4096;

long read_once(size_t i)
{
	Table *t = current_g.load(std::memory_order_acquire);
	if (sync_readers_g.load(std::memory_order_relaxed)) {
		// Bounded: if the writer is not mid-resize this falls through
		// rather than hanging the test.
		for (unsigned spin = 0; spin < 100000; ++spin) {
			if (protect_done_g.load(std::memory_order_acquire))
				break;
			if (stop_g.load(std::memory_order_relaxed))
				break;
		}
	}
	for (size_t n = 0; n < SCAN; ++n) {
		const size_t idx = (i + n) % t->entries;
		const unsigned long got = t->data[idx];
		if (got != idx * MAGIC)
			bad_values_g.fetch_add(1, std::memory_order_relaxed);
	}
	reads_g.fetch_add(1, std::memory_order_relaxed);
	return 0;
}

void reader_plain(rwlock_pagefault *lock, size_t seed)
{
	for (size_t i = seed; !stop_g.load(std::memory_order_relaxed); ++i) {
		lock->rd_enter();
		read_once(i);
		lock->rd_exit();
	}
}

// The same reader, driven through rd_execute__ so a fault rolls back and the
// operation is retried rather than killing the process.
void reader_with_rollback(rwlock_pagefault *lock, size_t seed)
{
	for (size_t i = seed; !stop_g.load(std::memory_order_relaxed); ++i) {
		bool entered_twice = false;
		lock->rd_execute__(
			[lock]  { lock->rd_enter(); },
			[lock, &entered_twice] {
				entered_twice = true;
				rollbacks_g.fetch_add(1, std::memory_order_relaxed);
				lock->rd_exit();
			},
			[lock]  { lock->rd_exit();  },
			read_once, i);
		(void)entered_twice;
	}
}

// Grow the table, in one of the two orderings described at the top.
void resize(rwlock_pagefault *lock, bool drain_first)
{
	Table *old_table = current_g.load(std::memory_order_acquire);
	Table *new_table = new Table(table_alloc(old_table->entries * 2));

	protect_done_g.store(false, std::memory_order_release);
	lock->write_enter();
	if (drain_first)
		lock->write_wait_readers();

	if (0 != mprotect(old_table->data, old_table->bytes, PROT_READ)) {
		perror("mprotect RO");
		exit(1);
	}
	// The copy a real grow would do while readers are still allowed in.
	for (size_t i = 0; i < old_table->entries; ++i)
		new_table->data[i] = old_table->data[i];

	if (0 != mprotect(old_table->data, old_table->bytes, PROT_NONE)) {
		perror("mprotect NONE");
		exit(1);
	}
	protect_done_g.store(true, std::memory_order_release);

	if (!drain_first)
		lock->write_wait_readers();

	current_g.store(new_table, std::memory_order_release);
	munmap(old_table->data, old_table->bytes);
	delete old_table;
	protect_done_g.store(true, std::memory_order_release);
	lock->write_exit();
}

struct Result {
	size_t reads;
	size_t rollbacks;
	size_t bad_values;
};

Result run(bool drain_first, bool use_rollback, unsigned nthreads, unsigned resizes)
{
	rwlock_pagefault lock;
	if (0 != lock.init()) {
		fprintf(stderr, "rwlock init failed\n");
		exit(1);
	}

	current_g.store(new Table(table_alloc(1024)));
	// Only the protect-first run needs readers pinned inside the window; the
	// drain-first runs must be free to prove that no fault occurs at all.
	sync_readers_g.store(!drain_first);
	protect_done_g.store(true);
	stop_g.store(false);
	reads_g.store(0);
	rollbacks_g.store(0);
	bad_values_g.store(0);

	std::vector<std::thread> readers;
	for (unsigned t = 0; t < nthreads; ++t)
		readers.emplace_back(use_rollback ? reader_with_rollback : reader_plain,
		                     &lock, t * 7919);

	for (unsigned r = 0; r < resizes; ++r)
		resize(&lock, drain_first);

	stop_g.store(true);
	for (auto &t : readers)
		t.join();

	Result res { reads_g.load(), rollbacks_g.load(), bad_values_g.load() };

	Table *last = current_g.load();
	munmap(last->data, last->bytes);
	delete last;
	return res;
}

} // namespace

int main(int argc, char *argv[])
{
	const unsigned nthreads = 2 <= argc ? (unsigned)atoi(argv[1]) : 8;
	const unsigned resizes  = 3 <= argc ? (unsigned)atoi(argv[2]) : 6;
	int failures = 0;

	// 1. Drain before protecting. No reader can be inside a protected table,
	//    so this must be clean without any rollback machinery -- which is
	//    what makes it a safe ordering for callers that cannot roll back,
	//    such as a coroutine.
	{
		Result r = run(true, false, nthreads, resizes);
		printf("drain-first, no rollback:   reads=%zu rollbacks=%zu bad=%zu\n",
		       r.reads, r.rollbacks, r.bad_values);
		if (0 != r.bad_values) {
			fprintf(stderr, "FAIL: %zu wrong values read\n", r.bad_values);
			failures++;
		}
		if (0 != r.rollbacks) {
			fprintf(stderr, "FAIL: %zu rollbacks with no reader inside\n",
			        r.rollbacks);
			failures++;
		}
	}

	// 2. Protect while readers are inside, with rd_execute__ catching the
	//    faults. This is the mechanism the coroutine migration dropped. If it
	//    still works, restoring it for non-coroutine callers is viable; if it
	//    does not, that is worth knowing before anyone tries.
	{
		Result r = run(false, true, nthreads, resizes);
		printf("protect-first, rollback:    reads=%zu rollbacks=%zu bad=%zu\n",
		       r.reads, r.rollbacks, r.bad_values);
		if (0 != r.bad_values) {
			fprintf(stderr, "FAIL: %zu wrong values read\n", r.bad_values);
			failures++;
		}
		// Without this the test silently degrades to proving nothing: if no
		// reader is ever faulted, "no bad values" says only that the rollback
		// was never needed.
		if (0 == r.rollbacks) {
			fprintf(stderr, "FAIL: no rollback occurred, so the fault path "
			        "was never exercised\n");
			failures++;
		}
	}

	// 3. Drain-first again, this time through rd_execute__. Belt and braces:
	//    the wrapper must not break the ordering that needs no rollback.
	{
		Result r = run(true, true, nthreads, resizes);
		printf("drain-first, rollback:      reads=%zu rollbacks=%zu bad=%zu\n",
		       r.reads, r.rollbacks, r.bad_values);
		if (0 != r.bad_values) {
			fprintf(stderr, "FAIL: %zu wrong values read\n", r.bad_values);
			failures++;
		}
	}

	if (failures) {
		fprintf(stderr, "FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("OK\n");
	return 0;
}
