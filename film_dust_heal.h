#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Healing Brush: RGBA8 source/destination, one-byte mask. Returns 0 on success.
int32_t fdh_heal_rgba8(uint32_t width, uint32_t height,
    const uint8_t* source_rgba, const uint8_t* destination_rgba,
    const uint8_t* mask, uint8_t* output_rgba);
// Automatic Remove: RGBA8 image, one-byte painted mask, RGBA8 output.
// PhotoCraft patch-based completion; closes loops and expands the mask by 2 px.
int32_t fdh_remove_rgba8(uint32_t width, uint32_t height,
    const uint8_t* image_rgba, const uint8_t* mask,
    uint8_t* output_rgba);
#ifdef __cplusplus
}
#endif
