#include <iostream>
#include <cstdint>
#include "ap_int.h"
#include "hls_stream.h"
#include "ap_axi_sdata.h"

#define PPC         2
#define BITS_PER_CH 8
#define NUM_CH      3
#define PIXEL_BITS  (BITS_PER_CH * NUM_CH)
#define WORD_BITS   (PIXEL_BITS * PPC)

#define TARGET_MEAN 128
#define Q_MEAN      28   // must match the IP

#ifndef TB_NUM_FRAMES
#define TB_NUM_FRAMES 3
#endif

// Small image for fast csim; IP is resolution-agnostic via tlast/tuser
#define IMG_WIDTH    8
#define IMG_HEIGHT   4
#define TOTAL_PIXELS (IMG_WIDTH * IMG_HEIGHT)
#define TOTAL_WORDS  (TOTAL_PIXELS / PPC)

// Two-zone test pattern: left half dark, right half bright
#define DARK_VAL    50
#define BRIGHT_VAL  200

typedef ap_axiu<WORD_BITS, 1, 1, 1> axi_pixel_t;
typedef hls::stream<axi_pixel_t>    axi_stream_t;

void brightness_match(axi_stream_t &in_stream, axi_stream_t &out_stream,
                      ap_uint<32> k_mean);

// Reference model — mirrors IP arithmetic exactly (same multiply-shift, no division)
static uint8_t ref_mean_approx(uint32_t acc, uint32_t k) {
    uint64_t product = (uint64_t)acc * (uint64_t)k;
    return (uint8_t)(product >> Q_MEAN);
}
static int16_t ref_offset(uint8_t mean) {
    return (int16_t)TARGET_MEAN - (int16_t)mean;
}
static uint8_t ref_clamp(int16_t v) {
    if (v <   0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}
static uint8_t expected_pixel(uint8_t in_val, int16_t offset) {
    return ref_clamp((int16_t)in_val + offset);
}

// Pack two gray pixels (R=G=B=v) into one AXI word.
// Byte order per pixel: B at [7:0], G at [15:8], R at [23:16].
static axi_pixel_t make_word(uint8_t v0, uint8_t v1, bool last, bool sof) {
    axi_pixel_t w;
    w.data( 7,  0) = v0;  w.data(15,  8) = v0;  w.data(23, 16) = v0;   // pixel 0
    w.data(31, 24) = v1;  w.data(39, 32) = v1;  w.data(47, 40) = v1;   // pixel 1
    w.last = last ? 1 : 0;
    w.user = sof  ? 1 : 0;   // tuser = start-of-frame
    w.keep = 0x3F;
    w.strb = 0x3F;
    w.id   = 0;
    w.dest = 0;
    return w;
}

// Push one two-zone frame: tuser=1 on first word, tlast=1 on last word.
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

// Drain one frame and verify each pixel against expected_offset.
static int check_frame(axi_stream_t &s, int16_t expected_offset,
                       const char *label) {
    int errors = 0;
    int dark_sample = -1, bright_sample = -1;

    for (int word_idx = 0; word_idx < TOTAL_WORDS; word_idx++) {
        axi_pixel_t w = s.read();
        int x_base = (word_idx % (IMG_WIDTH / PPC)) * PPC;

        for (int p = 0; p < PPC; p++) {
            int x    = x_base + p;
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
            if (x < IMG_WIDTH / 2) dark_sample   = r;
            else                   bright_sample = r;
        }
    }

    std::cout << "  " << label
              << ": offset=" << expected_offset
              << "  dark_out=" << dark_sample
              << "  bright_out=" << bright_sample
              << (errors == 0 ? "  PASS\n" : "  FAIL\n");
    return errors;
}

int main() {
    axi_stream_t in_stream, out_stream;
    int errors = 0;

    // k_mean = floor(2^Q_MEAN / N), the value that would come from BRAM at runtime
    uint32_t k_mean = (1u << Q_MEAN) / TOTAL_PIXELS;

    // Reference offset using the same multiply-shift as the IP
    uint32_t acc_ref = (uint32_t)(IMG_WIDTH / 2) * IMG_HEIGHT * DARK_VAL
                     + (uint32_t)(IMG_WIDTH / 2) * IMG_HEIGHT * BRIGHT_VAL;
    uint8_t  ref_m   = ref_mean_approx(acc_ref, k_mean);
    int16_t  ref_off = ref_offset(ref_m);

    std::cout << "============================================================\n";
    std::cout << " brightness_match testbench\n";
    std::cout << "============================================================\n";
    std::cout << " image: " << IMG_WIDTH << "x" << IMG_HEIGHT
              << "  dark=" << DARK_VAL << "  bright=" << BRIGHT_VAL << "\n";
    std::cout << " k_mean=" << k_mean << "  Q_MEAN=" << Q_MEAN << "\n";
    std::cout << " approx mean=" << (int)ref_m << "  offset=" << ref_off << "\n\n";

    // One call per frame — matches hardware where ap_start fires once per frame.
    // Static offset registers in the DUT persist across calls.
    for (int f = 0; f < TB_NUM_FRAMES; f++) {
        push_frame(in_stream);
        brightness_match(in_stream, out_stream, k_mean);
    }

    // Frame 0: offset registers start at 0 → pass-through
    errors += check_frame(out_stream, 0, "frame 0 (pass-through)");
    // Frame 1: offset computed from frame 0's mean
    errors += check_frame(out_stream, ref_off, "frame 1 (corrected)");
    // Frame 2: same pattern → same offset
    if (TB_NUM_FRAMES >= 3)
        errors += check_frame(out_stream, ref_off, "frame 2 (corrected)");

    std::cout << "\n============================================================\n";
    std::cout << (errors == 0 ? " RESULT: PASS\n" : " RESULT: FAIL\n");
    std::cout << "============================================================\n";
    return (errors == 0) ? 0 : 1;
}
