// ASTC LDR decoder (2D blocks) to RGBA8, for GPUs without native ASTC (desktop NVIDIA / AMD).
// Bit-exact with ARM astcenc's 8-bit (decode_unorm8) decode, -dl (srgb = false) / -ds (srgb = true).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// blocks: row-major 16-byte blocks covering ceil(width/bw) x ceil(height/bh).
// rgba_out: width*height*4 bytes, tightly packed rows. Values are the 8-bit UNORM results (no sRGB
// -> linear conversion; srgb only selects the sRGB endpoint expansion, which changes rounding).
// Error / illegal blocks and HDR content decode to magenta (FF 00 FF FF).
void astc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh,
                     bool srgb, uint8_t *rgba_out);

// One block -> bw*bh*4 bytes (rows of bw texels). Returns false for an error block (output magenta).
extern uint64_t astc_stat_error, astc_stat_zero;  // (astc_decode_ldr: error blocks, all-zero ones)
bool astc_decode_block(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t *rgba_out);

// What error blocks decode to (default magenta, FF 00 FF FF, as astcenc - the tests compare with it). The Switch's GPU
// gives transparent black (0 0 0 0) for them: TOTK samples layers of its terrain arrays it never filled - zero
// blocks - and on the console they add nothing; magenta, they tinted its whole first scene (runtime/nvn/vulkan).
extern uint8_t astc_error_rgba[4];
