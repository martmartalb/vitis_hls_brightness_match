#include <iostream>
#include <cstdint>

#include "ap_int.h"
#include "ap_fixed.h"
#include "hls_stream.h"
#include "ap_axi_sdata.h"

// ----------------------------------------------------------------------------
// Must match the DUT. TB_NUM_FRAMES is defined here BEFORE including nothing —
// the DUT reads it via its own #ifndef guard, so define it on the compile line
// or keep it identical here for the testbench's own loops.
// ----------------------------------------------------------------------------
#define IMG_WIDTH    1920
#define IMG_HEIGHT   1080
#define PPC          2
#define BITS_PER_CH  8
#define NUM_CH       3
#define PIXEL_BITS   (BITS_PER_CH * NUM_CH)
#define WORD_BITS    (PIXEL_BITS * PPC)
#define TOTAL_PIXELS (IMG_WIDTH * IMG_HEIGHT)
#define TOTAL_WORDS  (TOTAL_PIXELS / PPC)
#define Q_MEAN       28
#define K_MEAN       129
#define TARGET_MEAN  128

#ifndef TB_NUM_FRAMES
#define TB_NUM_FRAMES 1
#endif

typedef ap_fixed<10, 9>  offset_t;
typedef ap_axiu<WORD_BITS, 1, 1, 1> axi_pixel_t;
typedef hls::stream<axi_pixel_t>    axi_stream_t;

// DUT
void brightness_match(axi_stream_t &in_stream, axi_stream_t &out_stream);

// ----------------------------------------------------------------------------
// Two-zone frame:
//   Left  half  (x < W/2): DARK_VAL on all channels
//   Right half  (x >= W/2): BRIGHT_VAL on all channels
//   Expected mean = (DARK_VAL + BRIGHT_VAL) / 2
// ----------------------------------------------------------------------------
#define DARK_VAL    50
#define BRIGHT_VAL  200

// ----------------------------------------------------------------------------
// Reference model — mirrors the HLS multiply-shift exactly
// ----------------------------------------------------------------------------
static uint8_t ref_mean_approx(uint32_t acc) {
    uint64_t product = (uint64_t)acc * (uint64_t)K_MEAN;
    return (uint8_t)(product >> Q_MEAN);
}
static int16_t ref_offset(uint8_t mean) {
    return (int16_t)TARGET_MEAN - (int16_t)mean;
}
static uint8_t ref_clamp(int16_t v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}
static uint8_t expected_pixel(uint8_t in_val, int16_t offset) {
    return ref_clamp((int16_t)in_val + offset);
}

// ----------------------------------------------------------------------------
// Pack two RGB pixels into one 48-bit AXI word
//   pixel 0 -> bits [23:0], pixel 1 -> bits [47:24]
// ----------------------------------------------------------------------------
static axi_pixel_t make_word(uint8_t v0, uint8_t v1, bool last, bool sof) {
    axi_pixel_t w;
    w.data( 7,  0) = v0;  w.data(15,  8) = v0;  w.data(23, 16) = v0;  // pixel 0 (B,G,R)
    w.data(31, 24) = v1;  w.data(39, 32) = v1;  w.data(47, 40) = v1;  // pixel 1 (B,G,R)
    w.last = last ? 1 : 0;
    w.user = sof  ? 1 : 0;   // tuser marks start of frame
    w.keep = 0x3F;
    w.strb = 0x3F;
    w.id   = 0;
    w.dest = 0;
    return w;
}

// ----------------------------------------------------------------------------
// Push one two-zone frame into the stream
// ----------------------------------------------------------------------------
static void push_frame(axi_stream_t &s) {
    int word_idx = 0;
    for (int y = 0; y < IMG_HEIGHT; y++) {
        for (int x = 0; x < IMG_WIDTH; x += PPC) {
            bool is_last = (word_idx == TOTAL_WORDS - 1);
            bool is_sof  = (word_idx == 0);
            uint8_t v0 = (x       < IMG_WIDTH / 2) ? DARK_VAL : BRIGHT_VAL;
            uint8_t v1 = ((x + 1) < IMG_WIDTH / 2) ? DARK_VAL : BRIGHT_VAL;
            s.write(make_word(v0, v1, is_last, is_sof));
            word_idx++;
        }
    }
}

// ----------------------------------------------------------------------------
// Drain one frame, collect a few sample pixels and an error count vs expected
// offset.  Returns number of mismatches.
// ----------------------------------------------------------------------------
static int check_frame(axi_stream_t &s, int16_t expected_offset,
                       const char *label) {
    int errors = 0;
    int word_idx = 0;
    // Track sample outputs
    int dark_sample = -1, bright_sample = -1;

    for (int i = 0; i < TOTAL_WORDS; i++) {
        axi_pixel_t w = s.read();
        int x_base = (word_idx % (IMG_WIDTH / PPC)) * PPC;

        for (int p = 0; p < PPC; p++) {
            int x = x_base + p;
            int base = p * PIXEL_BITS;
            uint8_t r = (uint8_t)w.data(base + 23, base + 16).to_uint();

            uint8_t in_val   = (x < IMG_WIDTH / 2) ? DARK_VAL : BRIGHT_VAL;
            uint8_t expected = expected_pixel(in_val, expected_offset);

            if (r != expected) {
                if (errors < 5)
                    std::cout << "    mismatch x=" << x
                              << " got=" << (int)r
                              << " exp=" << (int)expected << "\n";
                errors++;
            }
            if (x < IMG_WIDTH / 2)  dark_sample   = r;
            else                    bright_sample = r;
        }
        word_idx++;
    }

    std::cout << "  " << label
              << ": offset=" << expected_offset
              << "  dark_out=" << dark_sample
              << "  bright_out=" << bright_sample
              << (errors == 0 ? "  PASS\n" : "  FAIL\n");
    return errors;
}

// ----------------------------------------------------------------------------
int main() {
    axi_stream_t in_stream, out_stream;
    int errors = 0;

    // Reference offset for a two-zone frame
    uint32_t acc_ref = (uint32_t)(IMG_WIDTH / 2) * IMG_HEIGHT * DARK_VAL
                     + (uint32_t)(IMG_WIDTH / 2) * IMG_HEIGHT * BRIGHT_VAL;
    uint8_t  ref_mean = ref_mean_approx(acc_ref);
    int16_t  ref_off  = ref_offset(ref_mean);

    std::cout << "============================================================\n";
    std::cout << " brightness_match testbench\n";
    std::cout << "============================================================\n";
    std::cout << " input: dark=" << DARK_VAL << " bright=" << BRIGHT_VAL << "\n";
    std::cout << " ideal mean=" << (DARK_VAL + BRIGHT_VAL) / 2
              << "  approx mean=" << (int)ref_mean
              << "  offset=" << ref_off << "\n\n";

    // Push exactly TB_NUM_FRAMES frames
    for (int f = 0; f < TB_NUM_FRAMES; f++)
        push_frame(in_stream);

    // Single call — DUT's bounded for-loop consumes all frames
    brightness_match(in_stream, out_stream);

    // Frame 0: initial offset = 0 -> pass-through
    errors += check_frame(out_stream, 0, "frame 0 (expect pass-through)");
    // Frame 1: offset from frame 0
    errors += check_frame(out_stream, ref_off, "frame 1 (expect corrected)");
    // Frame 2: offset from frame 1 (same pattern -> same as frame 1)
    if (TB_NUM_FRAMES >= 3)
        errors += check_frame(out_stream, ref_off, "frame 2 (expect corrected)");

    std::cout << "\n============================================================\n";
    std::cout << (errors == 0 ? " RESULT: PASS\n" : " RESULT: FAIL\n");
    std::cout << "============================================================\n";
    return (errors == 0) ? 0 : 1;
}