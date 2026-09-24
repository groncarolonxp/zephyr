/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared-block layout for the IPC (shared-memory + MBOX doorbell) virtual
 * Ethernet driver.
 *
 * The actual ring/FIFO is provided by the Zephyr IPC-service "packed buffer"
 * (struct pbuf, <zephyr/ipc/pbuf.h>, CONFIG_PBUF): a supported, cache- and
 * barrier-aware single-producer / single-consumer length-framed FIFO. This
 * driver only has to carve the shared block into two unidirectional pbuf
 * regions (one per direction) and let pbuf own all of the index management
 * and cache maintenance, so this header deliberately defines no head/tail
 * layout and no cache-line padding of its own.
 *
 * Shared block layout (host "a" / remote "b"):
 *
 *   +--------------------------+ reg base
 *   |  a2b pbuf region         |  host -> remote (host is writer)
 *   |  (holds the handshake    |
 *   |   word used for connect) |
 *   +--------------------------+ reg base + region_size
 *   |  b2a pbuf region         |  remote -> host (remote is writer)
 *   +--------------------------+ reg base + 2 * region_size
 */

#ifndef ETH_IPC_PRIV_H
#define ETH_IPC_PRIV_H

#include <stdint.h>

#include <zephyr/toolchain.h>

/* Maximum single frame we carry (standard Ethernet MTU + L2 header + slack). */
#define ETH_IPC_FRAME_MAX   1536U

/*
 * Cache line alignment used for the shared pbuf regions. pbuf consumes this to
 * place its read/write indices on their own cache lines and to size its cache
 * maintenance, which is what keeps the two cores from false-sharing a line and
 * clobbering each other's index.
 *
 * This must be at least the real data cache line size of the target cores: a
 * smaller value silently removes that protection. Over-estimating only costs
 * a little shared memory, so round up when in doubt.
 */
#define ETH_IPC_DCACHE_ALIGN  CONFIG_ETH_IPC_DCACHE_ALIGN

BUILD_ASSERT(ETH_IPC_DCACHE_ALIGN >= sizeof(uint32_t),
	     "CONFIG_ETH_IPC_DCACHE_ALIGN must be at least sizeof(uint32_t)");
BUILD_ASSERT((ETH_IPC_DCACHE_ALIGN & (ETH_IPC_DCACHE_ALIGN - 1)) == 0,
	     "CONFIG_ETH_IPC_DCACHE_ALIGN must be a power of two");
#if defined(CONFIG_DCACHE_LINE_SIZE) && CONFIG_DCACHE_LINE_SIZE > 0
BUILD_ASSERT(ETH_IPC_DCACHE_ALIGN >= CONFIG_DCACHE_LINE_SIZE,
	     "CONFIG_ETH_IPC_DCACHE_ALIGN is smaller than the CPU cache line; "
	     "the two cores would false-share a line and corrupt the indices");
#endif

/*
 * Handshake word value exchanged through pbuf's handshake slot during the
 * connect sequence. The host publishes ETH_IPC_HS_READY once it has
 * initialised its TX pbuf; the remote spins until it observes it.
 */
#define ETH_IPC_HS_READY   0x564E4554U /* "VNET" */

#endif /* ETH_IPC_PRIV_H */
