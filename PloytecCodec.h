/* PloytecCodec.h – Ploytec bit-swizzle codec for Xone:4D
 * MIT License. Pure C, no kernel deps — usable in DriverKit or user-space.
 *
 * encode_frame: 24 bytes packed S24_3LE PCM (8 channels) → 48 bytes wire format
 * decode_frame: 64 bytes wire format → 24 bytes packed S24_3LE PCM
 */
#ifndef PloytecCodec_h
#define PloytecCodec_h

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ploytec_encode_frame(uint8_t *dest, const uint8_t *src);
void ploytec_decode_frame(uint8_t *dest, const uint8_t *src);

#ifdef __cplusplus
}
#endif

#endif
