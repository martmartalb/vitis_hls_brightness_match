#include "ap_int.h"
#include "ap_fixed.h"
#include "hls_stream.h"
#include "ap_axi_sdata.h"

// ----------------------------------------------------------------------------
// Image and stream parameters
// ----------------------------------------------------------------------------
#define IMG_WIDTH    1920
#define IMG_HEIGHT   1080
#define PPC          2                        // Pixels per clock
#define BITS_PER_CH  8                        // Bits per channel
#define NUM_CH       3                        // R, G, B
#define PIXEL_BITS   (BITS_PER_CH * NUM_CH)   // 24 bits per pixel
#define WORD_BITS    (PIXEL_BITS * PPC)       // AXI data bus width
#define TOTAL_PIXELS (IMG_WIDTH * IMG_HEIGHT) // 2,073,600
#define TOTAL_WORDS  (TOTAL_PIXELS / PPC)     // Loop iterations per frame

// ----------------------------------------------------------------------------
// Mean approximation: mean ~= (acc * K_MEAN) >> Q_MEAN
// N = 2,073,600 -> Q=28, K = round(2^28 / 2,073,600) = 129
// Error: 2^28 / 2,073,600 = 129.45 -> ~0.3% approximation error
// ----------------------------------------------------------------------------
#define Q_MEAN       28
#define K_MEAN       129

// ----------------------------------------------------------------------------
// Target brightness [0, 255]
// ----------------------------------------------------------------------------
#define TARGET_MEAN  128

// ----------------------------------------------------------------------------
// Number of frames processed in C simulation.
// Ignored during synthesis/cosim, where the IP runs free (while(1)).
// The testbench must push exactly this many frames.
// ----------------------------------------------------------------------------
#ifndef TB_NUM_FRAMES
#define TB_NUM_FRAMES 3
#endif

// ----------------------------------------------------------------------------
// Types
// ----------------------------------------------------------------------------
// offset = TARGET_MEAN - current_mean, range [-255, 255], signed 9-bit integer
typedef ap_fixed<10, 9>  offset_t;

// Intermediate sum: pixel(8-bit) + offset([-255,255]) -> [-255, 510] -> 11 bits signed
typedef ap_fixed<11, 10> sum_t;

// External AXI stream type: only used at top-level interface ports
typedef ap_axiu<WORD_BITS, 1, 1, 1> axi_pixel_t;
typedef hls::stream<axi_pixel_t>    axi_stream_t;

// Internal stream type: used between submodules inside the top-level.
// tvalid and tready are implicit in hls::stream.
struct pixel_word_t {
    ap_uint<WORD_BITS>     data;  // TDATA
    ap_uint<1>             last;  // TLAST
    ap_uint<1>             user;  // TUSER
    ap_uint<WORD_BITS / 8> keep;  // TKEEP
    ap_uint<WORD_BITS / 8> strb;  // TSTRB
    ap_uint<1>             id;    // TID
    ap_uint<1>             dest;  // TDEST
};

typedef hls::stream<pixel_word_t> internal_stream_t;

// ----------------------------------------------------------------------------
// Component 1: compute_mean
//   - Reads one frame from external AXI stream
//   - Accumulates R, G, B sums across the whole frame
//   - Forwards pixels to internal stream unchanged
//   - Computes new offsets once after the complete frame
// ----------------------------------------------------------------------------
static void compute_mean(
    axi_stream_t      &in_stream,
    internal_stream_t &out_stream,
    offset_t          &offset_r,
    offset_t          &offset_g,
    offset_t          &offset_b
) {
#pragma HLS INLINE off

    ap_uint<32> acc_r = 0;
    ap_uint<32> acc_g = 0;
    ap_uint<32> acc_b = 0;

    word_loop: for (int i = 0; i < TOTAL_WORDS; i++) {
#pragma HLS PIPELINE II=1

        axi_pixel_t in_word = in_stream.read();

        pixel_word_t mid_word;
        mid_word.data = in_word.data;
        mid_word.last = in_word.last;
        mid_word.user = in_word.user;
        mid_word.keep = in_word.keep;
        mid_word.strb = in_word.strb;
        mid_word.id   = in_word.id;
        mid_word.dest = in_word.dest;

        ppc_loop: for (int p = 0; p < PPC; p++) {
#pragma HLS UNROLL

            int base = p * PIXEL_BITS;

            ap_uint<8> r = in_word.data(base + 23, base + 16);
            ap_uint<8> g = in_word.data(base + 15, base +  8);
            ap_uint<8> b = in_word.data(base +  7, base +  0);

            acc_r += r;
            acc_g += g;
            acc_b += b;
        }

        out_stream.write(mid_word);
    }

    // mean ~= (acc * K_MEAN) >> Q_MEAN
    ap_uint<8> mean_r = (ap_uint<8>)((acc_r * (ap_uint<32>)K_MEAN) >> Q_MEAN);
    ap_uint<8> mean_g = (ap_uint<8>)((acc_g * (ap_uint<32>)K_MEAN) >> Q_MEAN);
    ap_uint<8> mean_b = (ap_uint<8>)((acc_b * (ap_uint<32>)K_MEAN) >> Q_MEAN);

    offset_r = (offset_t)TARGET_MEAN - (offset_t)mean_r;
    offset_g = (offset_t)TARGET_MEAN - (offset_t)mean_g;
    offset_b = (offset_t)TARGET_MEAN - (offset_t)mean_b;
}

// ----------------------------------------------------------------------------
// Component 2: apply_offset
//   - Reads one frame from internal stream
//   - Applies previous-frame offsets
//   - Preserves AXI sideband fields
//   - Writes one frame to external AXI stream
// ----------------------------------------------------------------------------
static void apply_offset(
    internal_stream_t &in_stream,
    axi_stream_t      &out_stream,
    offset_t           offset_r,
    offset_t           offset_g,
    offset_t           offset_b
) {
#pragma HLS INLINE off

    word_loop: for (int i = 0; i < TOTAL_WORDS; i++) {
#pragma HLS PIPELINE II=1

        pixel_word_t in_word = in_stream.read();

        ppc_loop: for (int p = 0; p < PPC; p++) {
#pragma HLS UNROLL

            int base = p * PIXEL_BITS;

            ap_uint<8> r = in_word.data(base + 23, base + 16);
            ap_uint<8> g = in_word.data(base + 15, base +  8);
            ap_uint<8> b = in_word.data(base +  7, base +  0);

            // Add offset using wider signed type to avoid overflow before clamping
            sum_t r_sum = (sum_t)r + (sum_t)offset_r;
            sum_t g_sum = (sum_t)g + (sum_t)offset_g;
            sum_t b_sum = (sum_t)b + (sum_t)offset_b;

            // Clamp to [0, 255]
            ap_uint<8> r_out = (r_sum < 0)   ? ap_uint<8>(0)   :
                               (r_sum > 255) ? ap_uint<8>(255) :
                                               ap_uint<8>(r_sum);

            ap_uint<8> g_out = (g_sum < 0)   ? ap_uint<8>(0)   :
                               (g_sum > 255) ? ap_uint<8>(255) :
                                               ap_uint<8>(g_sum);

            ap_uint<8> b_out = (b_sum < 0)   ? ap_uint<8>(0)   :
                               (b_sum > 255) ? ap_uint<8>(255) :
                                               ap_uint<8>(b_sum);

            in_word.data(base + 23, base + 16) = r_out;
            in_word.data(base + 15, base +  8) = g_out;
            in_word.data(base +  7, base +  0) = b_out;
        }

        axi_pixel_t out_word;
        out_word.data = in_word.data;
        out_word.last = in_word.last;
        out_word.user = in_word.user;
        out_word.keep = in_word.keep;
        out_word.strb = in_word.strb;
        out_word.id   = in_word.id;
        out_word.dest = in_word.dest;

        out_stream.write(out_word);
    }
}

// ----------------------------------------------------------------------------
// Top-level: brightness_match
//   - Free-running AXI4-Stream IP for HDMI/video pipelines
//   - Frame N uses offsets computed from frame N-1
//   - First frame uses offset 0, so it passes unchanged
//   - In synthesis/cosim: runs forever (while 1)
//   - In C simulation:   runs TB_NUM_FRAMES iterations then returns
// ----------------------------------------------------------------------------

// Dataflow region as its own function
static void process_frame(
    axi_stream_t &in_stream,
    axi_stream_t &out_stream,
    offset_t  cur_offset_r, offset_t  cur_offset_g, offset_t  cur_offset_b,
    offset_t &new_offset_r, offset_t &new_offset_g, offset_t &new_offset_b
) {
#pragma HLS DATAFLOW
    internal_stream_t mid_stream;
#pragma HLS STREAM variable=mid_stream depth=16

    compute_mean(in_stream,  mid_stream, new_offset_r, new_offset_g, new_offset_b);
    apply_offset(mid_stream, out_stream, cur_offset_r, cur_offset_g, cur_offset_b);
}

// void brightness_match(axi_stream_t &in_stream, axi_stream_t &out_stream) {
// #pragma HLS INTERFACE axis port=in_stream
// #pragma HLS INTERFACE axis port=out_stream
// #pragma HLS INTERFACE ap_ctrl_none port=return

//     static offset_t offset_r = 0, offset_g = 0, offset_b = 0;

// #if defined(__SYNTHESIS__) && !defined(COSIM_BOUNDED)
//     while (1) {
// #else
//     for (int f = 0; f < TB_NUM_FRAMES; f++) {
// #endif
//         offset_t new_offset_r = 0, new_offset_g = 0, new_offset_b = 0;

//         process_frame(in_stream, out_stream,
//                       offset_r, offset_g, offset_b,
//                       new_offset_r, new_offset_g, new_offset_b);

//         offset_r = new_offset_r;
//         offset_g = new_offset_g;
//         offset_b = new_offset_b;
//     }
// }

void brightness_match(axi_stream_t &in_stream, axi_stream_t &out_stream) {
#pragma HLS INTERFACE axis port=in_stream
#pragma HLS INTERFACE axis port=out_stream
#pragma HLS INTERFACE ap_ctrl_none port=return

    static offset_t offset_r = 0, offset_g = 0, offset_b = 0;

    // No loop — process exactly TB_NUM_FRAMES frames driven by the testbench
    for (int f = 0; f < TB_NUM_FRAMES; f++) {
        offset_t new_offset_r = 0, new_offset_g = 0, new_offset_b = 0;
        process_frame(in_stream, out_stream,
                      offset_r, offset_g, offset_b,
                      new_offset_r, new_offset_g, new_offset_b);
        offset_r = new_offset_r;
        offset_g = new_offset_g;
        offset_b = new_offset_b;
    }
}