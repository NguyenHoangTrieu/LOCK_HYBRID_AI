/*
 * card_store.h - FR-10a/FR-51: the local card store an Attendance-mode
 * node decides card access from, without a server round trip.
 *
 * Lives on the same SD-over-SPI FatFs volume as snapshot.c (CON-9: the
 * only persistent, non-volatile medium actually wired into this
 * project - there is no on-chip NVM/flash driver anywhere in this
 * codebase to build on instead, see WORKLOG.md). FR-51 literally says
 * "internal flash"; this is a deliberate, documented substitution, not
 * an oversight.
 *
 * That SD path has a known, still-open, non-deterministic write
 * reliability problem (see EDGE_NODE/WORKLOG.md's SD write-failure
 * investigation - writes degrade over a session, FR_DISK_ERR observed
 * on real hardware). This module is written so that failure mode is
 * never a security hazard: SR-9 requires a store that fails
 * verification to be treated as completely empty (deny every card),
 * and FR-54 requires an update to be atomic (old-or-new, never a mix) -
 * both are satisfied by the same mechanism (write to a temp file,
 * verify, rename over the old one; any failure anywhere leaves
 * whatever was already loaded in RAM, from the previous successful
 * load, untouched). Worst case from a flaky card is therefore "cards
 * stop working until the next successful sync", never an incorrect
 * grant.
 *
 * Does NOT call f_mount() itself - FatFs only supports one FATFS object
 * bound per volume at a time, and snapshot.c already mounts volume ""
 * in SNAPSHOT_Init(). CARD_STORE_Init() must be called after that (see
 * main_edge_node.c).
 */
#ifndef _CARD_STORE_H_
#define _CARD_STORE_H_

#include <stdbool.h>
#include <stdint.h>

#define CARD_STORE_UID_MAX_LEN  10U
#define CARD_STORE_NAME_MAX_LEN 24U
/* Deliberately well below protocol.h's wire-level cap (384 =
 * PROTO_CARDSTORE_MAX_CHUNKS * PROTO_CARDSTORE_MAX_ENTRIES_PER_CHUNK) -
 * this is the in-RAM/on-disk budget. Measured, not guessed: a first
 * attempt at 128 entries built and linked at m_data 99.12% (build/
 * camera_ai_demo_cm33_core0.elf, EDGE_NODE_BUILD=ON) - technically fits,
 * but leaves only ~3.7KB of slack in a 412KB region for everything else
 * (stacks, AI buffers, ...), which is not a safe margin on a chip this
 * tight on RAM (see WORKLOG.md's dual-core entries for the same
 * m_data-near-100% pattern elsewhere in this project). 32 entries
 * measured at ~97.2% instead - still tight, but with real headroom.
 * Raise this only after checking the real build percentage again, not
 * by guessing a bigger number feels fine. The sync path
 * (network_client_esp32.c) must pass this as its own cap, not the
 * wire-level one, so a server with more cards than this is truncated
 * loudly (logged), not silently. */
#define CARD_STORE_MAX_ENTRIES 32U

typedef struct
{
    uint8_t uidLen;
    uint8_t uid[CARD_STORE_UID_MAX_LEN];
    uint8_t nameLen;
    char    name[CARD_STORE_NAME_MAX_LEN];
} card_store_entry_t;

typedef struct
{
    bool    found;
    char    name[CARD_STORE_NAME_MAX_LEN + 1U]; /* NUL-terminated, ready for display (UI-13) */
} card_store_lookup_result_t;

/*! @brief Call once at boot, after SNAPSHOT_Init(). Loads and verifies
 *  whatever is on disk; a missing SD card, missing file, or a CRC/size
 *  mismatch all result in an empty store (SR-9), logged but not fatal -
 *  RFID/servo/display keep working, cards just don't until a sync
 *  succeeds. */
void CARD_STORE_Init(void);

/*! @brief FR-10a: the entire local card decision. O(n) linear scan over
 *  whatever is currently loaded in RAM - fine at ASM-3/PR-8's scale
 *  (hundreds to a couple thousand cards), and simple enough to be
 *  obviously correct, which matters more than raw speed for an
 *  access-control decision. */
bool CARD_STORE_Lookup(const uint8_t *uid, uint8_t uidLen, card_store_lookup_result_t *outResult);

/*! @brief The version currently held (RAM, backed by what's on disk) -
 *  0 if the store is empty/unverified. Reported in each heartbeat
 *  (FR-54a) so the admin UI can show which nodes are stale. */
uint32_t CARD_STORE_GetVersion(void);

/*! @brief FR-53/FR-54: atomic full replace. On success, both the on-disk
 *  file and the in-RAM copy reflect `entries`/`version`; on any failure
 *  (SD error, verification failure, too many entries) neither changes -
 *  the previous store keeps being used exactly as if this call had
 *  never happened. */
bool CARD_STORE_WriteAtomic(uint32_t version, const card_store_entry_t *entries, uint16_t count);

#endif /* _CARD_STORE_H_ */
