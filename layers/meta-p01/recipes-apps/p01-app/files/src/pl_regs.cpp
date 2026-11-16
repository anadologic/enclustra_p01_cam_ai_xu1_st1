// p01-app - PL register access
#include "pl_regs.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.hpp"

namespace p01 {

namespace {

// HLS block-level control (all three IPs): bit0 ap_start, bit1 ap_done, bit2 ap_idle
enum : uint32_t { AP_CTRL = 0x00, GIE = 0x04, IER = 0x08, ISR = 0x0C };

// XV_FRMBUFRD_CTRL_ADDR_HWREG_* / XV_FRMBUFWR_CTRL_ADDR_HWREG_* (same map for both)
enum : uint32_t { FB_WIDTH = 0x10, FB_HEIGHT = 0x18, FB_STRIDE = 0x20, FB_FORMAT = 0x28, FB_ADDR = 0x30 };

// XP01_PREPROC_CONTROL_ADDR_*_DATA (p01_preproc 2.1)
enum : uint32_t {
  SRC_W = 0x10, SRC_H = 0x18, RSZ_W = 0x20, RSZ_H = 0x28, DST_W = 0x30, DST_H = 0x38,
  PAD = 0x40, ALPHA0 = 0x48, ALPHA1 = 0x50, ALPHA2 = 0x58, BETA0 = 0x60, BETA1 = 0x68, BETA2 = 0x70,
};

constexpr size_t kMapSize = 0x10000;

uint32_t float_bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

}  // namespace

Preproc::~Preproc() {
  for (Block* b : {&pre_, &ver_, &rd_, &wr_, &rst_}) {
    if (b->r) munmap(const_cast<uint32_t*>(b->r), kMapSize);
  }
  if (fd_ >= 0) close(fd_);
}

bool Preproc::open() {
  fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
  if (fd_ < 0) {
    log("pl: cannot open /dev/mem (run as root): %s", std::strerror(errno));
    return false;
  }
  const struct { Block* b; uint64_t base; } maps[] = {
      {&pre_, kPreprocBase}, {&ver_, kVersionBase}, {&rd_, kFbRdBase}, {&wr_, kFbWrBase},
      {&rst_, kVideoRstBase}};
  for (const auto& m : maps) {
    void* a = mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, m.base);
    if (a == MAP_FAILED) {
      log("pl: mmap 0x%llx failed: %s", (unsigned long long)m.base, std::strerror(errno));
      return false;
    }
    m.b->r = static_cast<volatile uint32_t*>(a);
  }
  return true;
}

// version_reg is read before the rest is used: a bitstream without the 2.0 pipeline still has it.
uint32_t Preproc::hw_magic() const { return ver_.rd(0x0); }    // GPIO_DATA
uint32_t Preproc::hw_version() const { return ver_.rd(0x8); }  // GPIO2_DATA

void Preproc::set_addresses(uint64_t src_phys, uint64_t dst_phys) {
  rd_.wr(FB_ADDR, uint32_t(src_phys));
  rd_.wr(FB_ADDR + 4, uint32_t(src_phys >> 32));
  wr_.wr(FB_ADDR, uint32_t(dst_phys));
  wr_.wr(FB_ADDR + 4, uint32_t(dst_phys >> 32));
}

void Preproc::configure(const p01ref::Params& p, const float alpha[3], const float beta[3],
                        uint64_t src_phys, uint64_t dst_phys) {
  for (Block* b : {&pre_, &rd_, &wr_}) {  // polled operation
    b->wr(GIE, 0);
    b->wr(IER, 0);
  }
  rd_.wr(FB_WIDTH, p.src_w);
  rd_.wr(FB_HEIGHT, p.src_h);
  rd_.wr(FB_STRIDE, p.src_stride);
  rd_.wr(FB_FORMAT, kFbFmtYUYV8);

  pre_.wr(SRC_W, p.src_w);
  pre_.wr(SRC_H, p.src_h);
  pre_.wr(RSZ_W, p.act_w);
  pre_.wr(RSZ_H, p.act_h);
  pre_.wr(DST_W, p.dst_w);
  pre_.wr(DST_H, p.dst_h);
  pre_.wr(PAD, p.pad);
  pre_.wr(ALPHA0, float_bits(alpha[0]));
  pre_.wr(ALPHA1, float_bits(alpha[1]));
  pre_.wr(ALPHA2, float_bits(alpha[2]));
  pre_.wr(BETA0, float_bits(beta[0]));
  pre_.wr(BETA1, float_bits(beta[1]));
  pre_.wr(BETA2, float_bits(beta[2]));

  wr_.wr(FB_WIDTH, p.dst_w);
  wr_.wr(FB_HEIGHT, p.dst_h);
  wr_.wr(FB_STRIDE, uint32_t(p.dst_w) * 3);
  wr_.wr(FB_FORMAT, kFbFmtRGB8);

  set_addresses(src_phys, dst_phys);
}

void Preproc::start() {
  wr_.wr(AP_CTRL, 0x1);
  pre_.wr(AP_CTRL, 0x1);
  rd_.wr(AP_CTRL, 0x1);
}

bool Preproc::wait_done(int timeout_us) {
  const double end = now_ms() + timeout_us / 1000.0;
  while (now_ms() < end) {
    if (wr_.rd(AP_CTRL) & 0x2) {  // ap_done: the tensor is in DDR
      pre_.rd(AP_CTRL);           // clear the other two ap_done flags (both finished earlier)
      rd_.rd(AP_CTRL);
      return true;
    }
    usleep(50);
  }
  return false;
}

void Preproc::reset() {
  rst_.wr(0x0, 0);  // GPIO_DATA: rst_video aux_reset_in low
  usleep(10);
  rst_.wr(0x0, 1);
  usleep(10);
}

}  // namespace p01
