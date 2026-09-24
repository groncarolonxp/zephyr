/*
 * Copyright (c) 2021 Carlo Caione <ccaione@baylibre.com>
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * Virtual network device demo (remote side).
 *
 * The vnet0 interface is provided by the in-tree zephyr,ipc-ethernet driver
 * (drivers/ethernet/eth_ipc.c): UDP datagrams sent to the CM33 host's
 * address cross the shared-memory ring signalled by the MBOX doorbell.
 */
#include "vnet_demo.h"

#define VNET_SELF_ADDR CONFIG_NET_CONFIG_MY_IPV4_ADDR
/* Defaults to the host (192.0.2.2); override CONFIG_VNET_DEMO_PEER_ADDR to
 * target an off-chip host (e.g. a PC at 192.0.2.10) reachable through the
 * host core's vnet0<->Ethernet bridge. This may be a comma-separated list,
 * e.g. "192.0.2.2,192.0.2.10" to send to both the host core and the PC.
 */
#define VNET_PEER_ADDR CONFIG_VNET_DEMO_PEER_ADDR


int main(void)
{
	printk("Hello from REMOTE - %s\n", CONFIG_BOARD_TARGET);

	vnet_demo_run(VNET_SELF_ADDR, VNET_PEER_ADDR, CONFIG_BOARD_TARGET);



	return 0;
}
