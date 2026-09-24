# eth_ipc data path: how it works, and pbuf vs virtio

Scope: the shared-memory data path of `drivers/ethernet/eth_ipc.c`. Answers
three questions:

1. How does a frame actually move between the two cores?
2. What are the link lifecycle and reset assumptions?
3. Would virtio be a better transport than pbuf?

---

## 1. System view

Two cores on one SoC (e.g. MCXN947 cpu0/cpu1, RT1180 cm33/cm7) share a block
of RAM plus an MU/MBOX doorbell. Each core runs a full, unmodified Zephyr IP
stack on top of a standard `ETH_NET_DEVICE`; only `.send` and the RX path know
about shared memory.

```
      CORE A  (role = "host")              CORE B  (role = "remote")
 +-----------------------------+      +-----------------------------+
 | sockets / IP / Ethernet L2  |      | sockets / IP / Ethernet L2  |
 |           net_pkt           |      |           net_pkt           |
 +-----------------------------+      +-----------------------------+
 |       eth_ipc driver        |      |       eth_ipc driver        |
 |  .send           rx_work    |      |  .send           rx_work    |
 +----|----------------^-------+      +----|----------------^-------+
      |                |                   |                |
      | pbuf_write     | pbuf_read         | pbuf_write     | pbuf_read
      |                |                   |                |
      |        +-------+-------------------|----------------+
      |        |                           |
      v        |                           v
 +=========================================================================+
 |   SHARED MEMORY BLOCK  (devicetree "reg", possibly NON-coherent)        |
 |                                                                        |
 |   +----------------------------------------------------------------+   |
 |   |  a2b region :  rd_idx | handshake | wr_idx | data[]            |   |
 |   |                A writes  ------------------>  B reads          |   |
 |   +----------------------------------------------------------------+   |
 |   |  b2a region :  rd_idx | handshake | wr_idx | data[]            |   |
 |   |                B writes  ------------------>  A reads          |   |
 |   +----------------------------------------------------------------+   |
 +=========================================================================+

   MBOX doorbell (MU):   A --- tx_db ---> B        B --- tx_db ---> A
                         (signal only, carries no payload)
```

The block is split in half: one unidirectional `struct pbuf` per direction.
Each region is single-producer / single-consumer, so no cross-core lock is
needed - only the two indices, which pbuf owns.

## 2. Inside one pbuf region

```
  +----------+------------+ ... +----------+-------------------------------+
  |  rd_idx  | handshake  | pad |  wr_idx  |          data[]               |
  | (reader  | (connect   |     | (writer  |  length-framed messages,      |
  |  owns)   |  word)     |     |  owns)   |  wraps at the end             |
  +----------+------------+-----+----------+-------------------------------+
  |<-- cache-line aligned ----->|<- own cache line ->|

  data[] holds:  [len:4][payload ...][len:4][payload ...] ...
```

Why this layout matters on this hardware: `rd_idx` and `wr_idx` sit on
**separate cache lines**. The writer flushes only `wr_idx`, the reader
invalidates only `wr_idx` and flushes only `rd_idx`. If they shared a line,
flushing one index would write back a stale copy of the other and the two
cores would clobber each other's position - the classic false-sharing bug that
shows up as "TX ring full" forever. pbuf does all of this internally
(`sys_cache_data_*` + `barrier_sync_synchronize`), which is exactly why this
driver does not hand-roll any cache maintenance of its own.

## 3. Frame walkthrough

### TX (`eth_ipc_send` -> `eth_ipc_fifo_send`)

```
  net_pkt
      |
      +-- contiguous?  frag->len == len
      |         |
      |         |  yes: pbuf_write(frag->data)   <-- 1 copy, DIRECT
      |         v
      |   shared data[]
      |
      +-- no (chained net_pkt):
                |  (1) net_pkt_read() -> tx_buf  <-- linearise
                |  (2) pbuf_write(tx_buf)        <-- 1 copy
                v
          shared data[]
                |
                +--> mbox_send_dt() -----------> doorbell to peer
```

pbuf_write() flushes both the payload and its own write index, so the
driver performs no cache maintenance itself.

The fast path is the common case: with `CONFIG_NET_BUF_DATA_SIZE` >= 1536 a
full-MTU frame lands in a single fragment, so `tx_buf` is never touched.
`frag->len == len` (not `>=`) is the test that proves the whole frame lives
in that one fragment - `len` is the sum over the chain.

`tx_lock` serialises producers only; pbuf is single-producer.

### RX (doorbell ISR -> `rx_work` -> `eth_ipc_rx_one`)

```
  doorbell ISR --> k_work_submit()   (nothing else in ISR context)
      v
  rx_work:  loop while eth_ipc_rx_one() consumed something
      |
      |  (a) pbuf_read(pb, NULL, 0)        <-- PEEK length, consumes nothing
      |  (b) net_pkt_rx_alloc_with_buffer(plen)
      |  (c) pbuf_read(pb, net_buf_tail(frag), plen)
      |          shared data[] ---> net_buf  <-- 1 copy, DIRECT
      |      net_buf_add() + net_pkt_cursor_init()
      v
  net_recv_data()  --> IP stack parses in place
```

The peek-then-allocate ordering is the whole trick: because the length is
known *before* the `net_pkt` exists, `pbuf_read()` can be pointed straight at
the `net_buf` the stack will parse. The staging buffer `rx_buf` is only used
on the fallback path where `net_pkt_rx_alloc_with_buffer()` returned a
*chained* pkt (small `CONFIG_NET_BUF_DATA_SIZE`), because then the payload is
not contiguous and `net_pkt_write()` has to walk the fragments.

### Copy count

| Path | copies per frame |
|---|---|
| TX | **1** when the frame is in one fragment; 2 via `tx_buf` for chained net_pkts |
| RX | **1** when the net_buf is contiguous; 2 via `rx_buf` for chained net_pkts |

One copy per direction is the **pbuf floor**, not a shortcoming of this
driver - see below.

## 4. Lifecycle: connect, start/stop, and reset assumptions

### Connect

The host initialises both pbuf regions and publishes `"VNET"` in pbuf's
handshake word; the remote polls that word until it appears. This makes the
link boot-order independent - whichever core comes up first simply waits.

The poll runs on a per-instance background thread rather than delayed work,
because the remote may block for an unbounded time and parking that on the
system workqueue would stall every other work item. The thread runs once and
exits as soon as the link is up.

By default (`CONFIG_ETH_IPC_CONNECT_TIMEOUT_MS=0`) the remote waits
**forever**. That is intentional for two cores of one SoC: there is no
meaningful "give up" when the peer is simply a core that has not been
released from reset yet. Set the option non-zero if the peer may
legitimately never boot; the thread then logs an error and exits, and the
interface stays down with the carrier never raised.

### start / stop

A virtual link has no MAC to clock-gate, and the MBOX doorbell must stay
armed even while the interface is down - the peer is a separate core that
keeps producing, and if we stopped draining its FIFO it would fill and wedge
the link permanently (pbuf has no flow control beyond "full").

So `.start` / `.stop` set a `running` flag that gates the data path rather
than tearing down the transport:

| state | TX | RX |
|---|---|---|
| started | normal | delivered to the stack |
| stopped | refused with `-ENETDOWN` | drained from the FIFO, then discarded |

Observable behaviour therefore matches a real interface (no traffic in or out
while stopped), without letting a stopped interface on one core block the
other core.

### Reset assumptions (v1 limitation)

`connected` is latched true after the handshake and never cleared. There is
deliberately **no reconnect path** in this version, and the design assumes:

- Both cores boot once and stay up for the lifetime of the system.
- Neither core resets independently of the other.
- The shared block is not re-initialised behind a running peer's back.

If one core *does* reset on its own, the surviving side keeps its stale view:
its pbuf indices no longer match the peer's freshly zeroed ones, and the link
will mis-frame rather than recover. Recovering properly needs a sequence or
epoch number in the handshake word so each side can detect that the peer
restarted, drop the carrier, re-run `pbuf_*_init()` and re-handshake. That is
future work; today a peer reset requires resetting both sides.

## 5. Would virtio be better than pbuf?

Short answer: **not for this use case, and not in this tree.** Four reasons,
all verified against the actual source.

### 5.1 The copy would not go away

The in-tree virtio-net driver stages every frame through a static buffer in
both directions, i.e. it pays the same two copies that eth_ipc avoids:

```c
/* drivers/ethernet/eth_virtio_net.c */
uint8_t txb[VIRTIO_NET_BUFLEN];                       /* static staging */
uint8_t rxb[CONFIG_ETH_VIRTIO_NET_RX_BUFFERS][...];

net_pkt_read(pkt, data->txb + sizeof(hdr), len);      /* TX copy */
net_pkt_write(pkt, &data->rxb[buf_no][sizeof(hdr)], len); /* RX copy */
```

Virtqueues *could* be zero-copy in principle, since a descriptor is just
`{addr, len}` and could point at a `net_buf` payload. But that is not a
property of virtio - it is a property of putting the net_buf data pool in
shared memory, which is the same work required to make a pbuf-based path
zero-copy. Virtio does not hand you that for free.

### 5.2 No cache maintenance - a correctness blocker here

```
$ grep -rn "sys_cache\|DCACHE" drivers/virtio/virtqueue.c \
      drivers/virtio/virtio_mmio.c drivers/virtio/virtio_common.c
(no matches)
```

The virtqueue implementation assumes a **coherent** view of the descriptor
table, avail ring and used ring. That holds for its intended target (QEMU /
PCI / an MMU-managed host) but **not** for two Cortex-M cores sharing
non-coherent SRAM or HyperRAM, which is precisely the eth_ipc target. Adopting
virtio would mean adding per-descriptor flush/invalidate plus cache-line
padding of the three rings - i.e. hand-rolling exactly the cache logic that
pbuf already provides, except now across a much larger and more intricate
shared structure (descriptor table + avail + used, each with its own
ownership rules).

pbuf already solves this, and solves it in upstream-maintained code.

### 5.3 There is no device (backend) side

```
$ ls drivers/virtio/
virtio_common.c  virtio_mmio.c  virtio_pci.c  virtqueue.c
```

All of this is the **driver** (guest) half: it *consumes* a virtio device over
PCI or MMIO. Both transports gate on discovering real hardware:

```
config VIRTIO_PCI   depends on DT_HAS_VIRTIO_PCI_ENABLED
config VIRTIO_MMIO  depends on DT_HAS_VIRTIO_MMIO_ENABLED
```

For a core-to-core link, *one of the two cores must be the device* - it would
have to publish a config space, service the avail ring, populate the used
ring, and raise the used-buffer notification. **None of that exists in
Zephyr.** We would have to write a virtio device backend from scratch. That is
a far bigger effort than the whole eth_ipc driver, for no copy saving.

### 5.4 Asymmetric roles and feature negotiation we do not need

Virtio brings config space layout, feature-bit negotiation, device status
handshake, and `virtio-net`'s own 10/12-byte header. eth_ipc's requirement is
"move an Ethernet frame to the other core and ring a bell". The pbuf handshake
word already covers boot-order independence (host publishes `"VNET"`, remote
spins until it sees it).

### Verdict

| | pbuf (current) | virtio |
|---|---|---|
| Copies per direction | 1 | 1 (as implemented in tree) |
| Non-coherent-memory safe | yes, built in | **no** - would need new cache logic |
| Device/backend side exists | n/a (symmetric) | **no** - must be written |
| Upstream maintained | yes | driver side only |
| Code in this driver | ~2 calls | large new subsystem |

pbuf is the right call. It is the cheapest transport that is *correct* on
non-coherent shared memory, and it is symmetric so both cores run identical
code.

## 6. If zero-copy becomes a hard requirement

The remaining copy is the ring copy, and removing it is orthogonal to the
choice of pbuf vs virtio. It requires:

1. A fixed-slot ring (contiguous, cache-line aligned, indexable), not a
   wrapping byte FIFO - a frame must never straddle the wrap.
2. The `net_buf` **data pool placed in the shared block** (linker section),
   so the payload the stack writes is already visible to the peer.
3. **RX**: `net_pkt_alloc_on_iface()` + `net_buf_alloc_with_data(slot->data)`
   + `net_pkt_append_buffer()`, and release the slot from the pool's `destroy`
   callback - the stack unrefs out of order, so the consumer index has to
   become a per-slot released bitmap rather than a single tail.
4. **TX**: only frames allocated from that shared pool are zero-copy;
   bridged/forwarded frames come from the generic pool and still need the
   copy, so a fallback must stay.

Net result would be 0 copies on the fast path, 1 on the fallback. Worth noting
that `eth_ivshmem` - the upstream driver this one mirrors - stops at 1 copy
each way for exactly these reasons.
