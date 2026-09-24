.. zephyr:code-sample:: vnet
   :name: Virtual network device (cross-core)
   :relevant-api: mbox_interface ethernet_api net_if

   Run the Zephyr IP/socket stack over a virtual Ethernet interface that
   bridges two cores through shared memory and an MBOX doorbell.

Overview
********

This sample brings up a minimal virtual Ethernet interface (``vnet0``) so two
cores of a multi-core SoC can run the full Zephyr networking stack (Ethernet
L2, ARP, IPv4, UDP, sockets) and exchange UDP datagrams with each other.

Two transport modes are selectable at build time:

* **Loopback** (``CONFIG_VNET_MODE_LOOPBACK``): frames transmitted on ``vnet0``
  are re-injected into RX on the same core. No mailbox and no shared memory are
  used. This validates the whole socket stack on a single core.

* **Shared memory** (``CONFIG_VNET_MODE_SHM``): TX frames are copied into a
  shared-memory ring and the peer core is notified through the MBOX (MU)
  doorbell. The peer drains its RX ring and hands the frames to the stack. The
  MU only carries the doorbell; the packet payload travels through shared RAM.

On the i.MX RT1180 EVK the CM33 is the host (role A) and the CM7 is the remote
(role B). Each core sends a periodic UDP datagram to the other and prints both
the transmitted and received messages.

A detailed design and implementation-notes document lives next to the sources
in ``common/README_vnet.md``.

Building and Running
********************

The sample is built with sysbuild so the host and remote images are produced
together. For the i.MX RT1180 EVK:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/vnet/
   :board: mimxrt1180_evk/mimxrt1189/cm33
   :goals: build
   :west-args: --sysbuild

Flash both cores:

.. code-block:: console

   west flash --build-dir build            # host (CM33)
   west flash --build-dir build/remote     # remote (CM7)

Open a serial terminal (minicom, putty, etc.) on each core's console with the
following settings:

- Speed: 115200
- Data: 8 bits
- Parity: None
- Stop bits: 1

Expected output on the host (CM33) core:

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   Hello from HOST - mimxrt1180_evk/mimxrt1189/cm33
   VNET: using interface 3 (vnet0)
   VNET: interface is up
   VNET: added 192.0.2.2 to vnet0
   VNET: socket bound to vnet0
   VNET: [TX] sent 18 bytes to 192.0.2.1:4242: "hello-from-cm33 #0"
   VNET: [RX] received 16 bytes from 192.0.2.1:4242: "hello-from-m7 #0"
   VNET: [TX] sent 18 bytes to 192.0.2.1:4242: "hello-from-cm33 #1"
   ...

And the mirror on the remote (CM7) core:

.. code-block:: console

   *** Booting Zephyr OS build ... ***
   Hello from REMOTE - mimxrt1180_evk/mimxrt1189/cm7
   VNET: using interface 3 (vnet0)
   VNET: interface is up
   VNET: added 192.0.2.1 to vnet0
   VNET: socket bound to vnet0
   VNET: [TX] sent ... to 192.0.2.2:4242: "hello-from-mimxrt1180_evk/mimxrt1189/cm7 #0"
   VNET: [RX] received ... from 192.0.2.2:4242: "hello-from-mimxrt1180_evk/mimxrt1189/cm33 #0"
   ...

Each core tags its payload with its own ``CONFIG_BOARD_TARGET`` so you can
tell which core (and SoC) a datagram came from.

Two ways to use the sample
**************************

1. Internal only (no PC, default)
   Both cores just exchange UDP datagrams with each other over the
   shared-memory link. This is the default build - nothing extra is needed:

   .. code-block:: console

      west build -b mimxrt1180_evk/mimxrt1189/cm33 --sysbuild samples/drivers/vnet
      # or:  -b frdm_mcxn947/mcxn947/cpu0
      west flash --build-dir build
      west flash --build-dir build/remote

   Each core prints ``[TX]`` and ``[RX]`` lines showing the round trip.

2. Reach an off-chip PC (host bridge)
   To let the remote core reach a PC on the wire, the host core bridges
   ``vnet0`` with the on-board Ethernet (``CONFIG_VNET_BRIDGE``) and the
   remote core targets the PC (``CONFIG_VNET_DEMO_PEER_ADDR``). On the
   MCXN947 board files these are already set (host bridges, remote sends to
   192.0.2.10). Then on the PC:

   .. code-block:: console

      sudo ip addr add 192.0.2.10/24 dev eth0     # your wired NIC
      sudo ip link set eth0 up
      python3 samples/drivers/vnet/tools/vnet_pc_echo.py

   The helper listens on UDP 4242, prints every datagram received, and echoes
   it back; the echo appears on the remote core console as an ``[RX]`` line.
   Use ``--probe 192.0.2.1`` / ``--probe 192.0.2.2`` to actively poke a core.
   See ``tools/README.md`` for the full explanation and troubleshooting.

Including the vnet device in your own application
*************************************************

The virtual Ethernet device and the demo helpers live in ``common/`` and are
self-contained. To reuse them:

* add ``common/vnet_eth.c`` (the driver), ``common/vnet_shm.h`` (ring layout)
  and, for the demo, ``common/vnet_demo.c`` to your app's ``target_sources``;
  add ``common/`` to ``target_include_directories``;
* source ``common/Kconfig.vnet`` from your app ``Kconfig`` and select the
  options you need (``CONFIG_VNET``, ``CONFIG_VNET_MODE_SHM`` /
  ``CONFIG_VNET_MODE_LOOPBACK``, ``CONFIG_VNET_ROLE_HOST``,
  ``CONFIG_VNET_SHM_ADDR``, ``CONFIG_VNET_MAC_LSB``);
* provide an ``mbox-consumer`` devicetree node with ``tx``/``rx`` channels and
  place ``struct vnet_shm`` in shared RAM (see the board overlays here);
* call ``vnet_demo_run(self_ip, peer_ip, tag)`` from ``main()``, or just open
  a normal BSD socket on the ``vnet0`` interface - it is a standard Ethernet
  device. For the L2 bridge add ``common/vnet_bridge.c`` and select
  ``CONFIG_VNET_BRIDGE`` (host only).

See ``common/DESIGN_vnet.md`` for the full theory of operation.

