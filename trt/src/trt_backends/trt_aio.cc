/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_aio.hh"

namespace trt {

thread_local AioState AioState__;

AioState *get_tls_AioState__() {
    return &AioState__;
}

int
AioOpBase::submit() {
    assert(aio_state_ == State::INITIALIZED);
    int ret = get_tls_AioState__()->submit(&aio_iocb_);
    if (ret == -1) {
        fprintf(stderr, "%s: submit I/O failed: %s (%d)\n",
                __PRETTY_FUNCTION__, strerror(errno), errno);
        aio_state_ = State::IO_ERROR;
        return -1;
    }
    T::io_npending_inc(1);
    aio_state_ = State::IO_SUBMITTED;
    return 0;
}

void AioOp::complete(RetT val) {
    aio_lsaobj_.set_val(val);
    TaskBase *waiter = aio_lsaobj_.take_waiter();
    if (waiter != nullptr)
        localScheduler__->wake_task(*waiter);
}

void AioOpCallback::complete(RetT val) {
    if (aio_callback_) {
        aio_callback_(this, val);
    } else {
        fprintf(stderr, "[%s +%d]: %s: no callback set\n",
                __FILE__, __LINE__, __FUNCTION__);
        abort();
    }
}

void
AioOpBase::fake_read(const void *src, size_t src_nbytes) {
    if (aio_state_ != State::INITIALIZED) {
        std::cerr << "performing read on an ioop that is not initialized\n";
        abort();
    }
    if (aio_iocb_.aio_lio_opcode != IOCB_CMD_PREAD) {
        std::cerr << "performing read on an opcode that is not read\n";
        abort();
    }

    size_t cp_nbytes = std::min(src_nbytes, (size_t)aio_iocb_.aio_nbytes);
    memcpy((void *)aio_iocb_.aio_buf, src, cp_nbytes);
    set_done();
    complete((RetT)cp_nbytes);
}

CoroTask
AIO::poller_task(void *unused) {
    struct timespec *ts = nullptr;
    const long min_events = 1;
    const long max_events = 8;
    struct io_event *events;

    events = (struct io_event *)malloc(sizeof(*events) * max_events);
    if (!events) {
        perror("malloc");
        exit(1);
    }

    trt_dmsg("aio poller starts\n");

    while (!AioState__.is_done()) {
        int nevents = AioState__.getevents(min_events, max_events, events, ts);
        if (nevents < 0) {
            perror("io_getevents");
            exit(1);
        } else if (nevents == 0) {
            co_await T::yield();
            continue;
        }

        for (int i = 0; i < nevents; i++) {
            struct io_event *ev = &events[i];
            AioOpBase *aio = (AioOpBase *)ev->data;

            RetT val;
            if (ev->res2 != 0) {
                assert(ev->res2 < 0);
                fprintf(stderr, "aio event error: %s\n", strerror(ev->res2));
                aio->set_error();
                val = (RetT)-1;
            } else {
                aio->set_done();
                val = (RetT)ev->res;
            }
            aio->complete(val); // directly pushes waiting task
        }
        T::io_npending_dec(nevents);

        co_await T::yield(); // yield to let woken tasks run
    }

    trt_dmsg("aio poller DONE\n");
    free(events);
    co_return 0;
}

} // end namespace trt
