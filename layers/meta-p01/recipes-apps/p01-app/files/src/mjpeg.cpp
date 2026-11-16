// p01-app - MJPEG -> YUYV decoder
#include "mjpeg.hpp"

#include <algorithm>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace p01 {

namespace {

void on_error(j_common_ptr c) {
  auto* m = reinterpret_cast<JpegErrorMgr*>(c->err);  // pub is the first member
  (*c->err->format_message)(c, m->msg);
  longjmp(m->jump, 1);
}

void on_message(j_common_ptr, int) {}  // corrupt-data warnings: the frame still decodes

// JPEG (JFIF) YCbCr is full range 0..255; the camera YUYV path, Vitis Vision yuyv2rgb in the PL
// and the display shader use BT.601 limited range (Y 16..235, C 16..240). Rescale while packing.
inline uint8_t y_lim(uint8_t v) { return uint8_t(((v * 220) >> 8) + 16); }
inline uint8_t c_lim(uint8_t v) { return uint8_t(((v * 225) >> 8) + 16); }

#if defined(__aarch64__)
inline uint8x16_t scale_add16(uint8x16_t v, uint8x8_t k) {
  const uint8x8_t lo = vshrn_n_u16(vmull_u8(vget_low_u8(v), k), 8);
  const uint8x8_t hi = vshrn_n_u16(vmull_u8(vget_high_u8(v), k), 8);
  return vaddq_u8(vcombine_u8(lo, hi), vdupq_n_u8(16));
}
#endif

// Y Y ... + Cb + Cr (half width), full range -> Y0 Cb Y1 Cr ..., limited range
void pack_yuyv(uint8_t* d, const uint8_t* y, const uint8_t* cb, const uint8_t* cr, int w) {
  int x = 0;
#if defined(__aarch64__)
  const uint8x8_t ky = vdup_n_u8(220), kc = vdup_n_u8(225);
  for (; x + 32 <= w; x += 32) {
    const uint8x16x2_t yy = vld2q_u8(y + x);  // even / odd pixels
    uint8x16x4_t       o;
    o.val[0] = scale_add16(yy.val[0], ky);
    o.val[1] = scale_add16(vld1q_u8(cb + x / 2), kc);
    o.val[2] = scale_add16(yy.val[1], ky);
    o.val[3] = scale_add16(vld1q_u8(cr + x / 2), kc);
    vst4q_u8(d + 2 * x, o);
  }
#endif
  for (; x + 1 < w; x += 2) {
    d[2 * x]     = y_lim(y[x]);
    d[2 * x + 1] = c_lim(cb[x / 2]);
    d[2 * x + 2] = y_lim(y[x + 1]);
    d[2 * x + 3] = c_lim(cr[x / 2]);
  }
}

}  // namespace

MjpegDecoder::MjpegDecoder() {
  cinfo_.err               = jpeg_std_error(&err_.pub);
  err_.pub.error_exit      = on_error;
  err_.pub.emit_message    = on_message;
  jpeg_create_decompress(&cinfo_);
}

MjpegDecoder::~MjpegDecoder() { jpeg_destroy_decompress(&cinfo_); }

bool MjpegDecoder::probe(const uint8_t* jpg, size_t n, int& width, int& height) {
  if (setjmp(err_.jump)) {
    error_ = err_.msg;
    jpeg_abort_decompress(&cinfo_);
    return false;
  }
  jpeg_mem_src(&cinfo_, jpg, static_cast<unsigned long>(n));
  jpeg_read_header(&cinfo_, TRUE);
  width  = int(cinfo_.image_width);
  height = int(cinfo_.image_height);
  jpeg_abort_decompress(&cinfo_);
  return true;
}

bool MjpegDecoder::decode_yuyv(const uint8_t* jpg, size_t n, uint8_t* dst, int dst_stride, int width,
                               int height) {
  // setjmp only here; the work is in decode_body(), so no local of this frame is live across longjmp
  if (setjmp(err_.jump)) {
    error_ = err_.msg;
    jpeg_abort_decompress(&cinfo_);
    return false;
  }
  return decode_body(jpg, n, dst, dst_stride, width, height);
}

bool MjpegDecoder::decode_body(const uint8_t* jpg, size_t n, uint8_t* dst, int dst_stride, int width,
                               int height) {
  jpeg_mem_src(&cinfo_, jpg, static_cast<unsigned long>(n));
  jpeg_read_header(&cinfo_, TRUE);

  // Y H2 with Cb / Cr H1 (half width). Vertical: chroma V equal to Y V = 4:2:2 (H2V1 + 1x1 as
  // webcams, H2V2 + 1x2 as ffmpeg), chroma V half of Y V = 4:2:0 (H2V2 + 1x1).
  const jpeg_component_info* c = cinfo_.comp_info;
  const bool layout_ok = cinfo_.num_components == 3 && cinfo_.jpeg_color_space == JCS_YCbCr &&
                         c[0].h_samp_factor == 2 && (c[0].v_samp_factor == 1 || c[0].v_samp_factor == 2) &&
                         c[1].h_samp_factor == 1 && c[2].h_samp_factor == 1 &&
                         c[1].v_samp_factor == c[2].v_samp_factor &&
                         (c[1].v_samp_factor == c[0].v_samp_factor || c[1].v_samp_factor * 2 == c[0].v_samp_factor);
  if (int(cinfo_.image_width) != width || int(cinfo_.image_height) != height || !layout_ok) {
    error_ = "unexpected size or chroma layout (need YCbCr 4:2:2 or 4:2:0)";
    jpeg_abort_decompress(&cinfo_);
    return false;
  }
  cinfo_.raw_data_out        = TRUE;
  cinfo_.do_fancy_upsampling = FALSE;
  cinfo_.dct_method          = fast_dct ? JDCT_IFAST : JDCT_ISLOW;
  jpeg_start_decompress(&cinfo_);

  const int rows   = c[0].v_samp_factor * DCTSIZE;  // luma rows per jpeg_read_raw_data call
  const int c_rows = c[1].v_samp_factor * DCTSIZE;  // chroma rows per call
  const int vdiv   = rows / c_rows;                 // 1: 4:2:2, 2: 4:2:0 (chroma row repeated)
  const int y_w    = int(c[0].width_in_blocks) * DCTSIZE;
  const int c_w    = int(c[1].width_in_blocks) * DCTSIZE;
  if (y_.size() < size_t(rows) * y_w) y_.resize(size_t(rows) * y_w);
  if (cb_.size() < size_t(c_rows) * c_w) {
    cb_.resize(size_t(c_rows) * c_w);
    cr_.resize(size_t(c_rows) * c_w);
  }
  JSAMPROW   y_rows[2 * DCTSIZE], cb_rows[2 * DCTSIZE], cr_rows[2 * DCTSIZE];
  JSAMPARRAY planes[3] = {y_rows, cb_rows, cr_rows};
  for (int r = 0; r < rows; r++) y_rows[r] = y_.data() + size_t(r) * y_w;
  for (int r = 0; r < c_rows; r++) {
    cb_rows[r] = cb_.data() + size_t(r) * c_w;
    cr_rows[r] = cr_.data() + size_t(r) * c_w;
  }

  while (cinfo_.output_scanline < cinfo_.output_height) {
    const int y0 = int(cinfo_.output_scanline);
    jpeg_read_raw_data(&cinfo_, planes, JDIMENSION(rows));
    const int n_rows = std::min(rows, height - y0);
    for (int r = 0; r < n_rows; r++) {
      pack_yuyv(dst + size_t(y0 + r) * dst_stride, y_rows[r], cb_rows[r / vdiv], cr_rows[r / vdiv], width);
    }
  }
  jpeg_finish_decompress(&cinfo_);
  return true;
}

std::vector<std::pair<size_t, size_t>> split_mjpeg(const std::vector<uint8_t>& s) {
  // FF D8 FF starts a JPEG; it cannot occur in entropy-coded data (FF is stuffed with 00 there).
  std::vector<size_t> starts;
  for (size_t i = 0; i + 2 < s.size(); i++) {
    if (s[i] == 0xFF && s[i + 1] == 0xD8 && s[i + 2] == 0xFF) starts.push_back(i);
  }
  std::vector<std::pair<size_t, size_t>> frames;
  for (size_t k = 0; k < starts.size(); k++) {
    const size_t end = k + 1 < starts.size() ? starts[k + 1] : s.size();
    frames.emplace_back(starts[k], end - starts[k]);
  }
  return frames;
}

}  // namespace p01
