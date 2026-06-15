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

#ifndef TRT_COMMON_HH_
#define TRT_COMMON_HH_

// RetT and TaskType are defined in task_base.hh (included transitively).
// TaskFnArg and TaskFn (CoroTask-based) are defined in task.hh.
// This header is kept for backward compat; include task_base.hh directly for RetT.
#include "trt/task_base.hh"

#endif // TRT_COMMON_HH_
