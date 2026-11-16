// ---------------------------------------------------------------------------------------------------
// p01_preproc 2.1 - PL preprocessing (Vitis HLS 2024.1 + AMD Vitis Vision library)
//
// AXI4-Stream Video in (from the AMD Video Frame Buffer Read IP, YUYV8, 1 pixel / clock, 24-bit TDATA)
//   -> xf::cv::AXIvideo2xfMat
//   -> xf::cv::yuyv2rgb
//   -> xf::cv::resize (bilinear) + xf::cv::insertBorder (pad)       = letterbox
//   -> xf::cv::preProcess: int8 = round((pixel - alpha[c]) * beta[c])  = DPU input quantisation
//   -> xf::cv::xfMat2AXIvideo
// AXI4-Stream Video out (to the AMD Video Frame Buffer Write IP, memory format RGB8 = DPU tensor)
//
// The only project code is the AXI4-Stream Video component order (UG934: RGB = {R, B, G}).
// Input: YUYV 4:2:2 camera frames (USB camera).
//
// Software computes the registers (p01_preproc_ref.hpp letterbox_params(), README):
//   src_w / src_h  camera frame size (YUYV, src_w even)
//   rsz_w / rsz_h  letterbox size (act_w / act_h); insertBorder centres it: (dst - rsz) / 2
//   dst_w / dst_h  DPU input tensor size, pad = letterbox fill value (114 for YOLOv5)
//   alpha[c]       mean, beta[c] = scale * 2^fix_point (channels R, G, B)
// Constraints: src_w <= MAX_SRC_W, src_h <= MAX_SRC_H, dst <= MAX_DST, src / rsz <= MAX_DOWN_SCALE,
//              (pixel - alpha) * beta must fit int8 (preProcess wraps, it does not saturate).
// ---------------------------------------------------------------------------------------------------
#pragma once

#include <ap_axi_sdata.h>
#include <hls_stream.h>
#include <cstdint>

namespace p01 {

constexpr int MAX_SRC_W      = 2048;
constexpr int MAX_SRC_H      = 2048;
constexpr int MAX_DST_W      = 1024;
constexpr int MAX_DST_H      = 1024;
constexpr int MAX_DOWN_SCALE = 4;    // xf::cv::resize line buffer: 2048 -> 640 is 3.2
constexpr int AXIS_W         = 24;   // Frame Buffer IPs: 3 components x 8 bit, 1 pixel / clock

using axis_t = ap_axiu<AXIS_W, 1, 1, 1>;  // TUSER = start of frame, TLAST = end of line

}  // namespace p01

void p01_preproc(hls::stream<p01::axis_t>& s_axis_video, hls::stream<p01::axis_t>& m_axis_video,
                 uint16_t src_w, uint16_t src_h, uint16_t rsz_w, uint16_t rsz_h,
                 uint16_t dst_w, uint16_t dst_h, uint8_t pad,
                 float alpha0, float alpha1, float alpha2, float beta0, float beta1, float beta2);
