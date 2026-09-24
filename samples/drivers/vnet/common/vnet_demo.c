/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared UDP demo for the virtual network device (implementation).
 *
 * Phase 1 (loopback): sends a datagram to this interface's own address and
 * receives it back, proving the socket stack runs on vnet0.
 *
 * Phase 2 (shared memory): send to the peer core's address instead; the
 * frame crosses the shared ring and is received on the other core.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <string.h>

#include "vnet_demo.h"

/* Return the vnet0 interface. IMPORTANT: on this board there are real
 * ENETC Ethernet interfaces too, and net_if_get_default() returns one of
 * those (with the cable unplugged, so carrier is down). We must select
 * the virtual interface explicitly by its bound device "vnet0", otherwise
 * the demo waits on / sends through the wrong (real) interface and fails
 * with ENETDOWN (errno 115).
 */
/* Optional override: when the demo runs on a bridging core, vnet0 is
 * enslaved to the L2 bridge and no longer delivers RX to a socket bound to
 * it. In that case the caller sets this to the bridge net_if so the local
 * UDP endpoint (IP address + SO_BINDTODEVICE + recvfrom) lives on the bridge
 * interface, which does receive frames destined to this core.
 */
static struct net_if *vnet_iface_override;

void vnet_demo_set_iface(struct net_if *iface)
{
	vnet_iface_override = iface;
}

static struct net_if *vnet_get_iface(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vnet0));
	struct net_if *iface;

	if (vnet_iface_override != NULL) {
		return vnet_iface_override;
	}

	if (device_is_ready(dev)) {

		iface = net_if_lookup_by_dev(dev);
		if (iface != NULL) {
			return iface;
		}
		printk("VNET: no net_if bound to vnet0 device\n");
	} else {
		printk("VNET: vnet0 device not ready\n");
	}

	/* Fallback so we at least run somewhere. */
	return net_if_get_default();
}

/* Bring the vnet0 interface administratively up and wait until it is
 * operational, then make sure our IPv4 address is on THIS interface. On a
 * virtual link there is no PHY to raise carrier, so without this the stack
 * reports ENETDOWN (errno 115) on send.
 */
static struct net_if *vnet_wait_iface_up(const char *self_addr)
{
	struct net_if *iface = vnet_get_iface();
	struct in_addr addr;
	int i;

	if (iface == NULL) {
		printk("VNET: no vnet interface\n");
		return NULL;
	}

	printk("VNET: using interface %d (vnet0)\n",
	       net_if_get_by_iface(iface));

	if (!net_if_is_admin_up(iface)) {
		(void)net_if_up(iface);
	}

	for (i = 0; i < 100; i++) {
		if (net_if_is_up(iface)) {
			break;
		}
		if (!net_if_is_admin_up(iface)) {
			(void)net_if_up(iface);
		}
		k_sleep(K_MSEC(50));
	}

	if (!net_if_is_up(iface)) {
		printk("VNET: interface did not come up "
		       "(admin %s, carrier %s)\n",
		       net_if_is_admin_up(iface) ? "up" : "down",
		       net_if_is_carrier_ok(iface) ? "ok" : "down");
		return iface;
	}

	printk("VNET: interface is up\n");

	/* NET_CONFIG applies the configured IPv4 address to the DEFAULT
	 * interface (a real ENETC here), not to vnet0. Add it to vnet0
	 * explicitly so our socket can bind and route over the virtual link.
	 *
	 * The address may already be assigned to a different (real ENETC)
	 * interface by NET_CONFIG. net_if_ipv4_addr_lookup() with a NULL
	 * "ret iface" argument matches the address on ANY interface, so a
	 * plain lookup would make us wrongly conclude the address is already
	 * where we need it and skip adding it to vnet0 - leaving the socket
	 * without a usable local address on the virtual link (sendto then
	 * fails with ENETDOWN, errno 115). Instead, if the address lives on
	 * another interface, remove it there first, then add it to vnet0.
	 */
	if (self_addr != NULL &&
	    zsock_inet_pton(AF_INET, self_addr, &addr) == 1) {
		struct net_if *cur_iface = NULL;

		if (net_if_ipv4_addr_lookup(&addr, &cur_iface) != NULL &&
		    cur_iface != NULL && cur_iface != iface) {
			(void)net_if_ipv4_addr_rm(cur_iface, &addr);
			printk("VNET: moved %s off iface %d\n", self_addr,
			       net_if_get_by_iface(cur_iface));
		}

		if (net_if_ipv4_addr_lookup(&addr, &cur_iface) != NULL &&
		    cur_iface == iface) {
			printk("VNET: %s already on vnet0\n", self_addr);
		} else if (net_if_ipv4_addr_add(iface, &addr,
						 NET_ADDR_MANUAL, 0) == NULL) {
			printk("VNET: failed to add %s to vnet0\n", self_addr);
		} else {
			printk("VNET: added %s to vnet0\n", self_addr);
		}
	}

	return iface;
}

/* Context shared between the TX and RX threads. */
struct vnet_demo_ctx {
	int sock;
	struct sockaddr_in peer;
	const char *peer_addr;
	const char *tag;
	/* Name of the interface the local endpoint is bound to (e.g. "vnet0"
	 * or the bridge interface on the host core), reported on each RX so
	 * it is clear which interface delivered the datagram.
	 */
	char ifname[CONFIG_NET_INTERFACE_NAME_LEN + 1];
};


/* RX thread: block on recvfrom() and print every datagram from the peer.
 * Running receive in its own thread decouples it from the send cadence, so
 * a slow or bursty TX side never delays delivery and vice versa.
 */
static void vnet_rx_thread_fn(void *p1, void *p2, void *p3)
{
	struct vnet_demo_ctx *c = p1;
	char rxbuf[64];

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		struct sockaddr_in from = {0};
		socklen_t fromlen = sizeof(from);
		char fromaddr[NET_IPV4_ADDR_LEN] = {0};
		int ret;

		ret = zsock_recvfrom(c->sock, rxbuf, sizeof(rxbuf) - 1, 0,
				     (struct sockaddr *)&from, &fromlen);
		if (ret > 0) {
			rxbuf[ret] = '\0';
			zsock_inet_ntop(AF_INET, &from.sin_addr, fromaddr,
					sizeof(fromaddr));
			printk("VNET: [RX] received %d bytes on %s from "
			       "%s:%d: \"%s\"\n", ret,
			       c->ifname[0] ? c->ifname : "?", fromaddr,
			       ntohs(from.sin_port), rxbuf);

		} else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			printk("VNET: [RX] recv error (%d)\n", errno);
			k_sleep(K_MSEC(100));
		}
	}
}

/* Dedicated RX thread resources. */
#define VNET_RX_STACK_SIZE 2048
static K_THREAD_STACK_DEFINE(vnet_rx_stack, VNET_RX_STACK_SIZE);
static struct k_thread vnet_rx_thread;
static struct vnet_demo_ctx vnet_ctx;

/* Maximum number of peer addresses we will send to per TX cycle. */
#define VNET_MAX_PEERS 4

/* Parse a comma-separated list of IPv4 addresses (e.g. "192.0.2.2,192.0.2.1")
 * into an array of sockaddr_in. Returns the number of peers parsed.
 */
static int vnet_parse_peers(const char *peer_addr,
			    struct sockaddr_in peers[VNET_MAX_PEERS],
			    char peer_str[VNET_MAX_PEERS][NET_IPV4_ADDR_LEN])
{
	char buf[VNET_MAX_PEERS * NET_IPV4_ADDR_LEN];
	char *p = buf;
	char *tok;
	int n = 0;

	if (peer_addr == NULL) {
		return 0;
	}

	strncpy(buf, peer_addr, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	while ((tok = strtok_r(p, ",", &p)) != NULL && n < VNET_MAX_PEERS) {
		/* Skip leading spaces after a comma. */
		while (*tok == ' ') {
			tok++;
		}
		if (*tok == '\0') {
			continue;
		}

		memset(&peers[n], 0, sizeof(peers[n]));
		peers[n].sin_family = AF_INET;
		peers[n].sin_port = htons(VNET_UDP_PORT);
		if (zsock_inet_pton(AF_INET, tok, &peers[n].sin_addr) != 1) {
			printk("VNET: skipping invalid peer \"%s\"\n", tok);
			continue;
		}
		strncpy(peer_str[n], tok, NET_IPV4_ADDR_LEN - 1);
		peer_str[n][NET_IPV4_ADDR_LEN - 1] = '\0';
		n++;
	}

	return n;
}

void vnet_demo_run(const char *self_addr, const char *peer_addr,
		   const char *tag)
{
	int sock;
	struct sockaddr_in local = {0};
	struct sockaddr_in peers[VNET_MAX_PEERS];
	char peer_str[VNET_MAX_PEERS][NET_IPV4_ADDR_LEN];
	int npeers;
	char msg[80];
	int ret;
	int i;

	struct net_if *iface;
	uint32_t seq = 0;

	/* peer_addr may be a single address or a comma-separated list, e.g.
	 * "192.0.2.2,192.0.2.1" to send to both cores each cycle.
	 */
	npeers = vnet_parse_peers(peer_addr, peers, peer_str);
	if (npeers == 0) {
		printk("VNET: no valid peer address in \"%s\"\n",
		       peer_addr ? peer_addr : "(null)");
		return;
	}

	/* Ensure the vnet0 interface is operational and owns our IP. */
	iface = vnet_wait_iface_up(self_addr);


	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		printk("VNET: socket() failed (%d)\n", errno);
		return;
	}

	/* Force this socket onto vnet0 so traffic does not leak to the real
	 * ENETC interface (which is the default and has no link).
	 */
	if (iface != NULL) {
		struct net_ifreq ifreq = {0};

		/* SO_BINDTODEVICE expects a struct net_ifreq (name inside
		 * ifr_name) and optlen == sizeof(struct net_ifreq). Passing
		 * the bare name string with optlen == strlen() makes the
		 * kernel reject it with EINVAL (errno 22).
		 */
		if (net_if_get_name(iface, ifreq.ifr_name,
				    sizeof(ifreq.ifr_name)) > 0) {
			ret = zsock_setsockopt(sock, SOL_SOCKET,
					       SO_BINDTODEVICE, &ifreq,
					       sizeof(ifreq));
			if (ret < 0) {
				printk("VNET: SO_BINDTODEVICE(%s) failed (%d)\n",
				       ifreq.ifr_name, errno);
			} else {
				printk("VNET: socket bound to %s\n",
				       ifreq.ifr_name);
			}
		}
	}

	local.sin_family = AF_INET;
	local.sin_port = htons(VNET_UDP_PORT);
	local.sin_addr.s_addr = INADDR_ANY;

	ret = zsock_bind(sock, (struct sockaddr *)&local, sizeof(local));
	if (ret < 0) {
		printk("VNET: bind() failed (%d)\n", errno);
		zsock_close(sock);
		return;
	}

	/* Do not block forever waiting for a reply. */
	{
		struct zsock_timeval tv = { .tv_sec = 1, .tv_usec = 0 };

		(void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv,
				       sizeof(tv));
	}

	/* Publish the shared context and start the dedicated RX thread. The
	 * receive path now runs independently of the transmit cadence.
	 */
	vnet_ctx.sock = sock;
	vnet_ctx.peer = peers[0];
	vnet_ctx.peer_addr = peer_addr;
	vnet_ctx.tag = tag;

	/* Record the bound interface name so each RX line shows which
	 * interface delivered the datagram (e.g. "vnet0" on the remote core,
	 * or the bridge interface on the host core).
	 */
	vnet_ctx.ifname[0] = '\0';
	if (iface != NULL) {
		(void)net_if_get_name(iface, vnet_ctx.ifname,
				      sizeof(vnet_ctx.ifname) - 1);
	}



	k_thread_create(&vnet_rx_thread, vnet_rx_stack,
			K_THREAD_STACK_SIZEOF(vnet_rx_stack),
			vnet_rx_thread_fn, &vnet_ctx, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&vnet_rx_thread, "vnet_rx");

	/* This (main) thread is now the TX loop only. */
	while (1) {
		/* Include a sequence number so successive iterations are
		 * visible in the log (otherwise identical payloads make the
		 * loop look like it only ran once).
		 */
		snprintk(msg, sizeof(msg), "hello-from-%s #%u", tag, seq++);

		/* Send the same datagram to every configured peer this
		 * cycle (e.g. both cpu0 and cpu1, and/or an off-chip PC).
		 */
		for (i = 0; i < npeers; i++) {
			ret = zsock_sendto(sock, msg, strlen(msg), 0,
					   (struct sockaddr *)&peers[i],
					   sizeof(peers[i]));
			if (ret < 0) {
				printk("VNET: [TX] sendto(%s:%d) failed (%d)\n",
				       peer_str[i], VNET_UDP_PORT, errno);
			} else {
				printk("VNET: [TX] sent %d bytes to %s:%d: "
				       "\"%s\"\n", ret, peer_str[i],
				       VNET_UDP_PORT, msg);
			}
		}

		k_sleep(K_MSEC(2000));

	}
}
