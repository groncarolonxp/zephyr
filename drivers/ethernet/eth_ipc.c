/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual Ethernet device over inter-processor shared memory + MBOX doorbell.
 *
 * Two cores that share a block of memory exchange full Ethernet frames
 * through a shared-memory FIFO in that block, and signal each other with an
 * MBOX (e.g. MU mailbox) doorbell. The device registers as a standard
 * Ethernet interface (ETH_NET_DEVICE), so the whole Zephyr IP / socket stack
 * runs on top of it unchanged - only the .send path and the RX path are
 * special.
 *
 * This is the on-SoC dual-core equivalent of eth_ivshmem (which targets QEMU
 * inter-VM shared memory). It differs in that it uses an MBOX doorbell rather
 * than the ivshmem/PCI doorbell.
 *
 * The transport ring is NOT hand-rolled here. It reuses the Zephyr
 * IPC-service "packed buffer" (struct pbuf, CONFIG_PBUF): a supported,
 * single-producer / single-consumer, length-framed shared-memory FIFO that
 * embeds all memory-barrier and cache maintenance (it invalidates only the
 * peer-owned index and flushes only its own), so it works on cores that share
 * NON-coherent memory (e.g. HyperRAM / SRAM on NXP RT1180 / MCXN947) without
 * any bespoke cache-line padding in this driver. The shared block is split
 * into two such pbuf regions, one per direction.
 *
 * Copy cost. pbuf is a wrapping byte FIFO whose only API is
 * pbuf_write(const char *) / pbuf_read(char *), so the shared-memory ring
 * itself always costs one copy per frame per direction - there is no
 * claim/commit call that would let the stack write in place. This driver
 * avoids the SECOND, staging copy on both paths:
 *   TX: when the frame is contiguous in the first fragment, pbuf_write() is
 *       pointed straight at the net_buf payload (staging buffer only for
 *       chained net_pkts).
 *   RX: the message length is peeked first, the net_pkt is allocated up
 *       front, and pbuf_read() writes directly into the net_buf fragment the
 *       IP stack will parse (staging buffer only for chained net_pkts).
 * Going to a genuine zero copy would require the net_buf data pool itself to
 * live in the shared block plus a descriptor ring of offsets, which pbuf
 * cannot express.
 *
 * Instances are created from devicetree ("zephyr,ipc-ethernet" nodes): the
 * shared-block address comes from reg, the doorbell from mboxes, and the
 * host/remote role from the "role" property. No Kconfig addresses required.
 */

#define DT_DRV_COMPAT zephyr_ipc_ethernet

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/logging/log.h>
#include <zephyr/ipc/pbuf.h>
#include <string.h>

#include "eth_ipc_priv.h"

LOG_MODULE_REGISTER(eth_ipc, CONFIG_ETHERNET_LOG_LEVEL);

/* pbuf encodes a message length with sys_put_be16(), so the framing is
 * 16-bit wide regardless of PBUF_PACKET_LEN_SZ. Anything larger would be
 * silently truncated by the (uint16_t) cast on the pbuf_write() call.
 */
BUILD_ASSERT(ETH_IPC_FRAME_MAX <= UINT16_MAX,
	     "ETH_IPC_FRAME_MAX must fit pbuf's 16-bit length field");

/* Per-instance constant configuration, resolved from devicetree. */
struct eth_ipc_cfg {
	struct pbuf *a2b;             /* host -> remote FIFO */
	struct pbuf *b2a;             /* remote -> host FIFO */
	struct mbox_dt_spec tx_db;    /* doorbell to peer */
	struct mbox_dt_spec rx_db;    /* doorbell from peer */
	bool is_host;                 /* role == "host" */
	uint8_t mac[6];
};

/* Per-instance runtime data. */
struct eth_ipc_data {
	struct net_if *iface;
	uint8_t mac[6];
	struct pbuf *tx_pb;           /* FIFO we produce into */
	struct pbuf *rx_pb;           /* FIFO we consume from */
	struct pbuf *hs_pb;           /* FIFO carrying the handshake word */
	struct k_work rx_work;
	const struct device *dev;
	bool connected;               /* peer handshake complete */
	struct k_thread connect_thread;
	K_KERNEL_STACK_MEMBER(connect_stack, CONFIG_ETH_IPC_CONNECT_STACK_SIZE);
	/* Serialises the TX producer side so concurrent .send callers cannot
	 * interleave writes into the single-producer FIFO.
	 */
	struct k_mutex tx_lock;
	/* Scratch buffer for the TX path: net_pkt_read() linearises the frame
	 * here before pbuf_write() frames it into the FIFO. Only touched
	 * under tx_lock.
	 */
	uint8_t tx_buf[ETH_IPC_FRAME_MAX];
	/* Scratch buffer for the RX path, used only on the fallback route
	 * where the allocated net_buf fragment cannot hold the whole frame
	 * contiguously. Only touched from the RX work handler.
	 */
	uint8_t rx_buf[ETH_IPC_FRAME_MAX];
};

/*
 * Consume one frame from the RX FIFO and push it up to the network stack.
 *
 * The frame length is first peeked (pbuf_read() with a NULL buffer returns the
 * pending message length without consuming it), so the net_pkt can be
 * allocated up front and pbuf_read() can land the payload DIRECTLY in the
 * net_buf fragment. That removes the intermediate staging copy: shared memory
 * is copied once, straight into the network buffer the IP stack will parse.
 *
 * If the allocated fragment cannot hold the whole frame contiguously (small
 * CONFIG_NET_BUF_DATA_SIZE, so net_pkt_rx_alloc_with_buffer() chains several
 * fragments), fall back to the staging buffer plus net_pkt_write(), which
 * handles the fragment walk.
 *
 * Returns true when a message was consumed from the FIFO (so the caller should
 * look for another one), false when the FIFO is empty or unusable.
 */
static bool eth_ipc_rx_one(struct eth_ipc_data *data)
{
	struct net_if *iface = data->iface;
	struct net_pkt *pkt;
	struct net_buf *frag;
	int plen;
	int len;
	int ret;

	/* Peek: how long is the next pending message? Does not consume it. */
	plen = pbuf_read(data->rx_pb, NULL, 0);
	if (plen <= 0) {
		if (plen < 0) {
			LOG_WRN("RX pbuf peek error (%d)", plen);
		}
		return false;
	}

	if ((size_t)plen > sizeof(data->rx_buf)) {
		/* pbuf_read() refuses to consume a message that does not fit
		 * the supplied buffer, so this frame can never be drained and
		 * the FIFO is wedged. The peer must honour ETH_IPC_FRAME_MAX,
		 * so this means the shared state is corrupt.
		 */
		LOG_ERR("RX frame too large (%d > %zu), FIFO stalled", plen,
			sizeof(data->rx_buf));
		return false;
	}

	pkt = net_pkt_rx_alloc_with_buffer(iface, plen, AF_UNSPEC, 0, K_NO_WAIT);
	if (pkt == NULL) {
		LOG_WRN("RX alloc failed (%d bytes), dropping", plen);
		/* Still drain the message, otherwise the FIFO head never
		 * advances and the link wedges.
		 */
		(void)pbuf_read(data->rx_pb, (char *)data->rx_buf,
				sizeof(data->rx_buf));
		return true;
	}

	frag = pkt->frags;

	if (frag != NULL && net_buf_tailroom(frag) >= (size_t)plen) {
		/* No staging copy: pbuf_read() lands the payload straight in
		 * the net_buf the IP stack will parse.
		 */
		len = pbuf_read(data->rx_pb, (char *)net_buf_tail(frag),
				(uint16_t)plen);
		if (len > 0) {
			net_buf_add(frag, (size_t)len);
			net_pkt_cursor_init(pkt);
		}
	} else {
		/* Chained net_pkt: the payload is not contiguous, so stage it
		 * and let net_pkt_write() walk the fragments.
		 */
		len = pbuf_read(data->rx_pb, (char *)data->rx_buf,
				sizeof(data->rx_buf));
		if (len > 0 && net_pkt_write(pkt, data->rx_buf, (size_t)len) < 0) {
			LOG_WRN("RX write failed");
			net_pkt_unref(pkt);
			return true;
		}
	}

	if (len <= 0) {
		LOG_WRN("RX pbuf_read error (%d)", len);
		net_pkt_unref(pkt);
		return false;
	}

	ret = net_recv_data(iface, pkt);
	if (ret < 0) {
		LOG_WRN("net_recv_data failed (%d)", ret);
		net_pkt_unref(pkt);
	}

	return true;
}

/* Drain the RX FIFO in thread context (submitted from the doorbell ISR). */
static void eth_ipc_rx_work(struct k_work *work)
{
	struct eth_ipc_data *data = CONTAINER_OF(work, struct eth_ipc_data,
						 rx_work);

	/* Keep going while messages are being consumed. A dropped frame still
	 * advances the FIFO, so the drain cannot livelock on it.
	 */
	while (eth_ipc_rx_one(data)) {
	}
}

static void eth_ipc_doorbell_cb(const struct device *mdev,
				mbox_channel_id_t id, void *user_data,
				struct mbox_msg *msg)
{
	struct eth_ipc_data *data = user_data;

	ARG_UNUSED(mdev);
	ARG_UNUSED(id);
	ARG_UNUSED(msg);

	/* Defer the FIFO drain to a workqueue: minimal work in ISR context. */
	k_work_submit(&data->rx_work);
}

static int eth_ipc_fifo_send(const struct device *dev, struct net_pkt *pkt,
			     size_t len)
{
	const struct eth_ipc_cfg *cfg = dev->config;
	struct eth_ipc_data *data = dev->data;
	struct net_buf *frag = pkt->frags;
	int ret;

	if (len > ETH_IPC_FRAME_MAX) {
		return -EMSGSIZE;
	}

	/* Hand the frame to pbuf as one length-framed message. pbuf_write
	 * handles the wrap, barriers and cache flush of the data plus its own
	 * write index.
	 *
	 * Fast path: when the whole frame already sits in the first fragment
	 * there is nothing to linearise, so point pbuf straight at the
	 * net_buf payload and skip the staging copy entirely. This mirrors
	 * what the RX path does and keeps the two directions at one copy
	 * each. frag->len == len (not >=) is the right test: it proves the
	 * entire frame is in this one fragment, whereas len is the sum over
	 * the whole chain.
	 *
	 * Slow path: a chained net_pkt is not contiguous, so linearise into
	 * the TX scratch buffer first. tx_buf is distinct from rx_buf on
	 * purpose: the RX work handler owns rx_buf and runs concurrently with
	 * .send, which is only serialised against other senders by tx_lock.
	 * Sharing one scratch buffer would let a concurrent RX drain corrupt
	 * the frame being transmitted.
	 */
	if (frag != NULL && frag->len == len) {
		ret = pbuf_write(data->tx_pb, (const char *)frag->data,
				 (uint16_t)len);
	} else {
		if (net_pkt_read(pkt, data->tx_buf, len) < 0) {
			return -EIO;
		}

		ret = pbuf_write(data->tx_pb, (const char *)data->tx_buf,
				 (uint16_t)len);
	}

	if (ret == -ENOMEM) {
		LOG_WRN("TX FIFO full, dropping frame");
		return -ENOBUFS;
	} else if (ret < 0) {
		return ret;
	}

	/* Ring the peer's doorbell (signalling mode: NULL message). */
	return mbox_send_dt(&cfg->tx_db, NULL);
}

/* ------------------------------------------------------------------ */
/* Handshake: run in a background thread so the two cores boot in any  */
/* order and iface_init() returns promptly.                            */
/* ------------------------------------------------------------------ */
#define ETH_IPC_CONNECT_POLL_MS   50
#define ETH_IPC_CONNECT_LOG_EVERY 20   /* 20 * 50ms == ~1s between logs */

static void eth_ipc_connect_thread_fn(void *p1, void *p2, void *p3)
{
	struct eth_ipc_data *data = p1;
	const struct device *dev = data->dev;
	const struct eth_ipc_cfg *cfg = dev->config;
	uint32_t tries = 0;
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ipc-eth connect: role %s", cfg->is_host ? "HOST(A)" : "REMOTE(B)");

	/* Register the RX doorbell first so we never miss the peer's FIFO. */
	k_work_init(&data->rx_work, eth_ipc_rx_work);

	ret = mbox_register_callback_dt(&cfg->rx_db, eth_ipc_doorbell_cb, data);
	if (ret < 0) {
		LOG_ERR("mbox register failed (%d)", ret);
		return;
	}

	ret = mbox_set_enabled_dt(&cfg->rx_db, true);
	if (ret < 0) {
		LOG_ERR("mbox enable failed (%d)", ret);
		return;
	}

	if (cfg->is_host) {
		/* Host is the writer of a2b and the reader of b2a. */
		data->tx_pb = cfg->a2b;
		data->rx_pb = cfg->b2a;
		data->hs_pb = cfg->a2b;

		/* The host owns FIFO init: pbuf_tx_init clears the shared
		 * indices for the direction it writes; it also initialises the
		 * reader side it will drain.
		 */
		if (pbuf_tx_init(data->tx_pb) < 0 ||
		    pbuf_rx_init(data->rx_pb) < 0) {
			LOG_ERR("ipc-eth HOST pbuf init failed");
			return;
		}

		/* Publish readiness through the standard pbuf handshake word. */
		pbuf_handshake_write(data->hs_pb, ETH_IPC_HS_READY);
		LOG_INF("ipc-eth HOST published FIFOs (handshake 0x%08x)",
			ETH_IPC_HS_READY);

		/* Poke the peer in case the remote started first. */
		(void)mbox_send_dt(&cfg->tx_db, NULL);
	} else {
		/* Remote is the writer of b2a and the reader of a2b. */
		data->tx_pb = cfg->b2a;
		data->rx_pb = cfg->a2b;
		data->hs_pb = cfg->a2b;

		LOG_INF("ipc-eth REMOTE waiting for host to publish FIFOs ...");
		while (pbuf_handshake_read(data->hs_pb) != ETH_IPC_HS_READY) {
			if ((tries % ETH_IPC_CONNECT_LOG_EVERY) == 0U) {
				LOG_INF("ipc-eth REMOTE still waiting, retry %u",
					tries);
			}
			tries++;
			k_sleep(K_MSEC(ETH_IPC_CONNECT_POLL_MS));
		}

		/* Host already wrote the shared indices; attach as reader of
		 * a2b and writer of b2a.
		 */
		if (pbuf_rx_init(data->rx_pb) < 0 ||
		    pbuf_tx_init(data->tx_pb) < 0) {
			LOG_ERR("ipc-eth REMOTE pbuf init failed");
			return;
		}

		LOG_INF("ipc-eth REMOTE handshake done after %u retries", tries);

		/* Acknowledge so the host knows the remote is alive. */
		(void)mbox_send_dt(&cfg->tx_db, NULL);
	}

	data->connected = true;

	if (data->iface != NULL) {
		net_eth_carrier_on(data->iface);
		LOG_INF("ipc-eth link connected, carrier raised");
	}
}

/* ------------------------------------------------------------------ */
/* Ethernet driver API.                                               */
/* ------------------------------------------------------------------ */
static int eth_ipc_send(const struct device *dev, struct net_pkt *pkt)
{
	struct eth_ipc_data *data = dev->data;
	size_t len = net_pkt_get_len(pkt);
	int ret;

	if (len > ETH_IPC_FRAME_MAX) {
		LOG_WRN("TX frame too large (%zu)", len);
		return -EMSGSIZE;
	}

	if (!data->connected) {
		LOG_WRN("TX dropped: peer not connected yet (%zu bytes)", len);
		return -ENETDOWN;
	}

	/* Serialise the producer side: pbuf is single-producer. */
	k_mutex_lock(&data->tx_lock, K_FOREVER);

	ret = eth_ipc_fifo_send(dev, pkt, len);

	k_mutex_unlock(&data->tx_lock);

	return ret;
}

static void eth_ipc_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct eth_ipc_data *data = dev->data;

	data->iface = iface;

	net_if_set_link_addr(iface, data->mac, sizeof(data->mac),
			     NET_LINK_ETHERNET);

	ethernet_init(iface);

	LOG_INF("ipc-eth iface_init, MAC %02x:%02x:%02x:%02x:%02x:%02x",
		data->mac[0], data->mac[1], data->mac[2],
		data->mac[3], data->mac[4], data->mac[5]);

	/* Do NOT raise the carrier here: the background connect thread raises
	 * it only once the peer handshake succeeds, so the interface reports
	 * "down" until the counterpart core is actually up.
	 */
	k_thread_create(&data->connect_thread, data->connect_stack,
			K_KERNEL_STACK_SIZEOF(data->connect_stack),
			eth_ipc_connect_thread_fn, data, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&data->connect_thread, "ipc_eth_connect");
}

static enum ethernet_hw_caps eth_ipc_caps(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* Advertise promiscuous mode so this interface can be added to an
	 * Ethernet bridge (eth_bridge_iface_add() requires it).
	 */
	return ETHERNET_LINK_10BASE | ETHERNET_PROMISC_MODE;
}

static int eth_ipc_set_config(const struct device *dev,
			      enum ethernet_config_type type,
			      const struct ethernet_config *config)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);

	/* The virtual link has no hardware filter; promiscuous mode is a
	 * no-op (we already deliver every frame we receive). Accept it so
	 * the bridge can enable it.
	 */
	if (type == ETHERNET_CONFIG_TYPE_PROMISC_MODE) {
		return 0;
	}

	return -ENOTSUP;
}

static int eth_ipc_start(const struct device *dev)
{
	struct eth_ipc_data *data = dev->data;

	if (data->iface != NULL && data->connected) {
		net_eth_carrier_on(data->iface);
	}
	return 0;
}

static int eth_ipc_stop(const struct device *dev)
{
	struct eth_ipc_data *data = dev->data;

	if (data->iface != NULL) {
		net_eth_carrier_off(data->iface);
	}
	return 0;
}

static const struct ethernet_api eth_ipc_api = {
	.iface_api.init = eth_ipc_iface_init,
	.get_capabilities = eth_ipc_caps,
	.set_config = eth_ipc_set_config,
	.start = eth_ipc_start,
	.stop = eth_ipc_stop,
	.send = eth_ipc_send,
};

static int eth_ipc_dev_init(const struct device *dev)
{
	const struct eth_ipc_cfg *cfg = dev->config;
	struct eth_ipc_data *data = dev->data;

	data->dev = dev;
	memcpy(data->mac, cfg->mac, sizeof(data->mac));
	k_mutex_init(&data->tx_lock);

	if (!mbox_is_ready_dt(&cfg->tx_db) || !mbox_is_ready_dt(&cfg->rx_db)) {
		LOG_ERR("mbox not ready");
		return -ENODEV;
	}

	return 0;
}

/* Resolve the role to a boolean at compile time per instance. The binding
 * lists "host" first in the enum, so index 0 is the host side. Using the
 * enum index rather than strcmp() keeps this a genuine constant expression
 * for the static initialiser below.
 */
#define ETH_IPC_IS_HOST(inst) (DT_INST_ENUM_IDX(inst, role) == 0)

/* Each direction gets half of the devicetree reg block. The split is aligned
 * down to the cache-line granularity pbuf requires.
 */
#define ETH_IPC_REGION_SIZE(inst) \
	ROUND_DOWN(DT_INST_REG_SIZE(inst) / 2U, ETH_IPC_DCACHE_ALIGN)

#define ETH_IPC_A2B_ADDR(inst)  (DT_INST_REG_ADDR(inst))
#define ETH_IPC_B2A_ADDR(inst)  (DT_INST_REG_ADDR(inst) + ETH_IPC_REGION_SIZE(inst))

/* Smallest region that can still carry one maximum-size frame: pbuf's own
 * header (indices + handshake + padding), the per-message length field, and
 * the frame itself, plus the one index-sized word pbuf keeps free to tell
 * "full" from "empty". PBUF_DEFINE only asserts enough room for a 1-byte
 * message, which would let an undersized reg build cleanly and then drop
 * every real frame at runtime.
 */
#define ETH_IPC_MIN_REGION_SIZE							\
	(PBUF_HEADER_OVERHEAD(ETH_IPC_DCACHE_ALIGN) + PBUF_PACKET_LEN_SZ +	\
	 ETH_IPC_FRAME_MAX + sizeof(uint32_t))

#define ETH_IPC_DEFINE(inst)							\
	BUILD_ASSERT(ETH_IPC_REGION_SIZE(inst) >= ETH_IPC_MIN_REGION_SIZE,	\
		     "zephyr,ipc-ethernet reg is too small: each direction "	\
		     "needs room for one full frame plus pbuf overhead");	\
										\
	/* Two unidirectional packed buffers carved from the shared block.	\
	 * use_handshake == true so the host/remote connect sequence can use	\
	 * pbuf's handshake word instead of a bespoke magic field.		\
	 */									\
	PBUF_DEFINE(eth_ipc_a2b_##inst, ETH_IPC_A2B_ADDR(inst),			\
		    ETH_IPC_REGION_SIZE(inst), ETH_IPC_DCACHE_ALIGN, true, false); \
	PBUF_DEFINE(eth_ipc_b2a_##inst, ETH_IPC_B2A_ADDR(inst),			\
		    ETH_IPC_REGION_SIZE(inst), ETH_IPC_DCACHE_ALIGN, true, false); \
										\
	static struct eth_ipc_data eth_ipc_data_##inst;				\
										\
	static const struct eth_ipc_cfg eth_ipc_cfg_##inst = {			\
		.a2b = &eth_ipc_a2b_##inst,					\
		.b2a = &eth_ipc_b2a_##inst,					\
		.tx_db = MBOX_DT_SPEC_INST_GET(inst, tx),			\
		.rx_db = MBOX_DT_SPEC_INST_GET(inst, rx),			\
		.is_host = ETH_IPC_IS_HOST(inst),				\
		.mac = DT_INST_PROP(inst, local_mac_address),			\
	};									\
										\
	ETH_NET_DEVICE_DT_INST_DEFINE(inst, eth_ipc_dev_init, NULL,		\
				      &eth_ipc_data_##inst,			\
				      &eth_ipc_cfg_##inst,			\
				      CONFIG_ETH_INIT_PRIORITY,			\
				      &eth_ipc_api, NET_ETH_MTU);

DT_INST_FOREACH_STATUS_OKAY(ETH_IPC_DEFINE)
