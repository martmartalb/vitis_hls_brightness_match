#include "ap_int.h"
#include "hls_stream.h"
#include "ap_axi_sdata.h"

#define PPC         2
#define BITS_PER_CH 8
#define NUM_CH      3
#define PIXEL_BITS  (BITS_PER_CH * NUM_CH)
#define WORD_BITS   (PIXEL_BITS * PPC)

#define TARGET_MEAN 128

// Fixed-point reciprocal: mean ≈ (acc * k_mean) >> Q_MEAN
// k_mean = floor(2^Q_MEAN / N), precomputed by the caller for their resolution.
// Q_MEAN=28 supports accumulators up to 4K (32-bit unsigned is sufficient).
#define Q_MEAN 28

// offset = TARGET_MEAN - mean, range [-255, 127] -> ap_int<9> would do, use ap_int<10>
typedef ap_int<10> offset_t;

typedef ap_axiu<WORD_BITS, 1, 1, 1> axi_pixel_t;
typedef hls::stream<axi_pixel_t>    axi_stream_t;

// ----------------------------------------------------------------------------
// brightness_match — processes ONE frame per call (ap_ctrl_hs default).
//
// Hardware: connect ap_start = 1 (or ap_done → ap_start) for continuous
//           frame-by-frame operation without any control intervention.
// Cosim:    the testbench calls this function once per frame; the ap_ctrl_hs
//           interface provides the synchronisation cosim requires, so the
//           ap_none scalar port k_mean is legal here.
//
// k_mean: floor(2^Q_MEAN / N) — wire this port to a BRAM output, register,
//         or constant in your block design.  N = total pixel count per frame.
//
// Offset registers are static: they start at 0 (first frame passes through
// unchanged) and carry over between calls automatically.
// ----------------------------------------------------------------------------
void brightness_match(
    axi_stream_t &in_stream,
    axi_stream_t &out_stream,
    ap_uint<32>   k_mean
) {
#pragma HLS INTERFACE axis    port=in_stream
#pragma HLS INTERFACE axis    port=out_stream
#pragma HLS INTERFACE ap_none port=k_mean
// No return pragma → default ap_ctrl_hs (start/done/idle/ready).
// ap_ctrl_hs is what cosim requires when a scalar port is present.

    static offset_t offset_r = 0, offset_g = 0, offset_b = 0;

    ap_uint<32> acc_r = 0, acc_g = 0, acc_b = 0;

    bool not_last = true;
    frame_loop: while (not_last) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=2073600 avg=1036800

        axi_pixel_t in_word  = in_stream.read();
        axi_pixel_t out_word = in_word;
        not_last = !in_word.last;

        ap_uint<16> word_acc_r = 0, word_acc_g = 0, word_acc_b = 0;

        pixel_loop: for (int p = 0; p < PPC; p++) {
#pragma HLS UNROLL
            int base = p * PIXEL_BITS;

            ap_uint<8> r = in_word.data(base + 23, base + 16);
            ap_uint<8> g = in_word.data(base + 15, base +  8);
            ap_uint<8> b = in_word.data(base +  7, base +  0);

            // Apply stored offset from the previous frame
            ap_int<12> r_sum = (ap_int<12>)r + (ap_int<12>)offset_r;
            ap_int<12> g_sum = (ap_int<12>)g + (ap_int<12>)offset_g;
            ap_int<12> b_sum = (ap_int<12>)b + (ap_int<12>)offset_b;

            // Clamp to [0, 255]
            out_word.data(base + 23, base + 16) = (r_sum < 0) ? ap_uint<8>(0) : (r_sum > 255) ? ap_uint<8>(255) : ap_uint<8>(r_sum);
            out_word.data(base + 15, base +  8) = (g_sum < 0) ? ap_uint<8>(0) : (g_sum > 255) ? ap_uint<8>(255) : ap_uint<8>(g_sum);
            out_word.data(base +  7, base +  0) = (b_sum < 0) ? ap_uint<8>(0) : (b_sum > 255) ? ap_uint<8>(255) : ap_uint<8>(b_sum);

            // Accumulate original values for the next offset computation
            word_acc_r += r;
            word_acc_g += g;
            word_acc_b += b;
        }

        acc_r += word_acc_r;
        acc_g += word_acc_g;
        acc_b += word_acc_b;

        out_stream.write(out_word);
    }

    // Once per frame, after tlast: mean ≈ (acc * k_mean) >> Q_MEAN.
    ap_uint<8> mean_r = (ap_uint<8>)((ap_uint<64>)(acc_r * (ap_uint<64>)k_mean) >> Q_MEAN);
    ap_uint<8> mean_g = (ap_uint<8>)((ap_uint<64>)(acc_g * (ap_uint<64>)k_mean) >> Q_MEAN);
    ap_uint<8> mean_b = (ap_uint<8>)((ap_uint<64>)(acc_b * (ap_uint<64>)k_mean) >> Q_MEAN);

    offset_r = (offset_t)TARGET_MEAN - (offset_t)mean_r;
    offset_g = (offset_t)TARGET_MEAN - (offset_t)mean_g;
    offset_b = (offset_t)TARGET_MEAN - (offset_t)mean_b;
}
