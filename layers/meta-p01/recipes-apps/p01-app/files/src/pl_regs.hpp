// p01-app - PL register access (/dev/mem): video pipeline and version_reg.
//
//   camera frame (CMA) -> fb_rd (AMD Video Frame Buffer Read) -> p01_preproc (Vitis Vision HLS)
//                      -> fb_wr (AMD Video Frame Buffer Write, RGB8) -> DPU input tensor
//
// Addresses: VIVADO_2024.1/bd/bd.tcl. Register offsets: the AMD drivers xv_frmbufrd_hw.h /
// xv_frmbufwr_hw.h (Vitis 2024.1 embeddedsw, v_frmbuf_*_v4_8) and the HLS header xp01_preproc_hw.h.
#pragma once

#include <cstdint>

#include "p01_preproc_ref.hpp"

namespace p01 {

constexpr uint64_t kPreprocBase  = 0xA0000000;
constexpr uint64_t kVersionBase  = 0xA0010000;
constexpr uint64_t kFbRdBase     = 0xA0020000;
constexpr uint64_t kFbWrBase     = 0xA0030000;
constexpr uint64_t kVideoRstBase = 0xA0040000;
constexpr uint32_t kMagic        = 0x50303120;  // "P01 "

// Frame Buffer memory formats (XVIDC_CSF_MEM_*, PG278 pixel format IDs)
constexpr uint32_t kFbFmtYUYV8 = 12;
constexpr uint32_t kFbFmtRGB8  = 20;  // bytes R, G, B

class Preproc {
 public:
  ~Preproc();
  bool     open();
  uint32_t hw_magic() const;
  uint32_t hw_version() const;
  // Programs the pipeline: src = YUYV camera frame (p.src_*), dst = DPU input tensor (RGB8).
  // alpha / beta: p01ref::quant_params().
  void     configure(const p01ref::Params& p, const float alpha[3], const float beta[3],
                     uint64_t src_phys, uint64_t dst_phys);
  void     set_addresses(uint64_t src_phys, uint64_t dst_phys);
  void     start();                    // fb_wr, p01_preproc, then fb_rd
  bool     wait_done(int timeout_us);  // polls fb_wr AP_CTRL (ap_done is clear-on-read)
  void     reset();                    // video_rst pulse, then configure() again

 private:
  struct Block {
    volatile uint32_t* r = nullptr;
    void     wr(uint32_t off, uint32_t v) { r[off / 4] = v; }
    uint32_t rd(uint32_t off) const { return r[off / 4]; }
  };
  int   fd_ = -1;
  Block pre_, ver_, rd_, wr_, rst_;
};

}  // namespace p01
