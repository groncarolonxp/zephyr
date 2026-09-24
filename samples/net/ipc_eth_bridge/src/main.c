/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Trampoline / bridge app for the host core (cpu0).
 *
 * This is the "simple trampoline" counterpart to running a stock networking
 * sample on the remote core: it does NOT run any socket application itself.
 * It only L2-bridges the IPC virtual Ethernet (vnet0, provided by the
 * zephyr,ipc-ethernet driver) with the on-board real Ethernet, so a stock
 * app on the remote core (e.g. samples/net/sockets/echo) can reach an
 * off-chip host (e.g. a PC) through this core's Ethernet port.
 *
 * All the "magic" (shared-memory ring, MBOX doorbell, handshake, carrier)
 * lives in the driver; this app is just the forwarding glue.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_bridge.h>
#include <zephyr/device.h>

/* The virtual interface is bound to the "zephyr,ipc-ethernet" DT node. */
#define VNET_NODE DT_NODELABEL(vnet0)

static struct net_if *bridge_iface;

static void bridge_find_cb(struct eth_bridge_iface_context *br, void *user_data)
{
	ARG_UNUSED(user_data);

	if (bridge_iface == NULL) {
		bridge_iface = br->iface;
	}
}

/* Return the real (non-virtual) Ethernet interface: not vnet0, not the
 * bridge interface itself.
 */
static struct net_if *find_real_eth(struct net_if *vnet0, struct net_if *bridge)
{
	STRUCT_SECTION_FOREACH(net_if, iface) {
		if (iface == vnet0 || iface == bridge) {
			continue;
		}
		if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
			continue;
		}
		return iface;
	}

	return NULL;
}

int main(void)
{
	const struct device *vdev = DEVICE_DT_GET(VNET_NODE);
	struct net_if *vnet0;
	struct net_if *eth;
	int ret;

	printk("IPC-ETH bridge (host core) starting\n");

	if (!device_is_ready(vdev)) {
		printk("BRIDGE: vnet0 device not ready\n");
		return 0;
	}

	vnet0 = net_if_lookup_by_dev(vdev);
	if (vnet0 == NULL) {
		printk("BRIDGE: no net_if bound to vnet0\n");
		return 0;
	}

	/* Wait until vnet0 is operational (peer handshake done). */
	if (!net_if_is_admin_up(vnet0)) {
		(void)net_if_up(vnet0);
	}
	for (int i = 0; i < 200 && !net_if_is_up(vnet0); i++) {
		if (!net_if_is_admin_up(vnet0)) {
			(void)net_if_up(vnet0);
		}
		k_sleep(K_MSEC(50));
	}

	net_eth_bridge_foreach(bridge_find_cb, NULL);
	if (bridge_iface == NULL) {
		printk("BRIDGE: no bridge interface available\n");
		return 0;
	}

	eth = find_real_eth(vnet0, bridge_iface);
	if (eth == NULL) {
		printk("BRIDGE: no real Ethernet interface found\n");
		return 0;
	}

	if (!net_if_is_admin_up(eth)) {
		(void)net_if_up(eth);
	}

	printk("BRIDGE: bridging vnet0 (iface %d) with eth (iface %d) via "
	       "bridge iface %d\n",
	       net_if_get_by_iface(vnet0), net_if_get_by_iface(eth),
	       net_if_get_by_iface(bridge_iface));

	ret = eth_bridge_iface_add(bridge_iface, vnet0);
	if (ret < 0) {
		printk("BRIDGE: add vnet0 failed (%d)\n", ret);
		return 0;
	}

	ret = eth_bridge_iface_add(bridge_iface, eth);
	if (ret < 0) {
		printk("BRIDGE: add eth failed (%d)\n", ret);
		return 0;
	}

	(void)net_if_up(bridge_iface);
	printk("BRIDGE: up - vnet0 <-> eth L2 forwarding active\n");

	return 0;
}
