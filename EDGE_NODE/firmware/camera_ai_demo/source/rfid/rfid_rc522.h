/*
 * rfid_rc522.h - MFRC522 (RFID-RC522) card reader, shared LPSPI1 bus.
 *
 * Satisfies SRS HI-2/FR-9: reads the UID of an ISO/IEC 14443 Type A card
 * (REQA -> anticollision -> select, single-card only - no cascade levels
 * beyond a 4-byte single-size UID). Makes no access decision itself
 * (FR-10) - RFID_Poll() only ever reports a UID, callers decide what to
 * do with it.
 *
 * CS/RST/IRQ pins are already muxed and initialized by
 * BOARD_InitBaseboardPeripheralPins() (pin_mux.c) - this driver only
 * drives them at runtime, via app.h's DEMO_RFID_* macros, same pattern as
 * lcd_spi_hw.c/sd_spi_disk.c on this shared bus (see spi1_bus.h).
 */
#ifndef _RFID_RC522_H_
#define _RFID_RC522_H_

#include <stdbool.h>
#include <stdint.h>

#define RFID_UID_MAX_LEN 10U /* covers single (4B), double (7B) and triple (10B) size UIDs */

typedef struct
{
    uint8_t uid[RFID_UID_MAX_LEN];
    uint8_t length; /* 4, 7 or 10 - 0 if no card */
} rfid_uid_t;

/*! @brief Reset + configure the MFRC522 and turn the antenna on. Call once
 *  after SPI1_BUS_Init(). Returns false if the chip doesn't answer (bad
 *  wiring/no module) - callers should treat that like the SD card's
 *  "log once, keep going" pattern, not a hard fault. */
bool RFID_Init(void);

/*! @brief Poll for a card. Non-blocking, meant to be called once per main
 *  loop iteration alongside the camera/AI poll (FR-8: card and face
 *  monitoring run concurrently). Returns true and fills `outUid` exactly
 *  once per card presentation - the same card held in the field will not
 *  report again until it has been removed and re-presented (REQA only
 *  succeeds against an IDLE card; a selected card is HALTed after each
 *  successful read so the next REQA from the same physical card needs a
 *  field power-cycle or removal to go IDLE again, which is the standard
 *  ISO14443A behavior this relies on rather than software debouncing). */
bool RFID_Poll(rfid_uid_t *outUid);

#endif /* _RFID_RC522_H_ */
