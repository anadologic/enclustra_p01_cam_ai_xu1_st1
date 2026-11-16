// ---------------------------------------------------------------------------------------------------
// p01_preproc 2.1 - kernel. See p01_preproc.hpp for the pipeline and the registers.
//
// Every image operation is an AMD Vitis Vision L1 function (ext/Vitis_Libraries, v2024.1_update2).
// Project code is only the glue marked "glue" below.
// ---------------------------------------------------------------------------------------------------
#include "p01_preproc.hpp"

#include "common/xf_common.hpp"
#include "common/xf_infra.hpp"
#include "dnn/xf_insertBorder.hpp"
#include "dnn/xf_preprocess.hpp"
#include "imgproc/xf_cvt_color.hpp"
#include "imgproc/xf_resize.hpp"

using namespace p01;

namespace {

using yuyv_mat_t = xf::cv::Mat<XF_16UC1, MAX_SRC_H, MAX_SRC_W, XF_NPPC1>;  // {C [15:8], Y [7:0]}
using rgb_mat_t  = xf::cv::Mat<XF_8UC3, MAX_SRC_H, MAX_SRC_W, XF_NPPC1>;   // {B, G, R [7:0]}
using dst_mat_t  = xf::cv::Mat<XF_8UC3, MAX_DST_H, MAX_DST_W, XF_NPPC1>;

constexpr int MAX_DST_PIX = MAX_DST_W * MAX_DST_H;

// Vitis Vision preProcess takes the per-channel mean (alpha) and scale (beta) as a float array.
// Fixed-point formats: alpha ap_ufixed<16, 8>, beta ap_fixed<24, 4>, result ap_fixed<8, 8> = int8.
void quantise(dst_mat_t& in, dst_mat_t& out, float a0, float a1, float a2, float b0, float b1, float b2) {
  float params[6] = {a0, a1, a2, b0, b1, b2};
  xf::cv::preProcess<XF_8UC3, XF_8UC3, MAX_DST_H, MAX_DST_W, XF_NPPC1, 16, 8, 24, 4, 8, 8>(in, out, params);
}

// glue: Vitis Vision RGB {B, G, R [7:0]} -> AXI4-Stream Video RGB {R, B, G [7:0]} (UG934), so that
// the Frame Buffer Write memory format RGB8 stores bytes R, G, B = the DPU tensor channel order.
void to_axis_rgb_order(dst_mat_t& in, dst_mat_t& out) {
  const int n = in.rows * in.cols;
  for (int i = 0; i < n; i++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT max=MAX_DST_PIX
    const ap_uint<24> px = in.read(i);
    ap_uint<24> v;
    v.range(7, 0)   = px.range(15, 8);   // G
    v.range(15, 8)  = px.range(23, 16);  // B
    v.range(23, 16) = px.range(7, 0);    // R
    out.write(i, v);
  }
}

}  // namespace

void p01_preproc(hls::stream<axis_t>& s_axis_video, hls::stream<axis_t>& m_axis_video,
                 uint16_t src_w, uint16_t src_h, uint16_t rsz_w, uint16_t rsz_h,
                 uint16_t dst_w, uint16_t dst_h, uint8_t pad,
                 float alpha0, float alpha1, float alpha2, float beta0, float beta1, float beta2) {
#pragma HLS INTERFACE mode=axis port=s_axis_video register_mode=both
#pragma HLS INTERFACE mode=axis port=m_axis_video register_mode=both
#pragma HLS INTERFACE mode=s_axilite port=src_w  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=src_h  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=rsz_w  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=rsz_h  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=dst_w  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=dst_h  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=pad    bundle=control
#pragma HLS INTERFACE mode=s_axilite port=alpha0 bundle=control
#pragma HLS INTERFACE mode=s_axilite port=alpha1 bundle=control
#pragma HLS INTERFACE mode=s_axilite port=alpha2 bundle=control
#pragma HLS INTERFACE mode=s_axilite port=beta0  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=beta1  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=beta2  bundle=control
#pragma HLS INTERFACE mode=s_axilite port=return bundle=control

  yuyv_mat_t yuyv(src_h, src_w);
  rgb_mat_t  rgb(src_h, src_w);
  dst_mat_t  resized(rsz_h, rsz_w);
  dst_mat_t  boxed(dst_h, dst_w);
  dst_mat_t  tensor(dst_h, dst_w);
  dst_mat_t  tensor_axis(dst_h, dst_w);

#pragma HLS DATAFLOW
  xf::cv::AXIvideo2xfMat(s_axis_video, yuyv);
  xf::cv::yuyv2rgb<XF_16UC1, XF_8UC3, MAX_SRC_H, MAX_SRC_W, XF_NPPC1>(yuyv, rgb);
  xf::cv::resize<XF_INTERPOLATION_BILINEAR, XF_8UC3, MAX_SRC_H, MAX_SRC_W, MAX_DST_H, MAX_DST_W, XF_NPPC1,
                 false, MAX_DOWN_SCALE>(rgb, resized);
  xf::cv::insertBorder<XF_8UC3, MAX_DST_H, MAX_DST_W, MAX_DST_H, MAX_DST_W, XF_NPPC1>(resized, boxed, pad);
  quantise(boxed, tensor, alpha0, alpha1, alpha2, beta0, beta1, beta2);
  to_axis_rgb_order(tensor, tensor_axis);
  xf::cv::xfMat2AXIvideo(tensor_axis, m_axis_video);
}
