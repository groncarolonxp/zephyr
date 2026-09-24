.. zephyr:code-sample:: ipc-eth-bridge
   :name: IPC Ethernet host-side bridge (trampoline)
   :relevant-api: ethernet

   Bridge the IPC virtual Ethernet with the real on-board Ethernet so a stock
   networking app on the other core can reach an off-chip host.

Overview
********

This sample demonstrates the ``zephyr,ipc-ethernet`` driver: a virtual
Ethernet interface (``vnet0``) that carries frames between two cores of the
same SoC through a shared-memory ring, signalling the peer with an MBOX
doorbell. Because it registers as a standard Ethernet device, the whole IP /
socket stack runs on top of it unchanged.

The point of this sample is the *split*:

* The **remote core** runs a completely stock networking application (for
  example :zephyr:code-sample:`sockets-echo`) with **no source changes** - it
  only needs a devicetree overlay that instantiates a ``zephyr,ipc-ethernet``
  node with ``role = "remote"``.

* The **host core** runs this tiny *trampoline*: it does not run a socket app
  at all. It only L2-bridges ``vnet0`` with the on-board real Ethernet
  (``CONFIG_NET_ETHERNET_BRIDGE``) so the remote core's traffic can reach an
  off-chip host (e.g. a PC), and vice-versa. Its ``zephyr,ipc-ethernet`` node
  uses ``role = "host"``.

All the "magic" (shared-memory ring, doorbell, handshake, cache maintenance,
carrier) lives in the driver (``drivers/ethernet/eth_ipc.c``), not in either
application.

Building
********

Host core (this sample, the bridge/trampoline):

.. code-block:: console

   west build -b frdm_mcxn947/mcxn947/cpu0 samples/net/ipc_eth_bridge

Remote core (stock echo sample, unmodified sources, using the provided
board overlay + conf):

.. code-block:: console

   west build -b frdm_mcxn947/mcxn947/cpu1 samples/net/sockets/echo

Devicetree
**********

Each core's overlay instantiates one ``zephyr,ipc-ethernet`` node. ``reg`` is
the shared block as seen from that core, ``mboxes`` the doorbell channels, and
``role`` selects host vs remote. See the ``boards/`` overlays in this sample
and in ``samples/net/sockets/echo``.
