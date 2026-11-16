// p01-app - V4L2 (UVC) capture with mmap buffers: YUYV, or MJPEG (decoded by the application)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p01 {

class V4l2Camera {
 public:
  struct Frame {
    const uint8_t* data = nullptr;
    size_t         bytes = 0;
    uint32_t       index = 0;     // V4L2 buffer index (for requeue)
    uint32_t       sequence = 0;  // driver frame counter (gaps = dropped frames)
    double         ts_ms = 0.0;   // driver timestamp, CLOCK_MONOTONIC (same base as now_ms)
    bool           error = false; // V4L2_BUF_FLAG_ERROR: incomplete / corrupted frame
  };

  ~V4l2Camera();
  // width / height <= 0: auto (largest mode up to max_w x max_h at fps). mjpeg falls back to YUYV
  // when the camera has no suitable MJPEG mode; mjpeg() tells which format is used.
  bool open(const std::string& dev, int width, int height, int fps, bool mjpeg = true, int max_w = 1280,
            int max_h = 960);
  bool start();
  void stop();
  bool dequeue(Frame& f, int timeout_ms);
  void requeue(const Frame& f);

  int         width() const { return width_; }
  int         height() const { return height_; }
  int         stride() const { return stride_; }  // YUYV bytes per line (also for MJPEG: decoded size)
  bool        mjpeg() const { return mjpeg_; }
  double      fps() const { return fps_; }
  std::string card() const { return card_; }

 private:
  struct Buf {
    void*  ptr = nullptr;
    size_t len = 0;
  };
  int              fd_ = -1;
  int              width_ = 0, height_ = 0, stride_ = 0;
  double           fps_ = 0.0;
  std::string      card_;
  std::vector<Buf> bufs_;
  bool             streaming_ = false;
  bool             mjpeg_ = false;
};

}  // namespace p01
