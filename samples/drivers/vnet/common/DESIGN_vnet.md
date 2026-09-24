# vnet - technical design and theory of operation

This document explains, in depth, how the virtual network device (`vnet0`)
works: the data structures, the shared-memory ring protocol, the doorbell
handshake, the memory model / cache-coherency reasoning, the interface
bring-up, and the end-to-end packet path. It complements `README_vnet.md`
(which is the quick-start / overview) with the "why it is written this way"
detail.

Target platform for the concrete numbers below is the NXP i.MX RT1180 EVK:
Cortex-M33 (host, "role A") + Cortex-M7 (remote, "role B"), sharing HyperRAM.

--------------------------------------------------------------------------------

## 1. Layering

```
   Application (src/main.c, remote/src/main.c)
        |  vnet_demo_run()
   Demo/socket layer (common/vnet_demo.c)         <- UDP sockets, TX & RX threads
        |  BSD sockets (zsock_*)
   Zephyr networking stack                         <- sockets, UDP, IPv4, ARP
        |  net_if / Ethernet L2
   Virtual Ethernet driver (common/vnet_eth.c)    <- ETH_NET_DEVICE_INIT("vnet0")
        |  shared ring + MU doorbell
   Shared-memory transport (common/vnet_shm.h)     <- struct vnet_shm in HyperRAM
        |
   Peer core (mirror of the whole stack)
```

`vnet0` registers as a normal Ethernet device (`ETH_NET_DEVICE_INIT`), so the
entire Zephyr IP/socket stack runs on top of it unmodified. Only the driver's
`.send` and its RX path are special: instead of a MAC/PHY, they move frames
through shared memory and signal the peer with the MU mailbox.

--------------------------------------------------------------------------------

## 2. Shared-memory layout (`vnet_shm.h`)

The two cores agree on one block of shared RAM described by `struct vnet_shm`:

```c
struct vnet_slot {
    uint32_t len;                 /* valid bytes in data[], 0 == empty */
    uint8_t  data[VNET_FRAME_MAX];/* one Ethernet frame (1536 bytes) */
};

struct vnet_ring {
    volatile uint32_t head;       /* producer index; its own cache line */
    uint8_t _pad_head[VNET_CACHE_LINE - sizeof(uint32_t)];
    volatile uint32_t tail;       /* consumer index; its own cache line */
    uint8_t _pad_tail[VNET_CACHE_LINE - sizeof(uint32_t)];
    struct vnet_slot slot[VNET_RING_SLOTS]; /* 8 slots, power of two */
};

struct vnet_shm {
    uint32_t magic;               /* VNET_SHM_MAGIC once host has initialised */
    uint32_t version;
    uint32_t _pad[2];
    struct vnet_ring a2b;         /* host -> remote */
    struct vnet_ring b2a;         /* remote -> host */
};
```

* Two rings, one per direction. Each ring is strictly **single-producer /
  single-consumer**: for `a2b`, the host is the only producer and the remote is
  the only consumer (and vice-versa for `b2a`). This is what lets the ring work
  with no locks - only ordering and cache maintenance.
* `head` is written only by the producer; `tail` only by the consumer.
* `VNET_RING_SLOTS` is a power of two (8), so `vnet_ring_next()` is a cheap mask
  (`(idx + 1) & (SLOTS - 1)`).
* Ring state helpers:
  * empty: `head == tail`
  * full : `next(head) == tail` (one slot is intentionally left unused so the
    empty and full conditions are distinguishable).
* `sizeof(struct vnet_shm)` is ~24.7 KB with the current constants.

### Address views

The same physical block is mapped at different addresses from each core, so
each side is told its own view via Kconfig:

| Core | `CONFIG_VNET_SHM_ADDR` | Note |
|------|------------------------|------|
| CM33 host   | `0x14700000` | top 1 MB of hyperram0 as seen from CM33 |
| CM7 remote  | `0x04700000` | same block, CM7 bus view (offset 0x10000000) |

The CM33 devicetree overlay shrinks `hyperram0` so Zephyr does not allocate the
top 1 MB; that window holds `struct vnet_shm`. The CM7 overlay enables its view
of the same region.

--------------------------------------------------------------------------------

## 3. Connection handshake

`vnet0` must not report "link up" until the peer is actually present, otherwise
the stack would try to transmit into a ring nobody is draining. The driver runs
a background **connect thread** (`vnet_connect_thread_fn`) so `iface_init()`
returns immediately and the two cores may boot in any order.

Host (role A):
1. Registers and enables the RX doorbell first (so it never misses an early
   poke from a remote that booted first).
2. Zeroes the block, sets `version`, then (after a `__DMB()`) writes `magic`.
   Writing `magic` last is the publish barrier: the remote keys off it.
3. Pokes the peer doorbell once, sets `connected = true`, and raises the
   virtual carrier (`net_eth_carrier_on`).

Remote (role B):
1. Registers and enables its RX doorbell.
2. Spins (with `k_sleep`) until it observes `magic == VNET_SHM_MAGIC`, logging
   progress periodically. This is the "peer may not be up yet" retry loop.
3. Checks `version`, acknowledges by poking the host doorbell, sets
   `connected = true`, raises its carrier.

Ring ownership after handshake:

| | TX ring (produce) | RX ring (consume) |
|--|-------------------|-------------------|
| Host (A)   | `a2b` | `b2a` |
| Remote (B) | `b2a` | `a2b` |

Until `connected` is true, `vnet_send()` returns `-ENETDOWN` so upper layers
know the link is not ready.

--------------------------------------------------------------------------------

## 4. Doorbell (MU mailbox)

The Messaging Unit moves only a few bytes per channel, so it is used purely as
a **signal**, not a data path:

* TX: after publishing a frame the producer calls `mbox_send_dt(&tx_db, NULL)`
  (signalling mode, no payload).
* RX: the doorbell callback `vnet_doorbell_cb` runs in interrupt context and
  does the minimum - it submits `rx_work` to the system workqueue. The actual
  ring drain (`vnet_rx_work`) runs in thread context, where it is safe to
  allocate `net_pkt`s and call into the stack.

The `tx`/`rx` channels come from the existing `mbox-consumer` devicetree node;
`vnet_late_init()` binds them into the driver instance at `PRE_KERNEL_1`.

--------------------------------------------------------------------------------

## 5. Memory model and cache coherency (the important part)

The shared block lives in **cacheable** HyperRAM and the two cores do not share
a coherent cache. Two distinct problems must both be handled; a plain
`__DMB()` solves neither because a barrier only orders a core's own accesses,
it does not move data between a core's cache and RAM.

### 5.1 Data visibility

* **Producer** must **flush (clean)** its cache after writing a slot and after
  advancing its index, so the bytes actually land in RAM for the peer:
  `sys_cache_data_flush_range()`.
* **Consumer** must **invalidate** its cache before reading, so it re-fetches
  the producer's fresh data instead of a stale cached copy:
  `sys_cache_data_invd_range()`.

These are wrapped in `vnet_cache_flush()` / `vnet_cache_invd()` and compile to
nothing when `CONFIG_DCACHE` is not set.

### 5.2 False sharing of `head` / `tail`

`head` is owned by the producer, `tail` by the consumer, but a cache
clean/invalidate operates at **cache-line granularity**. If `head` and `tail`
shared a line:

* the producer flushing its `head` line would also write back its stale copy of
  `tail`, stomping the consumer's real progress;
* the consumer invalidating to read `head` could discard its own not-yet-
  flushed `tail`.

The net effect observed on hardware was the producer reading a `tail` that never
advanced, so after ~7 frames (ring depth 8) it declared the ring permanently
full: `TX ring full, dropping frame`. The fix is structural: `head` and `tail`
are padded onto **separate cache lines** (`VNET_CACHE_LINE = 32`). Each side
then only ever maintains its own index's line.

### 5.3 Exact maintenance sequence

Producer (`vnet_shm_send`):
1. `invd(&tail)` - refresh the peer-owned consumer index.
2. check `vnet_ring_full()`; drop if full.
3. `memcpy` frame into `slot[head]`, set `len`.
4. `flush(slot[head])` - push the frame to RAM.
5. `__DMB()`, advance `head`, `flush(&head)` - publish the index.
6. ring the TX doorbell.

Consumer (`vnet_rx_work`):
1. `invd(&head)` - see the producer's latest index.
2. while not empty:
   * `invd(slot[tail])`, read `len`, deliver `slot.data` up the stack.
   * clear `len`, `flush(slot[tail])`.
   * `__DMB()`, advance `tail`, `flush(&tail)` - publish consumption.

`volatile` on `head`/`tail` prevents the compiler from caching them in
registers; `__DMB()` orders the slot write before the index write (and the
index write before the doorbell) at the CPU level; the cache ops handle the
memory-system level.

--------------------------------------------------------------------------------

## 6. Interface bring-up and addressing (`vnet_demo.c`)

The RT1180 board also has real ENETC interfaces, which introduces several
sharp edges the demo handles explicitly:

1. **Pick vnet0 explicitly.** `net_if_get_default()` returns a real ENETC
   interface (cable unplugged, carrier down). The demo resolves `vnet0` by
   `device_get_binding("vnet0")` + `net_if_lookup_by_dev()`. Using the default
   would fail every send with `ENETDOWN` (115).

2. **Bring vnet0 admin-up and wait for oper-up.** `net_if_up()` then poll
   `net_if_is_up()`; the driver raises the virtual carrier once the peer
   handshake completes.

3. **Move the IPv4 address onto vnet0.** `NET_CONFIG` assigns
   `CONFIG_NET_CONFIG_MY_IPV4_ADDR` to the default (ENETC) interface.
   `net_if_ipv4_addr_lookup(&addr, NULL)` matches on *any* interface, so a naive
   check would wrongly conclude the address is already where it is needed. The
   demo captures the owning interface, removes the address from the wrong
   interface, and adds it to `vnet0`. This also requires
   `CONFIG_NET_IF_MAX_IPV4_COUNT` / `CONFIG_NET_IF_UNICAST_IPV4_ADDR_COUNT` > 1
   (set to 4 in the board confs), otherwise only one interface can hold an IPv4
   address at a time.

4. **Bind the socket to vnet0.** `SO_BINDTODEVICE` requires the option value to
   be a `struct net_ifreq` (name in `ifr_name`) with
   `optlen == sizeof(struct net_ifreq)`. Passing the bare interface-name string
   with `optlen == strlen(name)` is rejected with `EINVAL` (22). Binding keeps
   traffic from leaking onto the carrier-less ENETC default.

--------------------------------------------------------------------------------

## 7. Threading model

`vnet_demo_run()` splits transmit and receive into independent threads so
neither blocks the other:

* **TX (main thread):** every 2 s, `sendto()` a payload
  `hello-from-<tag> #<seq>` with an incrementing sequence number, and log
  `[TX] ...`.
* **RX (`vnet_rx` thread):** a blocking `recvfrom()` loop that logs
  `[RX] received ... from <ip>:<port>: "..."` for every datagram.

Inside the driver, the doorbell ISR only submits a work item; the ring drain
runs on the system workqueue. So there are effectively three contexts per core:
TX thread (producer), MU ISR (signal), and workqueue (consumer + stack RX).

End-to-end path for one datagram, host -> remote:

```
host TX thread: sendto()
  -> UDP/IPv4/ARP -> Ethernet L2 -> vnet_send()
     -> vnet_shm_send(): copy into a2b.slot[head], flush, advance head, flush
     -> mbox_send_dt(tx_db)              === MU IRQ ===>
                                          remote vnet_doorbell_cb (ISR)
                                            -> k_work_submit(rx_work)
                                          remote workqueue: vnet_rx_work
                                            -> invd, read slot, net_recv_data()
                                            -> UDP -> socket recv queue
                                          remote RX thread: recvfrom() -> [RX] log
```

--------------------------------------------------------------------------------

## 8. Configuration reference

Per-board `.conf` values that matter for this sample (i.MX RT1180):

| Option | CM33 (host) | CM7 (remote) | Why |
|--------|-------------|--------------|-----|
| `CONFIG_VNET_MODE_SHM`   | y | y | cross-core transport |
| `CONFIG_VNET_ROLE_HOST`  | y | n | who publishes the block |
| `CONFIG_VNET_MAC_LSB`    | 2 | 1 | distinct MACs on one L2 segment |
| `CONFIG_VNET_SHM_ADDR`   | 0x14700000 | 0x04700000 | per-core view of shared block |
| `CONFIG_NET_CONFIG_MY_IPV4_ADDR` | 192.0.2.2 | 192.0.2.1 | node addresses |
| `CONFIG_NET_IF_MAX_IPV4_COUNT` | 4 | 4 | multiple IPv4-capable ifaces |
| `CONFIG_NET_IF_UNICAST_IPV4_ADDR_COUNT` | 4 | 4 | same |
| `CONFIG_NET_INTERFACE_NAME` | y | y | name-based SO_BINDTODEVICE |

Tunables in `vnet_shm.h`: `VNET_FRAME_MAX` (1536), `VNET_RING_SLOTS` (8),
`VNET_CACHE_LINE` (32).

### 8.1 MCXN947 (frdm_mcxn947 / mcx_n9xx_evk)

The same app runs on the MCXN947 (Cortex-M33 cpu0 host + Cortex-M33 cpu1
remote). Key differences from RT1180 and the reasoning behind them:

* Shared block address. Unlike RT1180 (where the two cores see HyperRAM at
  different bus addresses), both MCXN947 cores address SRAM identically, so
  host and remote use the *same* `CONFIG_VNET_SHM_ADDR`. The block is placed at
  `0x20060000` = `sramh` (32 KB), which the board devicetree does not assign to
  either core's `zephyr,sram` (cpu0 uses sram0, cpu1 uses sramg). Using an
  unallocated bank means no devicetree carve-out is needed and neither core's
  linker will place data there. `sizeof(struct vnet_shm)` (~24.7 KB) fits in
  the 32 KB bank.

* On-board ENET is intentionally left ENABLED. It is *not* required for the
  cross-core demo, but leaving it enabled preserves the "reach the world" path:
  a bridge/router on cpu0 can later forward vnet0 traffic out of the real
  Ethernet. Disabling `&enet_mac` would make RX buffer starvation "go away" but
  at the cost of that future capability, so we keep it.

* Larger RX/TX pools. The trade-off of keeping ENET enabled is that the real
  Ethernet driver reserves RX `net_pkt` / `net_buf` descriptors from the shared
  pools at init. With the default small pools that starved vnet0's receive
  allocation and produced `RX alloc failed (42 bytes)` on every incoming frame.
  The fix is to size the shared pools to serve BOTH interfaces rather than to
  remove one: on MCXN947 the host conf raises
  `CONFIG_NET_PKT_RX_COUNT`/`TX_COUNT` to 16 and
  `CONFIG_NET_BUF_RX_COUNT`/`TX_COUNT` to 32. This keeps ENET available while
  guaranteeing vnet0 always has buffers for its RX path.

Everything else (roles, MACs 02:..:53:02 / :01, IPs 192.0.2.2 / 192.0.2.1,
NET_IF_MAX_IPV4_COUNT, SO_BINDTODEVICE handling) is identical to RT1180.


--------------------------------------------------------------------------------

## 9. Limitations and future work

* No flow control beyond "drop if ring full"; the ring is sized (8 slots) for a
  low-rate demo. A real bridge would need back-pressure or a larger ring.
* Single frame per slot; no scatter/gather or jumbo frames.
* `VNET_CACHE_LINE` is hard-coded to 32; if targeting a core with a larger line
  size, bump it so `head`/`tail` truly land on separate lines.
* "Reaching the world": run IPv4 forwarding + NAT, or
  `CONFIG_NET_L2_ETHERNET_BRIDGE`, on the CM33 to bridge `vnet0` to the real
  ENETC interface.
