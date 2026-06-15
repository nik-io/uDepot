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

#ifndef	_UDEPOT_SALSA_STORE_H_
#define	_UDEPOT_SALSA_STORE_H_

#include "util/types.h"
namespace udepot {

// Header-only view of uDepotSalsaStore (no FAM): safe to use as a local
// variable inside C++20 coroutines where FAM members cannot appear in the
// middle of the generated coroutine frame struct.
struct uDepotSalsaStoreHeader {
	u16 key_size;		// bytes
	u32 val_size;		// bytes
	u64 timestamp;		// total order for crash recovery
}__attribute__((packed));

struct uDepotSalsaStore {
	u16 key_size;		// bytes
	u32 val_size;		// bytes
	u64 timestamp;		// total order for crash recovery
	char buf[];
}__attribute__((packed));

static_assert(sizeof(uDepotSalsaStoreHeader) == sizeof(uDepotSalsaStore),
              "uDepotSalsaStoreHeader must match fixed-field size of uDepotSalsaStore");

struct uDepotSalsaStoreSuffix {
	u16 crc16;
}__attribute__((packed));

};

#endif	// _UDEPOT_SALSA_STORE_H_
