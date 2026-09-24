#!/usr/bin/env python3
# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0
#
# vnet PC-side test helper.
#
# Point-to-point setup: the board's real Ethernet is cabled directly to this
# PC. Give the PC the address 192.0.2.10/24 on that interface, e.g. (Linux):
#
#     sudo ip addr add 192.0.2.10/24 dev eth0
#     sudo ip link set eth0 up
#
# or use the companion helper:
#
#     sudo ./tools/vnet_pc_setup.sh 192.0.2.10 eth0
#
# Then run this script. By default it behaves symmetrically with the board
# cores - it just sends and receives, it does NOT echo:
#   1. Periodically sends "hello-from-x86 #N" to BOTH board cores - cpu0
#      (192.0.2.2, host) and cpu1 (192.0.2.1, remote), and
#   2. Listens on UDP :4242 and prints any datagram it receives.
# Pass --echo to additionally reflect received datagrams back to the sender.
#
# Usage:
#   ./vnet_pc_echo.py                        # send+recv (symmetric), no echo
#   ./vnet_pc_echo.py --echo                 # also echo received datagrams
#   ./vnet_pc_echo.py --probe 192.0.2.2      # probe only the given target(s)
#   ./vnet_pc_echo.py --probe 192.0.2.2 --probe 192.0.2.1
#   ./vnet_pc_echo.py --no-probe             # listen only, no probing

#
# IMPORTANT (read tools/README.md): with the sample as-is, a core is only
# reachable from the PC if that core owns the real Ethernet AND a bridge is
# running between vnet0 and ENET. See the README for what "reach core 1"
# actually requires.

import argparse
import socket
import sys
import threading



UDP_PORT = 4242

# Default board cores to probe: cpu0 (host) and cpu1 (remote).
DEFAULT_PROBES = ["192.0.2.2", "192.0.2.1"]


def main():
    ap = argparse.ArgumentParser(description="vnet PC-side echo/probe helper")
    ap.add_argument("--bind", default="192.0.2.10",
                    help="local address to bind (default 192.0.2.10)")
    ap.add_argument("--port", type=int, default=UDP_PORT,
                    help="UDP port (default 4242)")
    ap.add_argument("--probe", metavar="BOARD_IP", action="append",
                    help="probe this board IP (repeatable). If omitted, "
                         "probes cpu0 (192.0.2.2) and cpu1 (192.0.2.1)")
    ap.add_argument("--no-probe", action="store_true",
                    help="do not send any probes, only listen/echo")
    ap.add_argument("--echo", action="store_true",
                    help="echo received datagrams back to the sender. Off by "
                         "default so the PC behaves symmetrically with the "
                         "cores: it only sends its own periodic probes and "
                         "prints what it receives (no echo: reflections)")

    ap.add_argument("--interval", type=float, default=2.0,
                    help="seconds between periodic probe cycles (default 2.0)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    print(f"[PC] listening on {args.bind}:{args.port} (UDP)")

    # Periodically send "hello-from-x86 #N" to each board core (cpu0 and cpu1),
    # mirroring what the cores do to each other and to the PC. A daemon thread
    # keeps this going for the life of the process; the main thread stays on
    # the recv/echo loop. This is why the cores now see PC-originated traffic
    # continuously rather than just once at startup.
    stop = threading.Event()

    def probe_loop():
        targets = args.probe if args.probe else DEFAULT_PROBES
        seq = 0
        while not stop.is_set():
            for target in targets:
                msg = f"hello-from-x86 #{seq}".encode()
                sock.sendto(msg, (target, args.port))
                print(f"[PC] [TX] sent {len(msg)} bytes to "
                      f"{target}:{args.port}: {msg.decode()!r}")

            seq += 1
            stop.wait(args.interval)

    if not args.no_probe:
        t = threading.Thread(target=probe_loop, daemon=True)
        t.start()

    print("[PC] waiting for datagrams (Ctrl+C to stop) ...")

    try:
        while True:
            data, addr = sock.recvfrom(2048)
            text = data.decode(errors="replace")
            print(f"[PC] [RX] {len(data)} bytes from {addr[0]}:{addr[1]}: "
                  f"{text!r}")
            if args.echo:
                reply = b"echo:" + data

                sock.sendto(reply, addr)
                print(f"[PC] [TX] echoed {len(reply)} bytes to "
                      f"{addr[0]}:{addr[1]}")
    except KeyboardInterrupt:
        print("\n[PC] bye")
        return 0


if __name__ == "__main__":
    sys.exit(main())
