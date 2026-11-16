// p01-app - MJPEG frame -> YUYV 4:2:2 (CPU, libjpeg-turbo of the image, libjpeg.so.62)
//
// Output is BT.601 limited range like a YUYV camera (the JPEG's full-range values are rescaled),
// so the PL and the display treat both sources the same way.
//
// The JPEG is decoded to raw Y / Cb / Cr planes (no colour conversion, no upsampling: the PL does
// YUV -> RGB), then packed to YUYV with NEON. Supports 4:2:2 (Y H2V1 + chroma 1x1 as webcams, or
// Y H2V2 + chroma 1x2 as ffmpeg) and 4:2:0 (Y H2V2 + chroma 1x1, chroma rows repeated). UVC MJPEG frames have no Huffman tables; libjpeg-turbo uses
// the standard ones. One decoder per thread.
#pragma once

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

namespace p01 {

// libjpeg error manager that returns to decode_yuyv() / probe() instead of calling exit().
struct JpegErrorMgr {
  jpeg_error_mgr pub;  // must be first: libjpeg sees only this part
  jmp_buf        jump;
  char           msg[JMSG_LENGTH_MAX];
};

class MjpegDecoder {
 public:
  MjpegDecoder();
  ~MjpegDecoder();
  MjpegDecoder(const MjpegDecoder&) = delete;
  MjpegDecoder& operator=(const MjpegDecoder&) = delete;

  // Frame size from the JPEG header.
  bool probe(const uint8_t* jpg, size_t n, int& width, int& height);
  // Decodes a width x height frame into YUYV rows of dst_stride bytes. False on a corrupt frame.
  bool decode_yuyv(const uint8_t* jpg, size_t n, uint8_t* dst, int dst_stride, int width, int height);
  const std::string& error() const { return error_; }
  bool               fast_dct = false;  // JDCT_IFAST instead of JDCT_ISLOW

 private:
  __attribute__((noinline)) bool decode_body(const uint8_t* jpg, size_t n, uint8_t* dst, int dst_stride,
                                             int width, int height);
  jpeg_decompress_struct cinfo_{};
  JpegErrorMgr           err_{};
  std::vector<uint8_t>   y_, cb_, cr_;  // one iMCU row of each plane
  std::string            error_;
};

// Splits a concatenated MJPEG stream (one JPEG after the other, e.g. ffmpeg -f mjpeg) into frames.
std::vector<std::pair<size_t, size_t>> split_mjpeg(const std::vector<uint8_t>& stream);

}  // namespace p01
