// p01-app - V4L2 (UVC) YUYV / MJPEG capture
#include "camera_v4l2.hpp"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "common.hpp"

namespace p01 {

namespace {

int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

struct Mode {
  int    w = 0, h = 0;
  double fps = 0;  // highest frame rate of this size
};

// Discrete frame sizes of a pixel format and their highest frame rate (UVC cameras report discrete
// sizes and intervals).
std::vector<Mode> enum_modes(int fd, uint32_t fourcc) {
  std::vector<Mode> modes;
  v4l2_frmsizeenum fs{};
  fs.pixel_format = fourcc;
  for (fs.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
    if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;
    Mode m;
    m.w = int(fs.discrete.width);
    m.h = int(fs.discrete.height);
    v4l2_frmivalenum fi{};
    fi.pixel_format = fourcc;
    fi.width        = fs.discrete.width;
    fi.height       = fs.discrete.height;
    for (fi.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; fi.index++) {
      if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE && fi.discrete.numerator) {
        m.fps = std::max(m.fps, double(fi.discrete.denominator) / fi.discrete.numerator);
      }
    }
    modes.push_back(m);
  }
  return modes;
}

void set_ctrl(int fd, uint32_t id, int32_t value, const char* name) {
  v4l2_control c{};
  c.id    = id;
  c.value = value;
  if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) log("camera: %s not set (%s)", name, std::strerror(errno));
}

}  // namespace

V4l2Camera::~V4l2Camera() {
  stop();
  for (auto& b : bufs_) {
    if (b.ptr) munmap(b.ptr, b.len);
  }
  if (fd_ >= 0) close(fd_);
}

bool V4l2Camera::open(const std::string& dev, int width, int height, int fps, bool mjpeg, int max_w,
                      int max_h) {
  fd_ = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
  if (fd_ < 0) {
    log("camera: cannot open %s: %s", dev.c_str(), std::strerror(errno));
    return false;
  }
  v4l2_capability cap{};
  if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0 || !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
      !(cap.capabilities & V4L2_CAP_STREAMING)) {
    log("camera: %s is not a streaming capture device", dev.c_str());
    return false;
  }
  card_ = reinterpret_cast<const char*>(cap.card);

  // Camera discovery: the modes of both formats go to the log (journalctl -u p01).
  const std::vector<Mode> mj = enum_modes(fd_, V4L2_PIX_FMT_MJPEG);
  const std::vector<Mode> yu = enum_modes(fd_, V4L2_PIX_FMT_YUYV);
  for (const auto* f : {&mj, &yu}) {
    std::string list;
    for (const Mode& m : *f) list += " " + std::to_string(m.w) + "x" + std::to_string(m.h) + "@" + std::to_string(int(m.fps + 0.5));
    log("camera: %s modes:%s", f == &mj ? "MJPEG" : "YUYV", list.empty() ? " (none)" : list.c_str());
  }
  // Auto size: the largest mode within max_w x max_h that reaches fps; MJPEG first (if asked for),
  // then YUYV. A camera without MJPEG always runs in YUYV.
  auto pick = [&](const std::vector<Mode>& modes) -> const Mode* {
    const Mode* best = nullptr;
    for (const Mode& m : modes) {
      if (m.w > max_w || m.h > max_h || m.fps + 0.5 < fps) continue;
      if (!best || m.w * m.h > best->w * best->h) best = &m;
    }
    return best;
  };
  if (mjpeg && mj.empty()) {
    log("camera: no MJPEG modes, using YUYV");
    mjpeg = false;
  }
  if (width <= 0 || height <= 0) {
    const Mode* best = mjpeg ? pick(mj) : nullptr;
    if (mjpeg && !best) {
      log("camera: no MJPEG mode up to %dx%d at %d fps, trying YUYV", max_w, max_h, fps);
      mjpeg = false;
    }
    if (!mjpeg) best = pick(yu);
    if (!best) {
      log("camera: no mode up to %dx%d at %d fps", max_w, max_h, fps);
      return false;
    }
    width  = best->w;
    height = best->h;
    log("camera: selected %s %dx%d (auto: largest up to %dx%d at %d fps)", mjpeg ? "MJPEG" : "YUYV", width,
        height, max_w, max_h, fps);
  }
  mjpeg_ = mjpeg;
  const uint32_t fourcc = mjpeg ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
  const char*    name   = mjpeg ? "MJPEG" : "YUYV";

  v4l2_format fmt{};
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = fourcc;
  fmt.fmt.pix.field       = V4L2_FIELD_NONE;
  if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != fourcc) {
    log("camera: %s %dx%d not supported", name, width, height);
    return false;
  }
  width_  = fmt.fmt.pix.width;
  height_ = fmt.fmt.pix.height;
  if (width_ != width || height_ != height) {
    log("camera: %dx%d not available, the camera gives %dx%d", width, height, width_, height_);
  }
  stride_ = mjpeg ? width_ * 2 : int(fmt.fmt.pix.bytesperline);

  v4l2_streamparm parm{};
  parm.type                                  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator   = 1;
  parm.parm.capture.timeperframe.denominator = fps;
  if (xioctl(fd_, VIDIOC_S_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator) {
    fps_ = double(parm.parm.capture.timeperframe.denominator) / parm.parm.capture.timeperframe.numerator;
  }
  // Keep the frame rate constant in low light (UVC "exposure, dynamic framerate").
  set_ctrl(fd_, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0, "exposure_auto_priority");

  v4l2_requestbuffers req{};
  req.count  = 8;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
    log("camera: VIDIOC_REQBUFS failed");
    return false;
  }
  bufs_.resize(req.count);
  for (uint32_t i = 0; i < req.count; i++) {
    v4l2_buffer b{};
    b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index  = i;
    if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) return false;
    bufs_[i].len = b.length;
    bufs_[i].ptr = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
    if (bufs_[i].ptr == MAP_FAILED) {
      bufs_[i].ptr = nullptr;
      return false;
    }
  }
  log("camera: %s '%s' %s %dx%d @ %.1f fps, %zu buffers", dev.c_str(), card_.c_str(), name, width_,
      height_, fps_, bufs_.size());
  return true;
}

bool V4l2Camera::start() {
  for (uint32_t i = 0; i < bufs_.size(); i++) {
    v4l2_buffer b{};
    b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index  = i;
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) return false;
  }
  v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  streaming_      = xioctl(fd_, VIDIOC_STREAMON, &t) == 0;
  return streaming_;
}

void V4l2Camera::stop() {
  if (!streaming_) return;
  v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  xioctl(fd_, VIDIOC_STREAMOFF, &t);
  streaming_ = false;
}

bool V4l2Camera::dequeue(Frame& f, int timeout_ms) {
  pollfd p{fd_, POLLIN, 0};
  if (poll(&p, 1, timeout_ms) <= 0) return false;
  v4l2_buffer b{};
  b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) return false;
  f.data     = static_cast<const uint8_t*>(bufs_[b.index].ptr);
  f.bytes    = b.bytesused;
  f.index    = b.index;
  f.sequence = b.sequence;
  f.ts_ms    = b.timestamp.tv_sec * 1000.0 + b.timestamp.tv_usec / 1000.0;
  f.error    = (b.flags & V4L2_BUF_FLAG_ERROR) != 0;
  return true;
}

void V4l2Camera::requeue(const Frame& f) {
  v4l2_buffer b{};
  b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  b.index  = f.index;
  xioctl(fd_, VIDIOC_QBUF, &b);
}

}  // namespace p01
