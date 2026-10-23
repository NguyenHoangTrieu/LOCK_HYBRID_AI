/*
 * protocol.h - the SPI wire protocol between the MCXN947 (master) and this
 * ESP32-C5 (slave). Mirrored byte-for-byte on the MCXN947 side in
 * EDGE_NODE/firmware/camera_ai_demo/source/network/protocol.h - keep the
 * two in sync; this is the only file that should ever need editing on
 * both sides at once.
 *
 * *** Why this shape: the board has no handshake/ready GPIO ***
 * LOCK_EDGE_BASEBOARD wires only SCK/MOSI/MISO/CS (ESP32_CS) between the
 * MCUs (confirmed by reading the schematic directly - PCB_DES/
 * LOCK_EDGE_BASEBOARD.kicad_sch, net list: ESP32_CS/SPI_MISO/SPI_MOSI/
 * SPI_SCK are the only 4 nets touching U3). ESP-IDF's spi_slave driver
 * normally expects a dedicated handshake line so the master only clocks
 * when the slave has a receive descriptor actually queued; without one,
 * a transaction that races the slave's queue gets silently dropped or
 * garbled. There is no hardware fix available on this fabricated board,
 * so the protocol instead leans on:
 *   1. Fixed-size, full-duplex transactions (FRAME_SIZE bytes every time,
 *      both directions) - the slave can keep a same-size RX descriptor
 *      permanently queued in a tight loop, which is the one usage
 *      pattern ESP-IDF's spi_slave driver supports reasonably reliably
 *      without a handshake pin.
 *   2. The master polling slowly (tens of ms between transactions, see
 *      SPI_BRIDGE_POLL_INTERVAL_MS on the MCXN947 side) rather than
 *      back-to-back, so the slave has slack to re-queue between
 *      transactions.
 *   3. A sequence number + idempotent request handling, so a dropped or
 *      duplicated transaction degrades to "try again next poll" instead
 *      of silent corruption.
 * This has NOT been verified against real hardware (no two physical
 * boards to test against yet) - treat it as a reasoned design under a
 * real wiring constraint, not a proven-reliable link. If it turns out
 * too fragile in practice, the fix is almost certainly adding a real
 * handshake GPIO to a future board revision, not more protocol cleverness.
 */
#ifndef _NET_BRIDGE_PROTOCOL_H_
#define _NET_BRIDGE_PROTOCOL_H_

#include <stdint.h>

#define PROTO_FRAME_SIZE   256U
#define PROTO_HEADER_SIZE  6U
#define PROTO_PAYLOAD_SIZE (PROTO_FRAME_SIZE - PROTO_HEADER_SIZE) /* 250 bytes */
#define PROTO_MAGIC        0xA5U

#define PROTO_FLAG_MORE  0x01U /* more chunks follow (face image upload) */
#define PROTO_FLAG_ERROR 0x02U

/* Master (MCXN947) -> slave (ESP32) frame types, sent in header byte[1]. */
#define PROTO_CMD_POLL        0x00U /* "anything to report?" / keepalive - no new request */
#define PROTO_CMD_CARD        0x01U /* payload[0..3] = occurred_at unix time, big-endian u32 (CI-5/FR-15: the device's own clock, not the ESP32's); payload[4..] = raw UID bytes, 4-10 bytes */
#define PROTO_CMD_FACE_BEGIN  0x02U /* payload[0..3] = occurred_at unix time, big-endian u32; payload[4..7] = total image size, big-endian u32 */
#define PROTO_CMD_FACE_CHUNK  0x03U /* payload = next slice of JPEG bytes; FLAG_MORE set until the last chunk */
#define PROTO_CMD_GET_CONFIG     0x04U
#define PROTO_CMD_HEARTBEAT      0x05U
#define PROTO_CMD_CARDSTORE_GET  0x06U /* FR-51/53: payload[0..1] = requested chunk index, big-endian u16 (0 first) */
#define PROTO_CMD_ENROLL_CARD    0x07U /* FR-48: payload = raw UID bytes, 4-10 bytes - only meaningful with an active card-registration session (see PROTO_RSP_HEARTBEAT_OK's session fields) */

#define PROTO_CARD_UID_OFFSET 4U /* see PROTO_CMD_CARD above */

/* FR-39: mode reported in every heartbeat (CI-4). */
#define PROTO_MODE_ATTENDANCE 0U
#define PROTO_MODE_ENROLLMENT 1U

/* FR-48: result of a PROTO_CMD_ENROLL_CARD submission. */
#define PROTO_ENROLL_RESULT_ASSIGNED            0U
#define PROTO_ENROLL_RESULT_ALREADY_REGISTERED  1U
#define PROTO_ENROLL_RESULT_NEEDS_CONFIRMATION  2U /* UI-21: admin must resolve on the web app (UI-25) */
#define PROTO_ENROLL_RESULT_ERROR               3U

/* Slave (ESP32) -> master frame types, sent in header byte[1]. Valid in
 * every transaction (full duplex) - reflects whatever the slave's
 * background state is, not necessarily a reply to the master's frame in
 * the SAME transaction (see file header: the master polls until it sees
 * the response it's waiting for, or its own timeout). */
#define PROTO_RSP_IDLE          0x00U /* nothing new; last request (if any) already delivered */
#define PROTO_RSP_BUSY          0x01U /* a request is being processed (HTTP round trip in flight) */
#define PROTO_RSP_DECISION      0x02U /* payload: granted(u8) unlock_duration_sec(u8) name_len(u8) name[name_len] */
#define PROTO_RSP_CONFIG        0x03U /* payload: see proto_config_payload_t below */
#define PROTO_RSP_HEARTBEAT_OK  0x04U /* payload: see proto_heartbeat_payload_t below (CI-4/CI-6/FR-39/FR-53: mode, enrollment session, and card_store_version ride along on the same reply) */
#define PROTO_RSP_CARDSTORE_DATA 0x06U /* payload: see proto_cardstore_payload_t below */
#define PROTO_RSP_ENROLL_RESULT  0x07U /* payload[0] = PROTO_ENROLL_RESULT_*; if NEEDS_CONFIRMATION, payload[1]=holder_name_len, payload[2..]=holder_name */
#define PROTO_RSP_WIFI_DOWN     0xFEU /* no WiFi/backend link - maps directly to NET_IsOnline()==false */
#define PROTO_RSP_ERROR         0xFFU

#pragma pack(push, 1)
typedef struct
{
    uint8_t  magic;       /* PROTO_MAGIC */
    uint8_t  type;        /* PROTO_CMD_* or PROTO_RSP_* depending on direction */
    uint8_t  seq;         /* increments per logical request on the master side; echoed back */
    uint8_t  flags;       /* PROTO_FLAG_* */
    uint16_t payload_len; /* big-endian, 0..PROTO_PAYLOAD_SIZE, valid bytes in payload[] */
    uint8_t  payload[PROTO_PAYLOAD_SIZE];
} proto_frame_t;

typedef struct
{
    uint8_t  granted;
    uint8_t  unlock_duration_sec;
    uint8_t  name_len;
    char     name[31]; /* not NUL-terminated on the wire - name_len is authoritative */
} proto_decision_payload_t;

typedef struct
{
    uint16_t recognition_threshold_x1000; /* big-endian; e.g. 600 == 0.600 */
    uint8_t  unlock_duration_sec;
    uint16_t cooldown_sec; /* big-endian */
    uint8_t  request_timeout_sec;
    uint8_t  offline_card_access_enabled; /* 0/1 */
} proto_config_payload_t;

/* FR-41/FR-54a: carried in every PROTO_CMD_HEARTBEAT request so the
 * server (and therefore the admin UI, FR-43) knows this node's firmware
 * version and local card_store_version without a separate round trip. */
typedef struct
{
    uint32_t card_store_version; /* big-endian */
    uint8_t  fw_version_len;
    char     fw_version[16]; /* not NUL-terminated; len authoritative */
} proto_heartbeat_request_payload_t;

/* FR-39/CI-4, FR-45 (enrollment session), FR-52/CI-6 (card_store_version
 * target) - everything a node needs to react to a server-side change,
 * all delivered on the heartbeat it was already sending (there is no
 * other channel the server could push through, see file header). */
typedef struct
{
    uint32_t server_time;              /* big-endian, CI-5 clock sync */
    uint8_t  mode;                     /* PROTO_MODE_* */
    uint32_t card_store_version;       /* big-endian - server's current target (FR-52) */
    uint8_t  session_active;           /* 0/1 - only meaningful when mode == PROTO_MODE_ENROLLMENT */
    uint8_t  session_user_name_len;
    char     session_user_name[24];    /* not NUL-terminated; len authoritative */
    uint16_t session_expires_in_sec;   /* big-endian */
} proto_heartbeat_payload_t;

/* FR-51: one assigned card. `active` is carried for fidelity with
 * DR-9/FR-51's wording even though, as of this change, the server
 * (cards.list_assigned_for_sync()) only ever includes already-active
 * holders in this feed - so every entry the node receives today has
 * active==1. user_id is deliberately NOT carried: the node never needs
 * it (events are reported by UID, FR-10b; the name is what's shown,
 * UI-13), and leaving it out keeps this struct and PROTO_CARDSTORE_MAX_
 * ENTRIES_PER_CHUNK's per-chunk budget smaller. */
typedef struct
{
    uint8_t uid_len;
    uint8_t uid[10];
    uint8_t active; /* 0/1 */
    uint8_t name_len;
    char    name[24]; /* not NUL-terminated; name_len authoritative */
} proto_card_entry_t;

#define PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK 6U
/* Bounds the total card list this protocol will sync in one go
 * (64 chunks * 6 entries = 384 cards) - below ASM-3/PR-8's 2,000-card
 * target (OI-6 already flags local flash capacity as open; this
 * protocol-level cap is a second, independent limit on top of that).
 * If a site needs more, this needs a real incremental-sync design, not
 * a bigger number here. */
#define PROTO_CARDSTORE_MAX_CHUNKS 64U

typedef struct
{
    uint32_t version;      /* big-endian */
    uint16_t total_chunks; /* big-endian */
    uint16_t chunk_index;  /* big-endian - which chunk this frame carries */
    uint8_t  entry_count;  /* valid entries in this chunk, 0..PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK */
    proto_card_entry_t entries[PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK];
} proto_cardstore_payload_t; /* 9 + 6*37 = 231 bytes, fits PROTO_PAYLOAD_SIZE (250) */
#pragma pack(pop)

static inline uint16_t proto_be16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t proto_be32(uint32_t v)
{
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) | ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24);
}

#endif /* _NET_BRIDGE_PROTOCOL_H_ */
