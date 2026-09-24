/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared UDP demo for the virtual network device.
 *
 * Phase 1 (loopback): sends a datagram to this interface's own address and
 * receives it back, proving the socket stack runs on vnet0.
 *
 * Phase 2 (shared memory): send to the peer core's address instead; the
 * frame crosses the shared ring and is received on the other core.
 *
 * This header only declares the public entry point; the implementation
 * lives in vnet_demo.c.
 */

#ifndef VNET_DEMO_H_
#define VNET_DEMO_H_

#ifdef __cplusplus
extern "C" {
#endif

/* UDP port used by both ends of the demo. */
#define VNET_UDP_PORT   4242

/*
 * Run the virtual network UDP demo.
 *
 * self_addr : IPv4 address bound locally (this interface).
 * peer_addr : IPv4 address to send to. For loopback this is self_addr.
 * tag       : short string included in the payload to identify the sender.
 *
 * This call does not return: it brings vnet0 up, spawns a dedicated RX
 * thread, and runs a periodic TX loop on the calling thread.
 */
void vnet_demo_run(const char *self_addr, const char *peer_addr,
		   const char *tag);

/*
 * Host-only: L2-bridge vnet0 with the real on-board Ethernet interface so the
 * remote core can reach an off-chip host. Available when CONFIG_VNET_BRIDGE is
 * set. Does not return meaningful state; call once at startup on the host.
 */
void vnet_bridge_run(void);

/*
 * Override the interface the UDP demo binds its socket and IP address to.
 * By default the demo uses "vnet0". On a bridging host, vnet0 is enslaved to
 * the L2 bridge and no longer delivers received frames to a socket bound to
 * it, so the local endpoint must live on the bridge interface instead. Call
 * this with the bridge net_if (see vnet_bridge_get_iface()) before
 * vnet_demo_run() so RX works on the bridging core. Pass NULL to clear.
 */
struct net_if;
void vnet_demo_set_iface(struct net_if *iface);

/*
 * Host-only: return the L2 bridge net_if created by vnet_bridge_run(), or
 * NULL if the bridge has not been set up. Used to point the local UDP
 * endpoint at the bridge interface rather than the enslaved vnet0.
 */
struct net_if *vnet_bridge_get_iface(void);


#ifdef __cplusplus
}
#endif


#endif /* VNET_DEMO_H_ */
