/*
 * backend_discovery.h - UDP broadcast discovery of the Cloud Backend's
 * current IP on the local network, so a DHCP-reassigned server address
 * doesn't require re-provisioning this device (see backend_client.c's
 * NVS-override mechanism for the other half of that same problem - the
 * device's own address/credentials surviving a move).
 *
 * mDNS was tried first and ruled out: multicast (224.0.0.251:5353)
 * measured as not forwarded between WiFi stations on the target AP
 * (confirmed with a raw socket probe - a listener on one station never
 * saw a query sent from another, even though plain unicast and plain
 * broadcast both crossed fine between the same two stations). Plain UDP
 * broadcast is the mechanism that was actually confirmed to work on this
 * network, so that's what this uses - IP_MULTICAST semantics are not
 * involved at all.
 */
#ifndef _BACKEND_DISCOVERY_H_
#define _BACKEND_DISCOVERY_H_

#include <stddef.h>
#include <stdbool.h>

/*! @brief Broadcasts a discovery request on
 *  CONFIG_LOCK_HYBRID_BACKEND_DISCOVERY_PORT and waits for the backend's
 *  responder (CLOUD_SERVER/deploy/discovery_responder.py) to answer with
 *  its current port. On success, writes "http://<replying-ip>:<port>"
 *  into outBaseUrl (NUL-terminated, truncated to outSize) and returns
 *  true. Returns false (outBaseUrl untouched) on timeout/malformed
 *  reply/socket error - the caller already has a fallback (the
 *  Kconfig-compiled or NVS-stored static URL), this is only ever a
 *  best-effort override of that. Blocks for up to roughly 2 seconds. */
bool BACKEND_DISCOVERY_Resolve(char *outBaseUrl, size_t outSize);

#endif /* _BACKEND_DISCOVERY_H_ */
