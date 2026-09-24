# Testing vnet reachability from a PC

Question: the board is cabled point-to-point to my PC (PC = 192.0.2.10/24).
Would a UDP message from a core reach my PC, and would my reply reach the core?

## Short answer

Not with the sample exactly as shipped. Today each core's UDP socket is bound
to the *virtual* interface `vnet0`, and `vnet0` only carries frames between the
two cores through shared memory. Nothing in the sample copies frames between
`vnet0` and the real on-board Ethernet (ENET). So:

* A datagram a core sends to `192.0.2.10` is handed to `vnet0` and delivered to
  the *other core*, not out of the ENET port. It never reaches the PC.
* A datagram the PC sends arrives on the core that owns ENET, but there is no
  code moving it onto `vnet0`/the socket, so the app does not see it.

In other words the cores talk to each other over `vnet0`; the PC is on a
different L2 segment (the ENET wire). They are not bridged yet.

## What is needed to actually reach the PC (and get a reply back to a core)

You need an L2/L3 relay on the core that owns ENET (cpu0/host here). Two
options, both listed as future work in `common/DESIGN_vnet.md`:

1. L2 bridge: enable `CONFIG_NET_L2_ETHERNET_BRIDGE` and bridge `vnet0` with the
   ENET interface. Then vnet0 and ENET are one broadcast domain; a core's frame
   to 192.0.2.10 egresses ENET to the PC, and the PC's reply is bridged back.

2. L3 forwarding + NAT: give ENET and vnet0 different subnets, enable IPv4
   forwarding on cpu0, and route/NAT between them.

Either way the host core (cpu0) acts as the gateway; the remote core (cpu1)
reaches the PC *through* cpu0.

Also make the addressing consistent: if the PC is 192.0.2.10/24 and the cores
are 192.0.2.2 (cpu0) and 192.0.2.1 (cpu1), they are already in the same /24, so
an L2 bridge (option 1) is the least-effort path - no routing/NAT needed.

## How to test with the provided script

On the PC, put the wire into the 192.0.2.0/24 subnet and run the helper:

```
sudo ip addr add 192.0.2.10/24 dev eth0    # replace eth0 with your NIC
sudo ip link set eth0 up

python3 tools/vnet_pc_echo.py              # listen + echo on UDP :4242
```

`vnet_pc_echo.py`:

* listens on UDP `:4242` and prints every datagram it receives (this tells you
  whether a core's message reached the PC), and
* echoes each datagram back to its sender (this exercises the reply path back
  toward a core), and
* can also actively probe a board address:

```
python3 tools/vnet_pc_echo.py --probe 192.0.2.2   # probe cpu0 (host)
python3 tools/vnet_pc_echo.py --probe 192.0.2.1   # probe cpu1 (remote)
```

### Interpreting the result

* Bridge/forwarding NOT set up (sample as-is): the script prints nothing (no
  `[RX]` lines) and `--probe` gets no reply - confirming the cores are not yet
  reachable from the PC.
* Bridge/forwarding IS set up on cpu0: you will see `[PC] [RX] ... from
  192.0.2.2` (host core) and, once cpu1 traffic is bridged/routed through cpu0,
  from `192.0.2.1` (remote core). The echo/`--probe` reply then shows up on the
  corresponding core's console as a `[RX] received ... from 192.0.2.10` line.

## Quick connectivity sanity checks from the PC

```
ping 192.0.2.2            # only answers if ARP/ICMP reaches that core
sudo tcpdump -ni eth0 udp port 4242   # watch what actually hits the wire
```

`tcpdump` is the definitive check: if you see the core's UDP frames on the wire
you have egress working; if you also see them answered, the return path works.
