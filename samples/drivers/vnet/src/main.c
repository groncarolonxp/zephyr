/*
 * Copyright (c) 2021 Carlo Caione <ccaione@baylibre.com>
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/*
 * Virtual network device demo (host side).
 *
 * The vnet0 interface is provided by the in-tree zephyr,ipc-ethernet driver
 * (drivers/ethernet/eth_ipc.c): UDP datagrams sent to the remote core's
 * address cross the shared-memory ring signalled by the MBOX doorbell.
 *
 * CONFIG_VNET_BRIDGE: instead of running the UDP demo, this core L2-bridges
 * vnet0 with the real on-board Ethernet so the remote core can reach an
 * off-chip host (e.g. a PC) through this core.
 */
#include "vnet_demo.h"

#define VNET_HOST_ADDR CONFIG_NET_CONFIG_MY_IPV4_ADDR
/* CONFIG_VNET_DEMO_PEER_ADDR may be a comma-separated list, e.g.
 * "192.0.2.1,192.0.2.10" to send to both the remote core (cpu1) and an
 * off-chip PC each cycle.
 */
#define VNET_PEER_ADDR CONFIG_VNET_DEMO_PEER_ADDR


int main(void)
{
	printk("Hello from HOST - %s\n", CONFIG_BOARD_TARGET);

#if defined(CONFIG_VNET_BRIDGE)
	/* Set up the vnet0 <-> Ethernet L2 bridge first so the remote core
	 * can reach an off-chip host. vnet_bridge_run() returns once the
	 * bridge is up, after which we ALSO run the UDP demo on this core so
	 * cpu0 is a full endpoint: it assigns 192.0.2.2 to vnet0, starts its
	 * own RX thread (to print CPU1->CPU0 and PC->CPU0 datagrams), and
	 * sends to its configured peers (CPU1 and the PC).
	 */
	vnet_bridge_run();

	/* vnet0 is now enslaved to the L2 bridge, so a socket bound to vnet0
	 * would never receive frames destined to this core. Bind the local
	 * UDP endpoint (IP + socket + RX) to the bridge interface instead so
	 * cpu0 can both send AND receive (CPU1->CPU0 and PC->CPU0).
	 */
	vnet_demo_set_iface(vnet_bridge_get_iface());
#endif

	vnet_demo_run(VNET_HOST_ADDR, VNET_PEER_ADDR, CONFIG_BOARD_TARGET);


	return 0;

}
