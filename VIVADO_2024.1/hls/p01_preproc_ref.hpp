// ---------------------------------------------------------------------------------------------------
// p01_preproc reference model (plain C++, no HLS types): the Vitis AI YOLOv5 preprocessing in
// integer arithmetic, equal to OpenCV (cv::cvtColor YUYV->RGB, cv::resize INTER_LINEAR, pad 114)
// within 1 LSB (check_vs_opencv.py). The kernel (Vitis Vision functions) is checked against it
// with a tolerance of 1 int8 LSB.
// Also used by the Linux application for the letterbox geometry, the registers and box mapping.
// ---------------------------------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace p01ref {

constexpr int POS_FRAC  = 16;
constexpr int COEF_BITS = 11;
constexpr int Q_FRAC    = 16;

// OpenCV BT.601 YUV -> RGB (color_yuv.simd.hpp)
constexpr int CSC_SHIFT = 20;
constexpr int CSC_CY    = 1220542;
constexpr int CSC_CUB   = 2116026;
constexpr int CSC_CUG   = -409993;
constexpr int CSC_CVG   = -852492;
constexpr int CSC_CVR   = 1673527;

struct Params {
  uint32_t src_stride;
  uint16_t src_w, src_h;
  uint16_t dst_w, dst_h;
  uint16_t act_x, act_y, act_w, act_h;
  uint32_t step_x, step_y;
  uint8_t  pad;
  uint32_t qmul[3];  // R, G, B
  int32_t  qadd[3];
};

// Letterbox geometry for a src_w x src_h frame into a dst_w x dst_h tensor, with
// int8 = round((pixel - mean) * scale * 2^fix_point) (Vitis AI convention), channels R, G, B.
// Rounding is half up, as xf::cv::preProcess (AP_RND). The Vitis AI Library truncates by default
// (XLNX_ENABLE_ROUND_SETINPUT unset); the difference is at most 1 LSB.
inline Params letterbox_params(uint16_t src_w, uint16_t src_h, uint32_t src_stride,
                               uint16_t dst_w, uint16_t dst_h, uint8_t pad,
                               const float mean[3], const float scale[3], int fix_point) {
  Params p{};
  p.src_stride = src_stride;
  p.src_w = src_w;
  p.src_h = src_h;
  p.dst_w = dst_w;
  p.dst_h = dst_h;
  if (uint64_t(src_w) * dst_h >= uint64_t(src_h) * dst_w) {  // width limited
    p.act_w = dst_w;
    p.act_h = uint16_t(uint64_t(src_h) * dst_w / src_w);
  } else {                                                   // height limited
    p.act_h = dst_h;
    p.act_w = uint16_t(uint64_t(src_w) * dst_h / src_h);
  }
  p.act_x  = uint16_t((dst_w - p.act_w) / 2);
  p.act_y  = uint16_t((dst_h - p.act_h) / 2);
  p.step_x = uint32_t(std::llround(double(src_w) / p.act_w * (1 << POS_FRAC)));
  p.step_y = uint32_t(std::llround(double(src_h) / p.act_h * (1 << POS_FRAC)));
  p.pad    = pad;
  const double fix = std::ldexp(1.0, fix_point);
  for (int c = 0; c < 3; c++) {
    p.qmul[c] = uint32_t(std::llround(scale[c] * fix * (1 << Q_FRAC)));
    p.qadd[c] = int32_t(std::llround(-mean[c] * scale[c] * fix * (1 << Q_FRAC)));
  }
  return p;
}

// p01_preproc registers alpha / beta (xf::cv::preProcess: int8 = round((pixel - alpha) * beta)).
inline void quant_params(const float mean[3], const float scale[3], int fix_point, float alpha[3],
                         float beta[3]) {
  for (int c = 0; c < 3; c++) {
    alpha[c] = mean[c];
    beta[c]  = float(scale[c] * std::ldexp(1.0, fix_point));
  }
}

inline void src_coord(int64_t pos2, int size, int& s0, int& s1, int& w1) {
  const int64_t pos = pos2 >> 1;  // arithmetic shift = floor
  if (pos < 0) {
    s0 = 0;
    w1 = 0;
  } else {
    const int64_t ipart = pos >> POS_FRAC;
    const int64_t frac  = pos & ((1 << POS_FRAC) - 1);
    if (ipart >= size - 1) {
      s0 = size - 1;
      w1 = 0;
    } else {
      s0 = int(ipart);
      w1 = int((frac * (1 << COEF_BITS) + (1 << (POS_FRAC - 1))) >> POS_FRAC);
    }
  }
  s1 = std::min(s0 + 1, size - 1);
}

inline uint8_t sat_u8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

inline void yuv_to_rgb(int y, int u, int v, uint8_t rgb[3]) {
  const int uu  = u - 128;
  const int vv  = v - 128;
  const int yy  = std::max(0, y - 16) * CSC_CY;
  const int ruv = (1 << (CSC_SHIFT - 1)) + CSC_CVR * vv;
  const int guv = (1 << (CSC_SHIFT - 1)) + CSC_CVG * vv + CSC_CUG * uu;
  const int buv = (1 << (CSC_SHIFT - 1)) + CSC_CUB * uu;
  rgb[0] = sat_u8((yy + ruv) >> CSC_SHIFT);
  rgb[1] = sat_u8((yy + guv) >> CSC_SHIFT);
  rgb[2] = sat_u8((yy + buv) >> CSC_SHIFT);
}

// RGB of source pixel x in a YUYV row (chroma shared by each pixel pair).
inline void src_rgb(const uint8_t* row, int x, uint8_t rgb[3]) {
  const uint8_t* m = row + 4 * (x >> 1);
  yuv_to_rgb(m[(x & 1) ? 2 : 0], m[1], m[3], rgb);
}

inline int8_t quantise(int v, uint32_t qmul, int32_t qadd) {
  const int64_t q = (int64_t(v) * qmul + qadd + (1 << (Q_FRAC - 1))) >> Q_FRAC;
  return int8_t(std::clamp<int64_t>(q, -128, 127));
}

// src: frame with row stride src_stride. Returns the dst_h x dst_w x 3 (R, G, B) int8 tensor.
inline std::vector<int8_t> run(const uint8_t* src, const Params& p) {
  std::vector<int8_t> out(size_t(p.dst_w) * p.dst_h * 3);
  for (int dy = 0; dy < p.dst_h; dy++) {
    const bool row_active = dy >= p.act_y && dy < p.act_y + p.act_h;
    int sy0 = 0, sy1 = 0, wy1 = 0;
    if (row_active) {
      const int64_t pos2y = int64_t(2 * (dy - p.act_y) + 1) * p.step_y - (1 << POS_FRAC);
      src_coord(pos2y, p.src_h, sy0, sy1, wy1);
    }
    const int wy0 = (1 << COEF_BITS) - wy1;
    const uint8_t* r0 = src + size_t(sy0) * p.src_stride;
    const uint8_t* r1 = src + size_t(sy1) * p.src_stride;
    int64_t pos2x = int64_t(p.step_x) - (1 << POS_FRAC);
    for (int dx = 0; dx < p.dst_w; dx++) {
      int v[3] = {p.pad, p.pad, p.pad};
      if (row_active && dx >= p.act_x && dx < p.act_x + p.act_w) {
        int sx0, sx1, wx1;
        src_coord(pos2x, p.src_w, sx0, sx1, wx1);
        pos2x += 2 * int64_t(p.step_x);
        const int wx0 = (1 << COEF_BITS) - wx1;
        uint8_t a0[3], a1[3], b0[3], b1[3];
        src_rgb(r0, sx0, a0);
        src_rgb(r0, sx1, a1);
        src_rgb(r1, sx0, b0);
        src_rgb(r1, sx1, b1);
        for (int c = 0; c < 3; c++) {
          const int64_t h0 = a0[c] * wx0 + a1[c] * wx1;
          const int64_t h1 = b0[c] * wx0 + b1[c] * wx1;
          v[c] = int((h0 * wy0 + h1 * wy1 + (1 << (2 * COEF_BITS - 1))) >> (2 * COEF_BITS));
        }
      }
      for (int c = 0; c < 3; c++) {
        out[(size_t(dy) * p.dst_w + dx) * 3 + c] = quantise(v[c], p.qmul[c], p.qadd[c]);
      }
    }
  }
  return out;
}

// Test frame (HLS testbench and p01-app --pl-test): gradients + checkerboard + LCG noise in every
// byte (Y, U and V for YUYV); exercises every weight, both borders and the YUV -> RGB saturation.
inline std::vector<uint8_t> test_pattern(int src_w, int src_h, uint32_t stride, uint32_t seed) {
  std::vector<uint8_t> img(size_t(stride) * src_h, 0xA5);
  const int bpp = 2;  // YUYV
  const int row_bytes = src_w * bpp;
  uint32_t s = seed;
  for (int y = 0; y < src_h; y++) {
    for (int i = 0; i < row_bytes; i++) {
      s = s * 1664525u + 1013904223u;
      const int x     = i / bpp;
      const int grad  = (x * 255) / (src_w > 1 ? src_w - 1 : 1);
      const int check = (((x >> 3) ^ (y >> 3)) & 1) ? 40 : 0;
      const int noise = int(s >> 27) - 16;
      int v;
      if (bpp == 2 && (i & 1)) {  // chroma byte: U at i%4 == 1, V at i%4 == 3
        v = (i & 2) ? 255 - (y * 255) / (src_h - 1) : grad;
        v = v / 2 + 64 + noise;
      } else {
        v = (y < src_h / 2) ? grad + noise : ((y * 255) / (src_h - 1)) / 2 + check + noise;
      }
      img[size_t(y) * stride + i] = sat_u8(v);
    }
  }
  return img;
}

// Maps a tensor coordinate (e.g. a YOLO box corner) back to source pixels.
inline void tensor_to_src(const Params& p, float tx, float ty, float& sx, float& sy) {
  sx = (tx - p.act_x) * float(p.src_w) / p.act_w;
  sy = (ty - p.act_y) * float(p.src_h) / p.act_h;
}

}  // namespace p01ref
