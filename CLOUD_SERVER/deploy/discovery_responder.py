#!/usr/bin/env python3
"""discovery_responder.py - answers the Edge Node firmware's UDP broadcast
discovery request (EDGE_NODE/firmware/esp32_net_bridge/main/backend_discovery.c)
with this host's current backend port, so the device can find the backend
without knowing its IP in advance (DHCP can and does reassign it - see
CLOUD_SERVER/README.md). Standard library only, no dependency on the
FastAPI app itself - this is a separate tiny process, run alongside it
(see lock-hybrid-discovery.service).

mDNS was tried first and ruled out: multicast between WiFi stations on
the target AP was confirmed not to be forwarded (a raw-socket probe from
one station never saw a query sent from another), while plain UDP
broadcast and plain unicast both crossed fine. This is a minimal
broadcast-request/unicast-reply protocol instead of anything multicast-
based - no mDNS/Bonjour/Avahi involved.
"""
import argparse
import socket

DISCOVERY_REQUEST = "LOCK_HYBRID_DISCOVER_V1"
DISCOVERY_REPLY_PREFIX = "LOCK_HYBRID_BACKEND_V1"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=47623, help="UDP port to listen on for discovery requests")
    parser.add_argument("--backend-port", type=int, default=8000, help="port to report as the backend's own port")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    print(f"discovery_responder: listening on UDP {args.port}, reporting backend port {args.backend_port}")

    while True:
        data, addr = sock.recvfrom(1024)
        if data.decode(errors="replace").strip() != DISCOVERY_REQUEST:
            continue
        reply = f"{DISCOVERY_REPLY_PREFIX} PORT={args.backend_port}"
        sock.sendto(reply.encode(), addr)
        print(f"discovery_responder: replied to {addr}")


if __name__ == "__main__":
    main()
