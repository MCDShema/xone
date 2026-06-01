/* ploytec_defs.h – Ploytec USB Protocol Constants for Xone:4D HAL plugin
 * Ported verbatim from the DriverKit driver. Pure C, user-space safe.
 * MIT License.
 */
#pragma once
#include <stdint.h>

/* ── USB IDs ──────────────────────────────────────────────────────────── */
#define PLOYTEC_VENDOR_ID        0x0A4A
#define PLOYTEC_PID_XONE_4D      0xFF4D

/* ── Endpoint addresses (as presented on the wire) ───────────────────── */
#define PLOYTEC_EP_PCM_OUT       0x05   /* High-BW Interrupt OUT */
#define PLOYTEC_EP_PCM_IN        0x86   /* High-BW Interrupt IN  */
#define PLOYTEC_EP_MIDI_IN       0x83   /* Interrupt IN          */

/* wIndex values for SET_CUR sample-rate vendor requests */
#define PLOYTEC_EP_RATE_IN       0x0086
#define PLOYTEC_EP_RATE_OUT      0x0005

/* ── Vendor command bytes ─────────────────────────────────────────────── */
#define PLOYTEC_CMD_FIRMWARE     0x56  /* 'V'  bmRequestType=0xC0, read 15 bytes */
#define PLOYTEC_CMD_STATUS       0x49  /* 'I'  read (0xC0) or write (0x40)       */
#define PLOYTEC_CMD_SET_RATE_REQ 0x01
#define PLOYTEC_CMD_SET_RATE_TYPE 0x22
#define PLOYTEC_CMD_GET_RATE_REQ 0x81
#define PLOYTEC_CMD_GET_RATE_TYPE 0xA2

/* wIndex register index for CMD_STATUS */
#define PLOYTEC_REG_AJ_INPUT_SEL 0

/* ── PCM topology ────────────────────────────────────────────────────── */
#define PLOYTEC_CHANNELS         8
#define PLOYTEC_OUT_FRAME_SIZE   48   /* bytes per device output frame */
#define PLOYTEC_IN_FRAME_SIZE    64   /* bytes per device input frame  */
#define PLOYTEC_FRAMES_PER_PKT   80   /* audio frames per USB packet   */

/* Packet sizes (interrupt mode, Xone:4D) */
#define PLOYTEC_INT_OUT_PKT_SIZE 3856  /* 8 sub-packets × 482 bytes */
#define PLOYTEC_INT_IN_PKT_SIZE  5120  /* 10 sub-packets × 512 bytes */

/* ── USB interface / alt-setting ─────────────────────────────────────── */
#define PLOYTEC_NUM_INTERFACES   2
#define PLOYTEC_ALT_SETTING_IDLE 0
#define PLOYTEC_ALT_SETTING_AUDIO 1

/* ── MIDI ────────────────────────────────────────────────────────────── */
#define PLOYTEC_MIDI_IDLE_BYTE   0xFD

/* ── Sample-rate helpers (3-byte little-endian) ─────────────────────── */
static inline void ploytec_encode_rate(uint32_t rate, uint8_t buf[3]) {
    buf[0] = (uint8_t)(rate & 0xFF);
    buf[1] = (uint8_t)((rate >> 8) & 0xFF);
    buf[2] = (uint8_t)((rate >> 16) & 0xFF);
}

/* Compute wValue for the confirm-status write-back (sets bit5, sign-extends) */
static inline uint16_t ploytec_confirm_wvalue(uint8_t status) {
    int8_t modified = (int8_t)(status | 0x20);  /* set MODE5 bit */
    return (uint16_t)(int16_t)modified;
}
