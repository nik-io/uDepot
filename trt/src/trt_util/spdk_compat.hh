/*
 *  Copyright (c) 2020 International Business Machines
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 *  Compatibility shims for SPDK/DPDK API changes across versions.
 *  Supports building against SPDK v18.04 through v24.x.
 */

#ifndef SPDK_COMPAT_HH__
#define SPDK_COMPAT_HH__

#include <rte_version.h>

// DPDK 21.11 renamed RTE_LCORE_FOREACH_SLAVE to RTE_LCORE_FOREACH_WORKER
#ifndef RTE_LCORE_FOREACH_WORKER
#define RTE_LCORE_FOREACH_WORKER RTE_LCORE_FOREACH_SLAVE
#endif

#endif // SPDK_COMPAT_HH__
