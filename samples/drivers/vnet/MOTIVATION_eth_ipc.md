# Why `eth_ipc.c` is a Needed Addition (and not a copy of `eth_ivshmem.c`)

## TL;DR

`drivers/ethernet/eth_ipc.c` and `drivers/ethernet/eth_ivshmem.c` are **not**
copies of each other. They share only the unavoidable Zephyr driver contract
for a shared-memory network device (register an `ETH_NET_DEVICE`, override the
`.send`/RX paths, move whole frames through shared memory, and notify the peer
with a doorbell). Everything below that thin skeleton — the target hardware,
the transport, the synchronization model, and, most importantly, the memory
coherency handling — is fundamentally different.

`eth_ipc.c` targets a scenario that `eth_ivshmem.c` **structurally cannot
serve**: two cores on a single NXP SoC sharing a block of possibly
non-coherent RAM and signalling each other over an MBOX / MU mailbox. That is
the whole point of the `vnet` sample.

---

## What the two drivers have in common

These similarities are inherent to *any* shared-memory netdev in Zephyr; they
are the driver contract, not evidence of copy/paste:

- Both register as a standard Zephyr Ethernet device, so the full IP / socket
  stack runs unchanged on top.
- Both move complete Ethernet frames through a shared-memory region and use a
  doorbell to notify the peer.
- Both handle MAC-address setup, carrier on/off, and RX delivery via
  `net_recv_data()`.

Function names, data structures, threading model, and synchronization are all
distinct between the two files.

---

## Key differences

| Aspect          | `eth_ivshmem.c`                                             | `eth_ipc.c`                                                                    |
|-----------------|------------------------------------------------------------|-------------------------------------------------------------------------------|
| Target          | QEMU **inter-VM** shared memory (ivshmem-v2 over PCI)      | On-SoC **dual-core** shared memory (e.g. RT1180, MCXN947)                     |
| Doorbell        | ivshmem / PCI interrupt (`ivshmem_int_peer`)               | MBOX / MU mailbox (`mbox_send_dt`)                                            |
| Transport       | virtio-style vrings (`eth_ivshmem_queue_*`)                | small bespoke single-producer / single-consumer (SPSC) ring                  |
| Cache handling  | none (coherent PCI shared memory)                          | explicit flush/invalidate for **non-coherent** memory; head/tail on separate cache lines; `__DMB()` barriers |
| State machine   | 4-state RESET/INIT/READY/RUN handshake driven by ivshmem peer-state registers | simple magic/version handshake in a background connect thread                |
| RX model        | dedicated kernel thread + `k_poll` signal                  | doorbell ISR -> workqueue drain                                              |
| Config source   | devicetree phandle to an ivshmem device                    | devicetree `reg` + `mboxes` + `role` property                                |

---

## Why writing `eth_ipc.c` is necessary and adds value

### 1. There is no ivshmem on real silicon
ivshmem is a QEMU / PCI virtualization construct. Bare-metal NXP dual-core SoCs
(RT1180 CM7 + CM33, MCXN947 dual Cortex-M) have no PCI bus and no ivshmem
device. The entire `ivshmem_*` dependency chain (`CONFIG_IVSHMEM`, PCI
enumeration, ivshmem-v2 protocol negotiation) is simply unavailable on these
parts, so `eth_ivshmem.c` cannot be instantiated at all.

### 2. Non-coherent shared memory is the real engineering content
Two Cortex-M cores sharing SRAM or HyperRAM are typically **not**
cache-coherent. `eth_ivshmem.c` assumes coherent shared memory and performs no
cache maintenance; running it against non-coherent inter-core RAM would corrupt
both the data frames and the ring indices.

`eth_ipc.c`'s distinctive value is precisely the coherency discipline:

- Explicit `sys_cache_data_flush_range()` / `sys_cache_data_invd_range()`
  around every producer/consumer access to the shared block.
- `head` and `tail` control words placed on **separate cache lines** to avoid
  false sharing: flushing/invalidating one index's line must not clobber the
  other core's index (this is what otherwise manifests as spurious
  "TX ring full").
- `__DMB()` barriers to order slot-content publication before the index
  advance.

### 3. MBOX/MU is the natural inter-core doorbell
On these NXP parts the Messaging Unit (MU), exposed through Zephyr's MBOX API,
is the idiomatic core-to-core signalling primitive — not a PCI interrupt.
`eth_ipc.c` uses `mbox_send_dt()` / `mbox_register_callback_dt()` and is
configured entirely from devicetree (`reg`, `mboxes`, `role`), with no Kconfig
addresses required.

### 4. It is lighter and bridge-ready
`eth_ipc.c` uses a tiny SPSC ring instead of a full virtio vring
implementation, reducing code size and complexity for the on-SoC use case. It
also advertises `ETHERNET_PROMISC_MODE` and implements `set_config`, so the
interface can be added to a Zephyr Ethernet bridge — which is exactly what the
`vnet` sample needs.

---

## TX-path hardening applied

While reviewing the driver, two TX-path issues were identified and fixed (each
in its own commit):

1. **Reentrancy.** The `.send` path originally staged the outgoing frame in a
   function-scope `static uint8_t linebuf[]`, shared across all callers and all
   instances. Two concurrent `.send()` calls could overwrite each other's frame
   between `net_pkt_read()` and the ring copy. This was replaced with a
   per-instance staging buffer guarded by a per-instance `k_mutex` (`tx_lock`).

2. **Double copy.** The TX path copied each frame twice (`net_pkt_read()` into
   the staging buffer, then `memcpy()` into the shared ring slot). The frame is
   now read straight into the ring slot with a single copy — mirroring how
   `eth_ivshmem.c` reads directly into its shared queue buffer — and the
   intermediate buffer was removed. `tx_lock` still serialises the producer-side
   slot claim and head advance, so the path remains reentrant.

---

## Bottom line

`eth_ipc.c` is a legitimate, purpose-built sibling of `eth_ivshmem.c`, not a
copy. It serves the non-coherent, on-SoC, dual-core-with-MBOX scenario that
`eth_ivshmem.c` cannot address, and its core value is the cache-coherency
handling and MBOX doorbell integration. Keeping both drivers is justified.
