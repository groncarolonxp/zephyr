# Virtual network device over MBOX + shared memory (RT1180)

A minimal **virtual Ethernet interface** (`vnet0`) that lets the two cores of
the i.MX RT1180 EVK (CM33 host + CM7 remote) run the full Zephyr IP/socket
stack and exchange UDP datagrams across on-chip shared memory. The virtual
device registers as a standard Ethernet interface, so everything above L2
(ARP, IPv4, UDP, TCP, sockets) runs on top of it unchanged.

Two transport modes are selectable at build time:

- **Phase 1 - intra-core loopback (`CONFIG_VNET_MODE_LOOPBACK`).**
  Frames transmitted on `vnet0` are re-injected into RX on the same core. No
  mailbox and no shared memory are touched. Validates the whole networking
  plumbing on a single core.

- **Phase 2 - cross-core over shared memory + MU doorbell (`CONFIG_VNET_MODE_SHM`).**
  This is the active configuration on RT1180. TX frames are copied into a
  shared-memory ring; the peer core is notified through the MBOX (MU) doorbell.
  The peer's doorbell ISR drains its RX ring and hands frames to the stack.

## Files

| File | Purpose |
|------|---------|
| `common/vnet_eth.c`   | Virtual Ethernet driver (both phases) + SHM ring transport. |
| `common/vnet_shm.h`   | Shared-memory ring layout and helpers for Phase 2. |
| `common/vnet_demo.h`  | Public API: declares `vnet_demo_run()` only. |
| `common/vnet_demo.c`  | Demo implementation: iface bring-up + TX/RX threads. |
| `common/Kconfig.vnet` | `CONFIG_VNET*` options, sourced by the apps. |
| `src/main.c`          | Host (CM33) entry point; calls `vnet_demo_run()`. |
| `remote/src/main.c`   | Remote (CM7) entry point; calls `vnet_demo_run()`. |

## RT1180 topology

- **CM33** = main image (`src/`), **CM7** = remote image (`remote/src/`).
- IPC uses the **MU mailbox** (`mbox1_a` / `mbox1_b`). The MU only moves a few
  bytes per channel, so it is used purely as a **doorbell**; the packet payload
  travels through shared memory.
- Shared RAM is **hyperram0**, mapped at `0x1400_0000` from the CM33 and
  `0x0400_0000` from the CM7 (same physical block, different bus views). The
  top 1 MB window holds `struct vnet_shm`.

## Architecture

```
   CM33 (host, role A)                         CM7 (remote, role B)
   ---------------------                        ---------------------
   main() -> vnet_demo_run()                    main() -> vnet_demo_run()
        |                                              |
   socket/UDP over vnet0                         socket/UDP over vnet0
        |                                              |
   vnet_eth.c (Ethernet driver)                 vnet_eth.c (Ethernet driver)
        |   TX: a2b ring  ------------------>   RX: a2b ring   |
        |   RX: b2a ring  <------------------   TX: b2a ring   |
        +---------- MU doorbell (tx/rx) --------------+
                shared block @ hyperram0 top 1 MB
```

The shared block (`struct vnet_shm`) contains two single-producer /
single-consumer rings: `a2b` (host -> remote) and `b2a` (remote -> host). The
host initialises the block once and publishes a magic value; the remote spins
until it sees the magic, then acknowledges. Once the handshake completes each
side raises its virtual carrier so `vnet0` becomes operational.

### Demo threading model (`vnet_demo.c`)

`vnet_demo_run()`:
1. Brings `vnet0` up and ensures this core's IPv4 address lives on `vnet0`.
2. Creates one UDP socket bound to `vnet0`.
3. Spawns a dedicated **RX thread** (`vnet_rx`) doing a blocking `recvfrom()`.
4. Runs the **TX loop** on the calling (main) thread, sending a datagram every
   2 s with an incrementing sequence number.

TX and RX are decoupled into separate threads so the receive path is never
delayed by the send cadence and vice versa.

## Key implementation details / fixes

These are the non-obvious points that make the cross-core path actually work
on this SoC. They are worth understanding before modifying the code.

### 1. Select `vnet0` explicitly, not the default interface

The board also exposes two real ENETC interfaces, and
`net_if_get_default()` returns one of those (cable unplugged -> carrier down).
The demo looks up `vnet0` by device binding and uses it explicitly; sending on
the wrong interface fails with `ENETDOWN` (errno 115).

### 2. `SO_BINDTODEVICE` needs a `struct net_ifreq`

The socket option value must be a `struct net_ifreq` with
`optlen == sizeof(struct net_ifreq)` (name in `ifr_name`). Passing the bare
name string with `optlen == strlen(name)` is rejected with `EINVAL` (errno 22).

### 3. Put the IPv4 address on `vnet0`

`NET_CONFIG` assigns the configured IPv4 address to the default (ENETC)
interface. `net_if_ipv4_addr_lookup(&addr, NULL)` matches on ANY interface, so
a naive check wrongly concludes the address is already placed and never adds it
to `vnet0`. The demo detects which interface owns the address, removes it from
the wrong one, and adds it to `vnet0`. It also needs
`CONFIG_NET_IF_MAX_IPV4_COUNT` / `CONFIG_NET_IF_UNICAST_IPV4_ADDR_COUNT` > 1
(set to 4 in the board confs) because multiple interfaces are IPv4-capable.

### 4. Cache coherency of the shared ring

The shared block is in cacheable HyperRAM, so a `__DMB()` barrier is not enough
for cross-core visibility. `vnet_eth.c` performs explicit cache maintenance
(`sys_cache_data_flush_range()` / `sys_cache_data_invd_range()`):

- Producer: flush the written slot, then flush its own `head`; invalidate the
  peer-owned `tail` before the ring-full check.
- Consumer: invalidate the peer-owned `head`, invalidate each slot before
  reading it, flush the cleared slot and then flush its own `tail`.

### 5. Avoid false sharing of head/tail

`head` (producer-owned) and `tail` (consumer-owned) are placed on **separate
cache lines** in `struct vnet_ring` (see `VNET_CACHE_LINE` padding). Otherwise
a flush/invalidate of one index also writes back or discards the other, so the
two cores clobber each other's index - which showed up as spurious
"TX ring full, dropping frame" after a few packets.

## Build & run

Build and flash both cores via sysbuild from the host board:

```
west build -b mimxrt1180_evk/mimxrt1189/cm33 --sysbuild \
    samples/drivers/vnet
west flash --build-dir build            # host (CM33)
west flash --build-dir build/remote     # remote (CM7)
```

Expected console output (per core, one line every 2 s):

```
Hello from HOST - mimxrt1180_evk/mimxrt1189/cm33
VNET: using interface 3 (vnet0)
VNET: interface is up
VNET: moved 192.0.2.2 off iface 1
VNET: added 192.0.2.2 to vnet0
VNET: socket bound to eth2
VNET: [TX] sent 18 bytes to 192.0.2.1:4242: "hello-from-cm33 #0"
VNET: [RX] received 16 bytes from 192.0.2.1:4242: "hello-from-m7 #0"
VNET: [TX] sent 18 bytes to 192.0.2.1:4242: "hello-from-cm33 #1"
...
```

## Configuration summary (per board .conf)

| Option | CM33 (host) | CM7 (remote) |
|--------|-------------|--------------|
| `CONFIG_VNET_MODE_SHM`   | y | y |
| `CONFIG_VNET_ROLE_HOST`  | y | n |
| `CONFIG_VNET_MAC_LSB`    | 2 | 1 |
| `CONFIG_VNET_SHM_ADDR`   | 0x14700000 | 0x04700000 |
| `CONFIG_NET_CONFIG_MY_IPV4_ADDR` | 192.0.2.2 | 192.0.2.1 |
| `CONFIG_NET_IF_MAX_IPV4_COUNT` | 4 | 4 |
| `CONFIG_NET_INTERFACE_NAME` | y | y |

## Bridging `vnet0` to the real Ethernet (reaching an off-chip PC)

Set `CONFIG_VNET_BRIDGE=y` on the **host** core (see the `frdm_mcxn947` cpu0
`.conf`) to L2-bridge `vnet0` with the on-board Ethernet interface. This lets
the remote core - which only has `vnet0` - reach an off-chip host (e.g. a PC
on the same subnet) through the host core's Ethernet port, and lets that PC
reach either core.

```
   PC (192.0.2.10)
        |  real Ethernet cable
   +----------------------- host core (cpu0) -----------------------+
   |  ENET MAC  <== L2 bridge ==>  vnet0                            |
   |                                 |  shared-memory ring + doorbell|
   +---------------------------------|-----------------------------+
                                     |
                              remote core (cpu1)  vnet0 (192.0.2.1)
```

`common/vnet_bridge.c` finds the bridge interface and the real Ethernet
interface, then `eth_bridge_iface_add()`s both `vnet0` and the ENET MAC to the
bridge. From then on frames are forwarded at L2 between them:

- A frame the remote core sends to the PC crosses the shared ring to the host
  core, is delivered to `vnet0`, the bridge forwards it out of the ENET MAC.
- A frame the PC sends to either core arrives on the ENET MAC and the bridge
  forwards it to `vnet0` (and thus across the ring to the remote core).

### The host core is ALSO a full endpoint

`vnet_bridge_run()` only sets up forwarding; it does not by itself make the
host core a UDP peer. `src/main.c` therefore ALSO calls `vnet_demo_run()` on
the host after the bridge is up, so cpu0 sends to and receives from both the
remote core and the PC.

### Important: bind the host endpoint to the BRIDGE interface, not `vnet0`

Once `vnet0` is added to the bridge it is "enslaved": inbound frames are
consumed by the bridge for forwarding and are **not** delivered to a socket
bound to `vnet0`. The local IP endpoint on a bridging core must therefore live
on the **bridge interface**. `src/main.c` calls
`vnet_demo_set_iface(vnet_bridge_get_iface())` before `vnet_demo_run()` so the
address, `SO_BINDTODEVICE`, and the RX socket all use the bridge interface.
Without this the host core can still transmit (locally originated) but never
receives - which is exactly the symptom of a socket left bound to the enslaved
`vnet0`. On the remote core (no bridge) the endpoint stays on `vnet0`.

## Data path and copies (is the frame copied twice?)

Yes - a datagram crossing from one core to the other is copied more than once.
There is no zero-copy DMA here; the shared ring is a plain memcpy buffer and
the frame is materialised as a `net_pkt` on each side. Per cross-core hop:

**Sender core (`vnet_send` -> `vnet_shm_send` in `vnet_eth.c`):**
1. The stack hands down a `net_pkt`. `net_pkt_read()` copies the linearised
   frame into a local `linebuf` (copy #1).
2. `memcpy(slot->data, linebuf, len)` copies it into the shared-memory ring
   slot, followed by a cache flush so the peer sees it in RAM (copy #2).

**Receiver core (`vnet_rx_work` -> `vnet_deliver`):**
3. `net_pkt_rx_alloc_with_buffer()` + `net_pkt_write()` copies the frame out of
   the ring slot into a freshly allocated `net_pkt` (copy #3), which is then
   passed to `net_recv_data()` and travels up the network stack normally.

So the minimum is **two copies into/out of shared memory plus the network-stack
buffering** (roughly: pkt -> linebuf -> ring on TX, ring -> pkt on RX). When the
host core is also **bridging**, a frame forwarded between `vnet0` and the ENET
MAC additionally goes through the bridge/L2 path (another `net_pkt` handed to
the real driver), and if the host core is itself the endpoint the datagram is
also copied up into its own socket. This is intentional for a minimal, portable
sample: correctness and clarity over throughput. If throughput matters, the
ring slot could be filled directly from the `net_pkt` (dropping the `linebuf`)
and/or the RX side could wrap the slot in a `net_buf` without the extra
`net_pkt_write()` copy.

## Reaching "the world" beyond the local subnet (future work)

The L2 bridge above puts the cores and the PC on the same subnet. To reach
hosts beyond it, add IPv4 forwarding + NAT on the host core (routing between
`vnet0`'s subnet and the upstream network) instead of, or in addition to, the
L2 bridge.


## Notes / limitations

- Each ring is single-producer / single-consumer per direction. Ordering uses
  `__DMB()` plus the explicit cache maintenance described above.
- `VNET_FRAME_MAX` (1536) and `VNET_RING_SLOTS` (8) in `vnet_shm.h` size the
  shared block: `sizeof(struct vnet_shm)` ~= 24.7 KB. Adjust to taste.
- `CONFIG_NET_VLAN` is not required and is left disabled.
