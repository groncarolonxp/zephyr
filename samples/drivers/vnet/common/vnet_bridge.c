/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side L2 bridge between the virtual interface (vnet0) and the real
 * on-board Ethernet interface. With this, the remote core (which only has
 * vnet0) can reach an off-chip host: its frames cross the shared-memory ring
 * to the host core, and the bridge forwards them out of the real Ethernet
 * port (and forwards replies back the same way).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_bridge.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "vnet_demo.h"

/* Located while iterating the registered bridges. */
static struct net_if *vnet_bridge_iface;

static void vnet_bridge_find_cb(struct eth_bridge_iface_context *br,
				void *user_data)
{
	ARG_UNUSED(user_data);

	if (vnet_bridge_iface == NULL) {
		vnet_bridge_iface = br->iface;
	}
}

/* Return the real (non-virtual) Ethernet interface, i.e. not vnet0 and not
 * the bridge interface itself.
 */
static struct net_if *vnet_bridge_find_eth(struct net_if *vnet0,
					   struct net_if *bridge)
{
	struct net_if *eth = NULL;

	STRUCT_SECTION_FOREACH(net_if, iface) {
		if (iface == vnet0 || iface == bridge) {
			continue;
		}
		if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
			continue;
		}
		eth = iface;
		break;
	}

	return eth;
}

void vnet_bridge_run(void)
{
	const struct device *vdev = DEVICE_DT_GET(DT_NODELABEL(vnet0));
	struct net_if *vnet0;
	struct net_if *eth;
	int ret;

	if (!device_is_ready(vdev)) {
		printk("BRIDGE: vnet0 device not ready\n");
		return;
	}

	vnet0 = net_if_lookup_by_dev(vdev);
	if (vnet0 == NULL) {
		printk("BRIDGE: no net_if bound to vnet0\n");
		return;
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

	net_eth_bridge_foreach(vnet_bridge_find_cb, NULL);
	if (vnet_bridge_iface == NULL) {
		printk("BRIDGE: no bridge interface available\n");
		return;
	}

	eth = vnet_bridge_find_eth(vnet0, vnet_bridge_iface);
	if (eth == NULL) {
		printk("BRIDGE: no real Ethernet interface found\n");
		return;
	}

	/* Bring the real Ethernet interface up so it can carry bridged L2. */
	if (!net_if_is_admin_up(eth)) {
		(void)net_if_up(eth);
	}

	printk("BRIDGE: bridging vnet0 (iface %d) with eth (iface %d) via "
	       "bridge iface %d\n",
	       net_if_get_by_iface(vnet0), net_if_get_by_iface(eth),
	       net_if_get_by_iface(vnet_bridge_iface));

	ret = eth_bridge_iface_add(vnet_bridge_iface, vnet0);
	if (ret < 0) {
		printk("BRIDGE: add vnet0 failed (%d)\n", ret);
		return;
	}

	ret = eth_bridge_iface_add(vnet_bridge_iface, eth);
	if (ret < 0) {
		printk("BRIDGE: add eth failed (%d)\n", ret);
		return;
	}

	(void)net_if_up(vnet_bridge_iface);
	printk("BRIDGE: up - vnet0 <-> eth L2 forwarding active\n");
}

/* Return the bridge net_if so the local UDP endpoint can bind to it instead
 * of the enslaved vnet0 (which no longer delivers RX to a bound socket).
 */
struct net_if *vnet_bridge_get_iface(void)
{
	return vnet_bridge_iface;
}

