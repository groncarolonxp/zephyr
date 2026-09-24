# eth_ipc data path: how it works, and pbuf vs virtio

Scope: the shared-memory data path of `drivers/ethernet/eth_ipc.c` after the
"drop the RX staging copy" change. Answers two questions:

1. How does a frame actually move between the two cores?
2. Would virtio be a better transport than pbuf?

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
driver no longer hand-rolls cache maintenance.

## 3. Frame walkthrough

### TX (`eth_ipc_send` -> `eth_ipc_fifo_send`)

```
  net_pkt (fragment chain)
      |  (1) net_pkt_read()          <-- linearise, 1 copy
      v
  data->tx_buf[1536]                 <-- private, under tx_lock
      |  (2) pbuf_write()            <-- frame + length header, 1 copy
      v
  shared data[]  ---- cache flush ---+
      |                              |
      +--> mbox_send_dt() ---------> doorbell to peer
```

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
the `net_buf` the stack will parse. The staging buffer `rx_buf` is now only
used on the fallback path where `net_pkt_rx_alloc_with_buffer()` returned a
*chained* pkt (small `CONFIG_NET_BUF_DATA_SIZE`), because then the payload is
not contiguous and `net_pkt_write()` has to walk the fragments.

### Copy count

| Path | before | after |
|---|---|---|
| TX | 2 (`net_pkt_read`->scratch, `pbuf_write`->ring) | **1** |
| RX | 2 (`pbuf_read`->scratch, `net_pkt_write`->net_pkt) | **1** (contiguous frag) |

One copy per direction is the **pbuf floor**, not a shortcoming of this
driver - see below.

## 4. Would virtio be better than pbuf?

Short answer: **not for this use case, and not in this tree.** Four reasons,
all verified against the actual source.

### 4.1 The copy would not go away

The in-tree virtio-net driver has the *same* two-copy shape this change just
removed from eth_ipc:

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
shared memory, which is the same work required to make the pbuf (or the old
bespoke ring) zero-copy. Virtio does not hand you that for free.

### 4.2 No cache maintenance - a correctness blocker here

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
padding of the three rings - i.e. reintroducing exactly the hand-rolled cache
logic that moving to pbuf eliminated, except now across a much larger and more
intricate shared structure (descriptor table + avail + used, each with its own
ownership rules).

pbuf already solves this, and solves it in upstream-maintained code.

### 4.3 There is no device (backend) side

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

### 4.4 Asymmetric roles and feature negotiation we do not need

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

## 5. If zero-copy becomes a hard requirement

The remaining copy is the ring copy, and removing it is orthogonal to the
choice of pbuf vs virtio. It requires:

1. A fixed-slot ring (contiguous, cache-line aligned, indexable), not a
   wrapping byte FIFO - a frame must never straddle the wrap. This is what
   commit `d4b7623` had.
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
