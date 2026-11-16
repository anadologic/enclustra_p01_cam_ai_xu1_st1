// ---------------------------------------------------------------------------------------------------
// p01_preproc 2.1 testbench: AXI4-Stream Video in / out, compared with the OpenCV-equivalent
// reference model (p01_preproc_ref.hpp).
//
// The streams are built like the AMD Video Frame Buffer IPs do it (UG934 / PG278):
//   in : YUYV8 -> {0, C, Y} per pixel (Cb on even, Cr on odd pixels)
//   out: {R, B, G} per pixel, stored by Frame Buffer Write (RGB8) as bytes R, G, B
// Checks: TUSER only on the first beat, TLAST at every line end, beat count, padding exact,
// |kernel - reference| <= kTolerance (int8 LSB) everywhere.
//
//   argv[1] = "all"   : every case (C simulation)
//             "cosim" : small cases only (RTL co-simulation)
//   argv[2] = optional directory for raw dumps (input / output / params), used by
//             check_vs_opencv.py to compare against the OpenCV-based Vitis AI preprocessing.
// ---------------------------------------------------------------------------------------------------
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "p01_preproc.hpp"
#include "p01_preproc_ref.hpp"

namespace {

constexpr int kTolerance = 1;

struct Case {
  const char* name;
  uint16_t    src_w, src_h;
  uint32_t    stride;  // bytes, as in the camera buffer (Frame Buffer Read skips the rest)
  uint16_t    dst_w, dst_h;
  bool        cosim;
};

const Case kCases[] = {
    // USB cameras (YUYV from the camera, or MJPEG decoded to YUYV) -> YOLOv5-nano 640x640
    {"yuyv_640x480",       640,  480, 1280, 640, 640, false},  // pure letterbox
    {"yuyv_1280x960",     1280,  960, 2560, 640, 640, false},  // C270 MJPEG maximum, downscale 2
    {"yuyv_1280x720",     1280,  720, 2560, 640, 640, false},  // 16:9 downscale
    {"yuyv_1920x1080",    1920, 1080, 3840, 640, 640, false},  // downscale 3.0
    {"yuyv_small_96x64",    96,   64,  200,  64,  64, true },  // stride % 16 = 8
    {"yuyv_upscale_38x30",  38,   30,   80,  64,  64, true },  // odd pairs, borders
    {"yuyv_portrait",      120,  200,  240,  64,  64, true },  // height-limited letterbox
};

// Frame Buffer Read: memory -> AXI4-Stream Video
void frmbuf_read(const Case& c, const std::vector<uint8_t>& img, hls::stream<p01::axis_t>& s) {
  for (int y = 0; y < c.src_h; y++) {
    const uint8_t* row = img.data() + size_t(y) * c.stride;
    for (int x = 0; x < c.src_w; x++) {
      p01::axis_t beat;
      beat.data = ap_uint<24>(row[2 * x]) | (ap_uint<24>(row[2 * x + 1]) << 8);
      beat.keep = -1;
      beat.strb = -1;
      beat.user = (x == 0 && y == 0);
      beat.last = (x == c.src_w - 1);
      beat.id   = 0;
      beat.dest = 0;
      s.write(beat);
    }
  }
}

// Frame Buffer Write (RGB8): AXI4-Stream Video -> bytes R, G, B. Returns the framing errors.
int frmbuf_write(const Case& c, hls::stream<p01::axis_t>& s, std::vector<int8_t>& out) {
  int errors = 0;
  out.assign(size_t(c.dst_w) * c.dst_h * 3, 0);
  for (int y = 0; y < c.dst_h; y++) {
    for (int x = 0; x < c.dst_w; x++) {
      if (s.empty()) {
        std::printf("  stream ended early at y=%d x=%d\n", y, x);
        return errors + 1;
      }
      const p01::axis_t beat = s.read();
      if (bool(beat.user) != (x == 0 && y == 0)) errors++;
      if (bool(beat.last) != (x == c.dst_w - 1)) errors++;
      int8_t* px = &out[(size_t(y) * c.dst_w + x) * 3];
      px[0] = int8_t(uint8_t(beat.data.range(23, 16)));  // R
      px[1] = int8_t(uint8_t(beat.data.range(7, 0)));    // G
      px[2] = int8_t(uint8_t(beat.data.range(15, 8)));   // B
    }
  }
  while (!s.empty()) {
    s.read();
    errors++;
  }
  return errors;
}

void dump(const char* dir, const std::string& name, const void* data, size_t bytes) {
  const std::string path = std::string(dir) + "/" + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::printf("  cannot write %s\n", path.c_str()); return; }
  std::fwrite(data, 1, bytes, f);
  std::fclose(f);
}

int run_case(const Case& c, const char* dump_dir) {
  const float mean[3]  = {0.f, 0.f, 0.f};
  const float scale[3] = {0.00392156f, 0.00392156f, 0.00392156f};  // YOLOv5-nano prototxt
  const int   fix      = 6;                                         // xmodel input fix_point
  const p01ref::Params p = p01ref::letterbox_params(c.src_w, c.src_h, c.stride, c.dst_w,
                                                    c.dst_h, 114, mean, scale, fix);
  float alpha[3], beta[3];
  p01ref::quant_params(mean, scale, fix, alpha, beta);

  const std::vector<uint8_t> img = p01ref::test_pattern(c.src_w, c.src_h, c.stride, 0x1234u + c.src_w);
  hls::stream<p01::axis_t> s_in("s_axis_video"), s_out("m_axis_video");
  frmbuf_read(c, img, s_in);
  p01_preproc(s_in, s_out, p.src_w, p.src_h, p.act_w, p.act_h, p.dst_w, p.dst_h, p.pad,
              alpha[0], alpha[1], alpha[2], beta[0], beta[1], beta[2]);
  std::vector<int8_t> out;
  const int framing = frmbuf_write(c, s_out, out);
  const int leftover = int(s_in.size());

  const std::vector<int8_t> ref = p01ref::run(img.data(), p);
  size_t hist[3] = {0, 0, 0};  // |diff| = 0, 1, > 1
  size_t pad_err = 0;
  int    worst   = 0;
  for (size_t i = 0; i < out.size(); i++) {
    const int d = std::abs(int(out[i]) - int(ref[i]));
    hist[std::min(d, 2)]++;
    worst = std::max(worst, d);
    const size_t px = i / 3, y = px / c.dst_w, x = px % c.dst_w;
    const bool active = y >= p.act_y && y < size_t(p.act_y) + p.act_h && x >= p.act_x &&
                        x < size_t(p.act_x) + p.act_w;
    if (!active && d) pad_err++;
    if (d > kTolerance && hist[2] <= 3) {
      std::printf("  diff y=%zu x=%zu c=%zu: hw=%d ref=%d\n", y, x, i % 3, out[i], ref[i]);
    }
  }
  const bool ok = worst <= kTolerance && !framing && !pad_err && !leftover;
  std::printf("%-19s %s src %4ux%-4u -> %ux%u letterbox %ux%u@(%u,%u): %s  max|diff| %d, exact %6.2f %%, "
              "|diff|=1 %6.3f %%, framing errors %d, pad errors %zu, unread input %d\n",
              c.name, "YUYV ", c.src_w, c.src_h, c.dst_w, c.dst_h,
              p.act_w, p.act_h, p.act_x, p.act_y, ok ? "PASS" : "FAIL", worst,
              100.0 * hist[0] / out.size(), 100.0 * hist[1] / out.size(), framing, pad_err, leftover);

  if (dump_dir) {
    dump(dump_dir, std::string(c.name) + "_src.raw", img.data(), img.size());
    dump(dump_dir, std::string(c.name) + "_out.raw", out.data(), out.size());
    const std::string params = std::string(dump_dir) + "/" + c.name + "_params.txt";
    if (FILE* f = std::fopen(params.c_str(), "w")) {
      std::fprintf(f, "src_w=%u src_h=%u stride=%u dst_w=%u dst_h=%u act_x=%u act_y=%u act_w=%u act_h=%u pad=%u fix=%d round=1\n",
                   c.src_w, c.src_h, c.stride, c.dst_w, c.dst_h, p.act_x, p.act_y, p.act_w, p.act_h,
                   p.pad, fix);
      std::fclose(f);
    }
  }
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const bool  cosim    = argc > 1 && std::string(argv[1]) == "cosim";
  const char* dump_dir = argc > 2 ? argv[2] : nullptr;
  int         fails    = 0;
  for (const Case& c : kCases) {
    if (cosim && !c.cosim) continue;
    fails += run_case(c, dump_dir);
  }
  std::printf("%s: %d failing case(s)\n", fails ? "TEST FAILED" : "TEST PASSED", fails);
  return fails ? 1 : 0;
}
