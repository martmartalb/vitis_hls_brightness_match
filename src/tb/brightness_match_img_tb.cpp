#include <iostream>
#include <string>
#include <cstdint>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
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

typedef ap_axiu<WORD_BITS, 1, 1, 1> axi_pixel_t;
typedef hls::stream<axi_pixel_t>    axi_stream_t;

void brightness_match(axi_stream_t &in_stream, axi_stream_t &out_stream,
                      ap_uint<32> k_mean);

// Derive directory paths from this source file's location at compile time.
// __FILE__ resolves to .../src/tb/brightness_match_img_tb.cpp
static std::string tb_data_dir() {
    std::string path = __FILE__;
    return path.substr(0, path.find_last_of("/\\") + 1) + "data/";
}

#ifdef __HLS_CSIM__
static std::string artifacts_dir() {
    std::string path = __FILE__;
    for (int i = 0; i < 3; i++) {        // strip filename, tb/, src/
        size_t pos = path.find_last_of("/\\");
        if (pos != std::string::npos) path = path.substr(0, pos);
    }
    std::string dir = path + "/artifacts/";
    std::filesystem::create_directories(dir);
    return dir;
}
#endif

// Pack two RGB pixels into one AXI word.
// Byte order per pixel in the stream: B[7:0], G[15:8], R[23:16].
static axi_pixel_t make_word(uint8_t r0, uint8_t g0, uint8_t b0,
                              uint8_t r1, uint8_t g1, uint8_t b1,
                              bool last, bool sof) {
    axi_pixel_t w;
    w.data( 7,  0) = b0;  w.data(15,  8) = g0;  w.data(23, 16) = r0;
    w.data(31, 24) = b1;  w.data(39, 32) = g1;  w.data(47, 40) = r1;
    w.last = last ? 1 : 0;
    w.user = sof  ? 1 : 0;
    w.keep = 0x3F;
    w.strb = 0x3F;
    w.id   = 0;
    w.dest = 0;
    return w;
}

// Push one frame from an OpenCV BGR Mat into the AXI stream.
static void push_frame(axi_stream_t &s, const cv::Mat &img) {
    int total_words = (img.cols * img.rows) / PPC;
    int word_idx = 0;
    for (int y = 0; y < img.rows; y++) {
        for (int x = 0; x < img.cols; x += PPC) {
            bool is_last = (word_idx == total_words - 1);
            bool is_sof  = (word_idx == 0);

            // OpenCV stores pixels as BGR
            cv::Vec3b p0 = img.at<cv::Vec3b>(y, x);
            cv::Vec3b p1 = img.at<cv::Vec3b>(y, x + 1);

            s.write(make_word(p0[2], p0[1], p0[0],
                              p1[2], p1[1], p1[0],
                              is_last, is_sof));
            word_idx++;
        }
    }
}

static uint8_t ref_clamp(int v) {
    if (v <   0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

int main() {
    std::string img_path = tb_data_dir() + "brightened.png";

    cv::Mat img = cv::imread(img_path, cv::IMREAD_COLOR);
    if (img.empty()) {
        std::cerr << "Failed to load image: " << img_path << "\n";
        return 1;
    }
    if (img.cols % PPC != 0) {
        std::cerr << "Image width " << img.cols << " is not a multiple of PPC=" << PPC << "\n";
        return 1;
    }

    int total_pixels = img.cols * img.rows;
    uint32_t k_mean = (1u << Q_MEAN) / (uint32_t)total_pixels;

    // Compute per-channel accumulator and reference mean/offset
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
    for (int y = 0; y < img.rows; y++) {
        for (int x = 0; x < img.cols; x++) {
            cv::Vec3b px = img.at<cv::Vec3b>(y, x);
            sum_b += px[0];
            sum_g += px[1];
            sum_r += px[2];
        }
    }

    // Mirror IP arithmetic: mean = (acc * k_mean) >> Q_MEAN
    uint8_t ref_mean_r = (uint8_t)(((uint64_t)sum_r * k_mean) >> Q_MEAN);
    uint8_t ref_mean_g = (uint8_t)(((uint64_t)sum_g * k_mean) >> Q_MEAN);
    uint8_t ref_mean_b = (uint8_t)(((uint64_t)sum_b * k_mean) >> Q_MEAN);

    int16_t ref_off_r = (int16_t)TARGET_MEAN - (int16_t)ref_mean_r;
    int16_t ref_off_g = (int16_t)TARGET_MEAN - (int16_t)ref_mean_g;
    int16_t ref_off_b = (int16_t)TARGET_MEAN - (int16_t)ref_mean_b;

    std::cout << "============================================================\n";
    std::cout << " brightness_match_img testbench\n";
    std::cout << "============================================================\n";
    std::cout << " image : " << img_path << "\n";
    std::cout << " size  : " << img.cols << "x" << img.rows
              << "  pixels=" << total_pixels << "\n";
    std::cout << " k_mean=" << k_mean << "  Q_MEAN=" << Q_MEAN << "\n";
    std::cout << " mean  R=" << (int)ref_mean_r
              << " G=" << (int)ref_mean_g
              << " B=" << (int)ref_mean_b << "\n";
    std::cout << " offset R=" << ref_off_r
              << " G=" << ref_off_g
              << " B=" << ref_off_b << "\n\n";

    axi_stream_t in_stream, out_stream;

    // Push two frames; the IP's static offset registers start at 0.
    push_frame(in_stream, img);
    brightness_match(in_stream, out_stream, k_mean);
    push_frame(in_stream, img);
    brightness_match(in_stream, out_stream, k_mean);

    int errors = 0;
    int total_words = total_pixels / PPC;
    const int mismatch_limit = 10;

    // Frame 0: offset registers are 0 → output must equal input
    {
        int local_errors = 0;
        for (int word_idx = 0; word_idx < total_words; word_idx++) {
            axi_pixel_t w = out_stream.read();
            for (int p = 0; p < PPC; p++) {
                int base = p * PIXEL_BITS;
                int x    = (word_idx % (img.cols / PPC)) * PPC + p;
                int y    = word_idx / (img.cols / PPC);

                uint8_t out_r = (uint8_t)w.data(base + 23, base + 16).to_uint();
                uint8_t out_g = (uint8_t)w.data(base + 15, base +  8).to_uint();
                uint8_t out_b = (uint8_t)w.data(base +  7, base +  0).to_uint();

                cv::Vec3b px = img.at<cv::Vec3b>(y, x);
                uint8_t exp_r = px[2], exp_g = px[1], exp_b = px[0];

                if (out_r != exp_r || out_g != exp_g || out_b != exp_b) {
                    if (local_errors < mismatch_limit)
                        std::cout << "  frame0 pixel(" << x << "," << y << ")"
                                  << " got RGB=(" << (int)out_r << "," << (int)out_g
                                  << "," << (int)out_b << ")"
                                  << " exp=(" << (int)exp_r << "," << (int)exp_g
                                  << "," << (int)exp_b << ")\n";
                    local_errors++;
                }
            }
        }
        errors += local_errors;
        std::cout << "  frame 0 (pass-through): "
                  << (local_errors == 0 ? "PASS" : "FAIL") << "\n";
    }

    // Frame 1: offset computed from frame 0's per-channel mean
#ifdef __HLS_CSIM__
    cv::Mat out_img(img.rows, img.cols, CV_8UC3);
#endif
    {
        int local_errors = 0;
        for (int word_idx = 0; word_idx < total_words; word_idx++) {
            axi_pixel_t w = out_stream.read();
            for (int p = 0; p < PPC; p++) {
                int base = p * PIXEL_BITS;
                int x    = (word_idx % (img.cols / PPC)) * PPC + p;
                int y    = word_idx / (img.cols / PPC);

                uint8_t out_r = (uint8_t)w.data(base + 23, base + 16).to_uint();
                uint8_t out_g = (uint8_t)w.data(base + 15, base +  8).to_uint();
                uint8_t out_b = (uint8_t)w.data(base +  7, base +  0).to_uint();

#ifdef __HLS_CSIM__
                out_img.at<cv::Vec3b>(y, x) = cv::Vec3b(out_b, out_g, out_r);
#endif
                cv::Vec3b px = img.at<cv::Vec3b>(y, x);
                uint8_t exp_r = ref_clamp((int)px[2] + ref_off_r);
                uint8_t exp_g = ref_clamp((int)px[1] + ref_off_g);
                uint8_t exp_b = ref_clamp((int)px[0] + ref_off_b);

                if (out_r != exp_r || out_g != exp_g || out_b != exp_b) {
                    if (local_errors < mismatch_limit)
                        std::cout << "  frame1 pixel(" << x << "," << y << ")"
                                  << " got RGB=(" << (int)out_r << "," << (int)out_g
                                  << "," << (int)out_b << ")"
                                  << " exp=(" << (int)exp_r << "," << (int)exp_g
                                  << "," << (int)exp_b << ")\n";
                    local_errors++;
                }
            }
        }
        errors += local_errors;
        std::cout << "  frame 1 (corrected):    "
                  << (local_errors == 0 ? "PASS" : "FAIL") << "\n";
    }

#ifdef __HLS_CSIM__
    std::string out_path = artifacts_dir() + "output_img.png";
    cv::imwrite(out_path, out_img);
    std::cout << " output : " << out_path << "\n";
#endif

    std::cout << "\n============================================================\n";
    std::cout << (errors == 0 ? " RESULT: PASS\n" : " RESULT: FAIL\n");
    std::cout << "============================================================\n";
    return (errors == 0) ? 0 : 1;
}
