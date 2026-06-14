/*
 *  Copyright (c) 2020,2022 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
 *           Nikolas Ioannou (nio@zurich.ibm.com, nicioan@gmail.com)
 *
 */

// vim: set expandtab softtabstop=4 tabstop:4 shiftwidth:4:

#include "trt/uapi/trt.hh"
#include "trt_backends/trt_uring.hh"

namespace trt {

thread_local UringState IouState__;

UringState *get_tls_IouState__() {
    return &IouState__;
}

int
IouOp::submit() {
    assert(state_ == State::INITIALIZED);
    UringState *uring_state = get_tls_IouState__();
    struct io_uring_sqe *sqe = uring_state->get_sqe();
    if (nullptr == sqe) {
        fprintf(stderr, "%s: submit I/O failed: %s (%d)\n", __PRETTY_FUNCTION__, strerror(errno), errno);
        state_ = State::IO_ERROR;
        return -1;
    }

    if (op_code_ == IouOpCode::PWRITEV) {
        io_uring_prep_writev(sqe, fd_, iovec_, iovec_cnt_, offset_);
    } else {
        io_uring_prep_readv(sqe, fd_, iovec_, iovec_cnt_, offset_);
    }
    io_uring_sqe_set_data(sqe, this);
    int ret = uring_state->submit(sqe);
    if (ret == -1) {
        fprintf(stderr, "%s: submit I/O failed: %s (%d)\n", __PRETTY_FUNCTION__, strerror(errno), errno);
        state_ = State::IO_ERROR;
        return -1;
    }

    T::io_npending_inc(1);
    state_ = State::IO_SUBMITTED;
    return 0;
}

// Called by the poller when IO completes: directly pushes the waiting task.
void IouOp::complete(RetT val) {
    iou_lsaobj_.set_val(val);
    TaskBase *waiter = iou_lsaobj_.take_waiter();
    if (waiter != nullptr)
        localScheduler__->wake_task(*waiter);
}

// For cache-hit scenarios: fake a read by copying from src into the iov buffers.
// state_ must be INITIALIZED. complete() is called directly (no batch needed).
void
IouOp::fake_read(const void *src, size_t src_nbytes) {
    if (state_ != State::INITIALIZED) {
        std::cerr << "performing read on an ioop that is not initialized\n";
        abort();
    }
    if (op_code_ != IouOp::IouOpCode::PREADV) {
        std::cerr << "performing read on an opcode that is not read\n";
        abort();
    }
    set_done();
    complete((RetT)src_nbytes);
}

CoroTask
IOU::poller_task(void *unused) {
    const long max_cqes = UringState::iou_maxio_ / 2;
    struct io_uring_cqe **cqes_batch;

    cqes_batch = (io_uring_cqe **)malloc(sizeof(*cqes_batch) * max_cqes);
    if (!cqes_batch) {
        perror("malloc");
        exit(1);
    }

    trt_dmsg("iou poller starts\n");

    while (!IouState__.is_done() || T::io_npending_get()) {
        int cqe_nr = IouState__.peek_batch_cqe(max_cqes, cqes_batch);
        if (cqe_nr < 0) {
            perror("io_uring_peek_batch_cqe");
            exit(1);
        } else if (cqe_nr == 0) {
            co_await T::yield();
            continue;
        }

        for (int i = 0; i < cqe_nr; i++) {
            struct io_uring_cqe *cqe = cqes_batch[i];
            IouOp *iou_op = static_cast<IouOp *>(io_uring_cqe_get_data(cqe));

            RetT val;
            if (cqe->res > 0) {
                iou_op->set_done();
                val = (RetT)cqe->res;
            } else {
                fprintf(stderr, "iou event error: %s\n", strerror(-cqe->res));
                iou_op->set_error();
                val = (RetT)cqe->res;
            }

            iou_op->complete(val);
            IouState__.cqe_seen(cqe);
        }
        T::io_npending_dec(cqe_nr);

        co_await T::yield();
    }

    trt_dmsg("iou poller DONE\n");
    free(cqes_batch);
    co_return 0;
}

} // end namespace trt
