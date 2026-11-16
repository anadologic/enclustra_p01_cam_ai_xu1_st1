// ---------------------------------------------------------------------------------------------------
// p01-app - camera -> PL preprocessing -> DPU YOLOv5-nano -> Mali GPU overlay
//           -> mini DisplayPort.
//
// PL preprocessing: Frame Buffer Read -> p01_preproc (Vitis Vision) -> Frame Buffer Write.
// Threads: capture  (V4L2 YUYV -> CMA ring slot; MJPEG camera / file -> decode queue)
//          decode   (MJPEG only, --decoders N: libjpeg-turbo -> YUYV in the CMA ring slot)
//          infer    (PL pipeline writes the DPU input tensor -> VART run -> YOLOv5 decode + NMS)
//          main     (GLES2 composition on /dev/fb0: video + boxes + labels + statistics)
// Control: echo "<cmd>" > /tmp/p01.ctl   (video|boxes|labels|stats on|off|toggle, mode sync|lowlat,
//          shot <file.ppm>, quit)
// Run as root (/dev/mem, /proc/self/pagemap, /dev/dpu) with the fbdev libMali first in
// LD_LIBRARY_PATH (see scripts/p01-run.sh).
// ---------------------------------------------------------------------------------------------------
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "camera_v4l2.hpp"
#include "cma_buffer.hpp"
#include "common.hpp"
#include "dpu_runner.hpp"
#include "mjpeg.hpp"
#include "p01_preproc_ref.hpp"
#include "pl_regs.hpp"
#include "renderer.hpp"
#include "yolo.hpp"

using namespace p01;

namespace {

struct Options {
  std::string dev = "/dev/video0";
  int         width = 0, height = 0, fps = 30;  // 0 x 0 = auto: largest mode up to max_w x max_h at fps
  int         max_w = 1280, max_h = 960;
  std::string model = "/usr/share/p01/models/yolov5_nano_pt/yolov5_nano_pt.xmodel";
  float       conf = 0.40f, nms = 0.45f;
  bool        lowlat = false;
  double      seconds = 0;  // 0 = run until stopped
  double      shot_at = 0;
  std::string shot_path;
  std::string ctl = "/tmp/p01.ctl";
  bool        test_display = false;  // GPU / display path only (no camera, PL or DPU)
  bool        capture_only = false;  // camera only: frame rate and sequence gaps
  bool        pl_test = false;       // PL pipeline only: test frames vs. the reference model
  bool        mjpeg = true;          // camera delivers MJPEG (decoded on the CPU to YUYV); --format yuyv
  std::string mjpeg_file;            // MJPEG file played as a camera at --fps (implies mjpeg)
  int         decoders = 2;          // MJPEG decode threads
  bool        fast_dct = false;      // libjpeg JDCT_IFAST instead of JDCT_ISLOW
  std::string record;                // --capture-only: write the camera frames to this file
  std::string bench_mjpeg;           // decode speed of an MJPEG file, 1..4 threads
  int         sim_cameras = 0;       // N cameras at --fps from --mjpeg-file: decode only, real-time
  bool        view_fit = false;      // false: video covers the screen (cropped), true: whole video (bars)
};

constexpr int kSlots = 8;

struct Captured {
  int      slot = -1;
  uint32_t id = 0;
  double   ts = 0;
};

struct Result {
  uint32_t         id = 0;
  int              slot = -1;
  double           ts_capture = 0;
  std::vector<Box> boxes;  // camera pixels
  double           t_pre = 0, t_dpu = 0, t_post = 0;
};

std::atomic<bool> g_run{true};
void on_signal(int) { g_run = false; }

struct Shared {
  std::mutex              m;
  std::condition_variable cv;
  Captured                latest;
  bool                    fresh = false;  // a frame the infer thread has not taken yet
  std::shared_ptr<Result> result;
  uint64_t                captured = 0, dropped = 0, corrupt = 0, inferred = 0;
  // MJPEG: decode time, frames finished after a newer one (not shown), frames dropped from a full queue
  int64_t                 latest_id = -1;
  Ema                     dec_ms;
  uint64_t                late = 0, qdrop = 0;
};

// One compressed frame waiting for a decode thread.
struct Job {
  std::vector<uint8_t> owned;  // copy of a camera buffer (empty for file frames)
  const uint8_t*       data = nullptr;
  size_t               size = 0;
  uint32_t             id = 0;
  double               ts = 0;
  int                  stream = 0;  // --sim-cameras
};

struct Layers {
  std::atomic<bool> video{true}, boxes{true}, labels{true}, stats{true}, lowlat{false};
  std::mutex        shot_m;
  std::string       shot;
};

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) { log("missing value for %s", name); std::exit(2); }
      return argv[++i];
    };
    if (a == "--dev") o.dev = next("--dev");
    else if (a == "--size") {
      const std::string v = next("--size");
      if (v == "auto") o.width = o.height = 0;
      else std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height);
    }
    else if (a == "--max-size") { std::sscanf(next("--max-size").c_str(), "%dx%d", &o.max_w, &o.max_h); }
    else if (a == "--fps") o.fps = std::stoi(next("--fps"));
    else if (a == "--model") o.model = next("--model");
    else if (a == "--conf") o.conf = std::stof(next("--conf"));
    else if (a == "--nms") o.nms = std::stof(next("--nms"));
    else if (a == "--lowlat") o.lowlat = true;
    else if (a == "--seconds") o.seconds = std::stod(next("--seconds"));
    else if (a == "--shot") { o.shot_at = std::stod(next("--shot")); o.shot_path = next("--shot"); }
    else if (a == "--ctl") o.ctl = next("--ctl");
    else if (a == "--test-display") o.test_display = true;
    else if (a == "--capture-only") o.capture_only = true;
    else if (a == "--pl-test") o.pl_test = true;
    else if (a == "--format") o.mjpeg = next("--format") == "mjpeg";
    else if (a == "--mjpeg-file") { o.mjpeg_file = next("--mjpeg-file"); o.mjpeg = true; }
    else if (a == "--decoders") o.decoders = std::max(1, std::stoi(next("--decoders")));
    else if (a == "--fast-dct") o.fast_dct = true;
    else if (a == "--record") o.record = next("--record");
    else if (a == "--bench-mjpeg") o.bench_mjpeg = next("--bench-mjpeg");
    else if (a == "--sim-cameras") o.sim_cameras = std::max(1, std::stoi(next("--sim-cameras")));
    else if (a == "--view") o.view_fit = next("--view") == "fit";
    else {
      std::fprintf(stderr,
                   "usage: p01-app [--dev /dev/video0] [--size auto|WxH] [--max-size 1280x960] [--fps 30]\n"
                   "               [--model file.xmodel]\n"
                   "               [--conf 0.4] [--nms 0.45] [--lowlat] [--seconds N]\n"
                   "               [--shot <after_s> <file.ppm>] [--ctl /tmp/p01.ctl] [--test-display]\n"
                   "               [--capture-only [--record file]] [--pl-test]\n"
                   "               [--format mjpeg|yuyv] [--mjpeg-file file.mjpg] [--decoders 2] [--fast-dct]\n"
                   "               [--bench-mjpeg file.mjpg] [--view cover|fit]\n"
                   "               [--sim-cameras N --mjpeg-file file.mjpg [--decoders D] [--seconds S]]\n");
      return false;
    }
  }
  return true;
}

Rgba class_color(int cls) {
  static const Rgba pal[] = {{1.00f, 0.22f, 0.22f, 1}, {0.20f, 0.80f, 0.30f, 1}, {0.25f, 0.55f, 1.00f, 1},
                             {1.00f, 0.75f, 0.10f, 1}, {0.85f, 0.30f, 0.95f, 1}, {0.10f, 0.85f, 0.90f, 1},
                             {1.00f, 0.50f, 0.10f, 1}, {0.60f, 0.90f, 0.20f, 1}};
  return pal[cls % 8];
}

std::string fmt(const char* f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

void control_thread(const std::string& path, Layers& L) {
  unlink(path.c_str());
  if (mkfifo(path.c_str(), 0666) < 0) {
    log("ctl: cannot create %s", path.c_str());
    return;
  }
  chmod(path.c_str(), 0666);
  const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
  std::string buf;
  while (g_run) {
    char tmp[256];
    const ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n <= 0) { usleep(50000); continue; }
    buf.append(tmp, size_t(n));
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::istringstream ss(buf.substr(0, nl));
      buf.erase(0, nl + 1);
      std::string what, arg;
      ss >> what >> arg;
      auto apply = [&](std::atomic<bool>& f) { f = arg == "on" ? true : arg == "off" ? false : !f; };
      if (what == "video") apply(L.video);
      else if (what == "boxes") apply(L.boxes);
      else if (what == "labels") apply(L.labels);
      else if (what == "stats") apply(L.stats);
      else if (what == "mode") L.lowlat = arg == "lowlat";
      else if (what == "shot") { std::lock_guard<std::mutex> g(L.shot_m); L.shot = arg; }
      else if (what == "quit") g_run = false;
      log("ctl: %s %s", what.c_str(), arg.c_str());
    }
  }
  close(fd);
  unlink(path.c_str());
}

// Busy share of all CPU cores since the previous call, in % (from /proc/stat).
double cpu_busy() {
  static unsigned long long last_total = 0, last_idle = 0;
  unsigned long long v[8] = {0};
  FILE* f = std::fopen("/proc/stat", "r");
  if (!f) return 0;
  const int n = std::fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                            &v[4], &v[5], &v[6], &v[7]);
  std::fclose(f);
  if (n < 5) return 0;
  unsigned long long total = 0;
  for (unsigned long long x : v) total += x;
  const unsigned long long idle = v[3] + v[4];
  const unsigned long long dt = total - last_total, di = idle - last_idle;
  last_total = total;
  last_idle  = idle;
  return dt ? 100.0 * double(dt - di) / double(dt) : 0;
}

bool load_file(const std::string& path, std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    log("cannot open %s", path.c_str());
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  data.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  const bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  return ok;
}

// Decode speed of an MJPEG file: 1..4 threads, each decoding frames to YUYV for 3 s.
int bench_mjpeg(const std::string& path, bool fast_dct) {
  std::vector<uint8_t> data;
  if (!load_file(path, data)) return 1;
  const auto frames = split_mjpeg(data);
  int w = 0, h = 0;
  MjpegDecoder probe;
  if (frames.empty() || !probe.probe(data.data() + frames[0].first, frames[0].second, w, h)) {
    log("bench: no JPEG frames in %s", path.c_str());
    return 1;
  }
  log("bench: %s: %zu frames %dx%d, %.0f KB per frame, %s DCT", path.c_str(), frames.size(), w, h,
      data.size() / 1024.0 / frames.size(), fast_dct ? "IFAST" : "ISLOW");
  for (int threads = 1; threads <= 4; threads++) {
    std::atomic<uint64_t> done{0}, bad{0};
    std::atomic<bool>     stop{false};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) {
      pool.emplace_back([&, t] {
        MjpegDecoder dec;
        dec.fast_dct = fast_dct;
        std::vector<uint8_t> out(size_t(w) * 2 * h);
        for (size_t k = size_t(t); !stop; k = (k + 1) % frames.size()) {
          if (!dec.decode_yuyv(data.data() + frames[k].first, frames[k].second, out.data(), w * 2, w, h) &&
              bad++ == 0 && t == 0) {
            log("bench: decode error: %s", dec.error().c_str());
          }
          done++;
        }
      });
    }
    const double t0 = now_ms();
    while (now_ms() - t0 < 3000) usleep(20000);
    stop = true;
    for (auto& th : pool) th.join();
    const double fps = done * 1000.0 / (now_ms() - t0);
    log("bench: %d thread(s): %6.1f frames/s, %5.1f ms per frame per thread, errors %llu", threads, fps,
        threads * 1000.0 / fps, (unsigned long long)bad.load());
  }
  return 0;
}

// N MJPEG cameras in real time, decode only: each camera thread delivers a frame of the file every
// 1/fps s (copied like a camera buffer), D decode threads write YUYV into per-camera CMA slots with
// the same cache maintenance as the application. Reports delivered fps, drops, latency, CPU load.
int sim_cameras(const Options& opt) {
  std::vector<uint8_t> data;
  if (!load_file(opt.mjpeg_file, data)) return 1;
  const auto frames = split_mjpeg(data);
  int w = 0, h = 0;
  MjpegDecoder probe;
  if (frames.empty() || !probe.probe(data.data() + frames[0].first, frames[0].second, w, h)) {
    log("sim: no JPEG frames in %s", opt.mjpeg_file.c_str());
    return 1;
  }
  const int    n = opt.sim_cameras, kRing = 4;
  const size_t frame_bytes = size_t(w) * 2 * h;
  std::vector<std::vector<std::unique_ptr<CmaBuffer>>> rings(static_cast<size_t>(n));
  for (auto& r : rings) {
    for (int i = 0; i < kRing; i++) {
      r.emplace_back(new CmaBuffer);
      if (!r.back()->alloc(frame_bytes)) return 1;
    }
  }
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  log("sim: %d camera(s) x %dx%d MJPEG @ %d fps from %s (%.0f KB per frame), %d decode thread(s), %s DCT, "
      "%ld CPU core(s) online", n, w, h, opt.fps, opt.mjpeg_file.c_str(), data.size() / 1024.0 / frames.size(),
      opt.decoders, opt.fast_dct ? "IFAST" : "ISLOW", cores);

  struct Stat {
    uint64_t offered = 0, decoded = 0, dropped = 0, bad = 0;
    double   lat_sum = 0, lat_max = 0;
  };
  std::vector<Stat>       st(static_cast<size_t>(n));
  std::mutex              m;
  std::condition_variable cv;
  std::deque<Job>         q;
  const size_t            max_q = size_t(2 * opt.decoders);
  std::atomic<bool>       run{true};

  std::vector<std::thread> decoders;
  for (int t = 0; t < opt.decoders; t++) {
    decoders.emplace_back([&] {
      MjpegDecoder dec;
      dec.fast_dct = opt.fast_dct;
      while (true) {
        Job j;
        {
          std::unique_lock<std::mutex> lk(m);
          cv.wait(lk, [&] { return !q.empty() || !run; });
          if (!run) return;
          j = std::move(q.front());
          q.pop_front();
        }
        CmaBuffer& b = *rings[size_t(j.stream)][j.id % kRing];
        b.begin_cpu_write();
        const bool ok = dec.decode_yuyv(j.data, j.size, b.virt(), w * 2, w, h);
        b.end_cpu_write();
        const double lat = now_ms() - j.ts;
        std::lock_guard<std::mutex> g(m);
        Stat& s = st[size_t(j.stream)];
        s.decoded += ok;
        s.bad += !ok;
        s.lat_sum += lat;
        s.lat_max = std::max(s.lat_max, lat);
      }
    });
  }
  const double t_end = now_ms() + (opt.seconds > 0 ? opt.seconds : 20) * 1000;
  std::vector<std::thread> cams;
  for (int c = 0; c < n; c++) {
    cams.emplace_back([&, c] {
      double   next = now_ms() + c * 1000.0 / opt.fps / n;  // cameras are not frame-synchronous
      uint32_t id   = 0;
      for (size_t k = size_t(c * 37) % frames.size(); now_ms() < t_end; k = (k + 1) % frames.size()) {
        next += 1000.0 / opt.fps;
        const double wait = next - now_ms();
        if (wait > 0) usleep(useconds_t(wait * 1000));
        Job j;
        j.owned.assign(data.begin() + long(frames[k].first), data.begin() + long(frames[k].first + frames[k].second));
        j.data   = j.owned.data();
        j.size   = j.owned.size();
        j.id     = id++;
        j.ts     = now_ms();
        j.stream = c;
        std::lock_guard<std::mutex> g(m);
        st[size_t(c)].offered++;
        if (q.size() >= max_q) {
          st[size_t(q.front().stream)].dropped++;
          q.pop_front();
        }
        q.push_back(std::move(j));
        cv.notify_one();
      }
    });
  }
  cpu_busy();
  const double t0 = now_ms();
  for (auto& t : cams) t.join();
  const double busy = cpu_busy(), secs = (now_ms() - t0) / 1000;
  run = false;
  cv.notify_all();
  for (auto& t : decoders) t.join();
  bool ok = true;
  for (int c = 0; c < n; c++) {
    const Stat& s = st[size_t(c)];
    log("sim: camera %d: offered %llu, decoded %llu (%.1f fps), dropped %llu, errors %llu, latency avg %.1f ms max %.1f ms",
        c, (unsigned long long)s.offered, (unsigned long long)s.decoded, s.decoded / secs,
        (unsigned long long)s.dropped, (unsigned long long)s.bad, s.decoded ? s.lat_sum / s.decoded : 0.0, s.lat_max);
    ok = ok && s.dropped == 0 && s.bad == 0;
  }
  log("sim: CPU %.0f %% of %ld core(s) = %.2f cores busy -> %s", busy, cores, busy * cores / 100,
      ok ? "PASS (no frame dropped)" : "FAIL (frames dropped)");
  return ok ? 0 : 1;
}

// YOLOv5-nano input quantisation (prototxt mean / scale, xmodel fix_point 6)
const float kMean[3]  = {0.f, 0.f, 0.f};
const float kScale[3] = {0.00392156f, 0.00392156f, 0.00392156f};

// Hardware check of the PL pipeline without camera and DPU: test frames (both formats) through
// Frame Buffer Read -> p01_preproc -> Frame Buffer Write, compared with p01ref::run().
int pl_test(Preproc& pre) {
  struct Case { const char* name; int w, h; uint32_t stride; };
  const Case cases[] = {
      {"YUYV 640x480",   640,  480, 1280},
      {"YUYV 1280x720",  1280, 720, 2560},
      {"YUYV 1280x960",  1280, 960, 2560},
      {"YUYV 1920x1080", 1920, 1080, 3840},
  };
  const int fix = 6, dst = 640;
  float alpha[3], beta[3];
  p01ref::quant_params(kMean, kScale, fix, alpha, beta);
  CmaBuffer out;
  if (!out.alloc(size_t(dst) * dst * 3)) return 1;
  int fails = 0;
  for (const Case& c : cases) {
    const std::vector<uint8_t> img = p01ref::test_pattern(c.w, c.h, c.stride, 0x1234u + c.w);
    CmaBuffer src;
    if (!src.alloc(img.size())) return 1;
    src.begin_cpu_write();
    std::memcpy(src.virt(), img.data(), img.size());
    src.end_cpu_write();
    out.begin_cpu_write();
    std::memset(out.virt(), 0x55, out.size());  // stale data must not pass
    out.end_cpu_write();

    const p01ref::Params p = p01ref::letterbox_params(uint16_t(c.w), uint16_t(c.h), c.stride,
                                                      dst, dst, 114, kMean, kScale, fix);
    pre.configure(p, alpha, beta, src.phys(), out.phys());
    const double t0 = now_ms();
    pre.start();
    const bool done = pre.wait_done(200000);
    const double t1 = now_ms();
    if (!done) {
      log("pl-test %-15s: TIMEOUT", c.name);
      pre.reset();
      fails++;
      continue;
    }
    const std::vector<int8_t> ref = p01ref::run(img.data(), p);
    out.begin_cpu_read();
    const int8_t* hw = reinterpret_cast<const int8_t*>(out.virt());
    size_t exact = 0, pad_err = 0, big = 0, first = ref.size(), last = 0;
    int worst = 0, bias = 0;
    for (size_t i = 0; i < ref.size(); i++) {
      const int d = int(hw[i]) - int(ref[i]);
      worst = std::max(worst, std::abs(d));
      exact += d == 0;
      bias += d;
      const size_t px = i / 3, y = px / dst, x = px % dst;
      const bool active = y >= p.act_y && y < size_t(p.act_y) + p.act_h && x >= p.act_x &&
                          x < size_t(p.act_x) + p.act_w;
      if (!active && d) pad_err++;
      if (std::abs(d) > 1) {
        if (big++ < 4) log("  diff at byte %zu (y %zu x %zu c %zu): hw %d ref %d", i, y, x, i % 3, hw[i], ref[i]);
        first = std::min(first, i);
        last = i;
      }
    }
    if (big) log("  %zu values off by > 1, bytes %zu..%zu", big, first, last);
    out.end_cpu_read();
    const bool ok = worst <= 1 && pad_err == 0;
    log("pl-test %-15s: %s  %.2f ms, max|diff| %d, exact %.2f %%, bias %+.4f, pad errors %zu", c.name,
        ok ? "PASS" : "FAIL", t1 - t0, worst, 100.0 * exact / ref.size(), double(bias) / ref.size(), pad_err);
    fails += !ok;
  }
  log("pl-test: %s", fails ? "FAILED" : "PASSED");
  return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, opt)) return 2;
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (opt.capture_only) {  // V4L2 dequeue / requeue only
    V4l2Camera cam;
    if (!cam.open(opt.dev, opt.width, opt.height, opt.fps, opt.mjpeg, opt.max_w, opt.max_h) || !cam.start()) return 1;
    FILE* rec = opt.record.empty() ? nullptr : std::fopen(opt.record.c_str(), "wb");
    const double t0 = now_ms();
    uint64_t n = 0, gaps = 0, bad = 0, bytes = 0;
    uint32_t last = 0;
    while (g_run && now_ms() - t0 < (opt.seconds > 0 ? opt.seconds : 20) * 1000) {
      V4l2Camera::Frame f;
      if (!cam.dequeue(f, 200)) continue;
      if (n && f.sequence > last + 1) gaps += f.sequence - last - 1;
      last = f.sequence;
      bad += f.error;
      n++;
      bytes += f.bytes;
      if (rec && !f.error) std::fwrite(f.data, 1, f.bytes, rec);
      cam.requeue(f);
    }
    if (rec) std::fclose(rec);
    log("capture-only: %llu frames in %.1f s (%.2f fps), sequence gaps %llu, error frames %llu, %.0f KB per frame",
        (unsigned long long)n, (now_ms() - t0) / 1000, n * 1000.0 / (now_ms() - t0),
        (unsigned long long)gaps, (unsigned long long)bad, n ? bytes / 1024.0 / n : 0.0);
    return 0;
  }

  if (!opt.bench_mjpeg.empty()) return bench_mjpeg(opt.bench_mjpeg, opt.fast_dct);
  if (opt.sim_cameras > 0) return sim_cameras(opt);

  if (opt.test_display) {  // colour bars, synthetic YUYV video, boxes and text
    Renderer gl;
    if (!gl.init()) return 1;
    std::vector<uint8_t> yuyv(640 * 480 * 2);
    for (int y = 0; y < 480; y++) {
      for (int x = 0; x < 640; x += 2) {
        static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                           {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
        const uint8_t* b = bars[x * 8 / 640];
        uint8_t* p = &yuyv[(size_t(y) * 640 + x) * 2];
        p[0] = b[0]; p[1] = b[1]; p[2] = b[0]; p[3] = b[2];
      }
    }
    gl.upload_yuyv(yuyv.data(), 640, 480);
    const double t0 = now_ms();
    bool shot = false;
    while (g_run && now_ms() - t0 < (opt.seconds > 0 ? opt.seconds : 3) * 1000) {
      gl.begin();
      gl.video(0, 0, 1440, 1080);
      gl.frame_rect(300, 300, 400, 300, 4, class_color(0));
      gl.rect(300, 270, 230, 30, {0.6f, 0.13f, 0.13f, 0.85f});
      gl.text(305, 270, "person 0.91", 0.75f, {1, 1, 1, 1});
      gl.rect(1440, 0, 480, 1080, {0.07f, 0.09f, 0.12f, 1});
      gl.text(1468, 30, "DISPLAY TEST", 0.78f, {0.40f, 0.75f, 1.00f, 1});
      gl.text(1468, 70, "Mali-400 GLES2 / fbdev", 0.78f, {0.92f, 0.94f, 0.97f, 1});
      const bool take = !shot && !opt.shot_path.empty() && now_ms() - t0 > opt.shot_at * 1000;
      gl.end(take ? opt.shot_path : "");
      shot = shot || take;
    }
    return 0;
  }

  // --- PL: release check -----------------------------------------------------------------------
  Preproc pre;
  if (!pre.open()) return 1;
  const uint32_t magic = pre.hw_magic(), ver = pre.hw_version();
  if (magic != kMagic) {
    log("pl: version_reg magic 0x%08x != 0x%08x (wrong bitstream?)", magic, kMagic);
    return 1;
  }
  const std::string hw = fmt("P01 v%u.%u.%u", ver >> 16, (ver >> 8) & 0xff, ver & 0xff);
  log("pl: %s", hw.c_str());
  if (ver < 0x00020100) {
    log("pl: need the P01 2.1 bitstream (Frame Buffer Read / Write + Vitis Vision p01_preproc 2.1)");
    return 1;
  }
  if (opt.pl_test) return pl_test(pre);

  // --- DPU ------------------------------------------------------------------------------------
  DpuRunner dpu;
  if (!dpu.load(opt.model)) return 1;

  // --- Video source (V4L2 camera YUYV / MJPEG, or MJPEG file) + CMA ring --------------------------
  V4l2Camera cam;
  std::vector<uint8_t> file_data;
  std::vector<std::pair<size_t, size_t>> file_frames;
  int         src_w = 0, src_h = 0, src_stride = 0;
  std::string src_label;
  if (!opt.mjpeg_file.empty()) {
    MjpegDecoder probe;
    if (!load_file(opt.mjpeg_file, file_data)) return 1;
    file_frames = split_mjpeg(file_data);
    if (file_frames.empty() ||
        !probe.probe(file_data.data() + file_frames[0].first, file_frames[0].second, src_w, src_h)) {
      log("source: no JPEG frames in %s", opt.mjpeg_file.c_str());
      return 1;
    }
    src_stride = src_w * 2;
    src_label  = "MJPEG file @ " + std::to_string(opt.fps) + " fps";
    log("source: %s, %zu frames %dx%d, played at %d fps", opt.mjpeg_file.c_str(), file_frames.size(), src_w,
        src_h, opt.fps);
  } else {
    if (!cam.open(opt.dev, opt.width, opt.height, opt.fps, opt.mjpeg, opt.max_w, opt.max_h)) return 1;
    src_w      = cam.width();
    src_h      = cam.height();
    src_stride = cam.stride();
    src_label  = cam.card();
    const size_t lp = src_label.find('('), rp = src_label.find(')');
    src_label = (lp != std::string::npos && rp > lp) ? "Camera USB " + src_label.substr(lp + 1, rp - lp - 1)
                                                     : "Camera " + src_label.substr(0, 18);
  }
  const bool decode = !opt.mjpeg_file.empty() || cam.mjpeg();  // MJPEG decoded on the CPU
  if (decode) log("decode: %d thread(s), libjpeg-turbo %s DCT -> YUYV", opt.decoders, opt.fast_dct ? "IFAST" : "ISLOW");
  const size_t frame_bytes = size_t(src_stride) * src_h;
  std::vector<std::unique_ptr<CmaBuffer>> slots;
  for (int i = 0; i < kSlots; i++) {
    slots.emplace_back(new CmaBuffer);
    if (!slots.back()->alloc(frame_bytes)) return 1;
  }
  log("cma: %d slots x %zu bytes, slot0 phys 0x%llx", kSlots, frame_bytes, (unsigned long long)slots[0]->phys());

  const p01ref::Params prm = p01ref::letterbox_params(
      uint16_t(src_w), uint16_t(src_h), uint32_t(src_stride),
      uint16_t(dpu.input_w()), uint16_t(dpu.input_h()), 114, kMean, kScale, dpu.input_fix());
  float alpha[3], beta[3];
  p01ref::quant_params(kMean, kScale, dpu.input_fix(), alpha, beta);
  // Zero copy: Frame Buffer Write writes the VART input tensor. Fallback: a CMA tensor copied by the CPU.
  CmaBuffer  tensor_copy;
  const bool zero_copy = dpu.input_phys() != 0;
  const size_t tensor_bytes = size_t(dpu.input_w()) * dpu.input_h() * 3;
  if (!zero_copy && !tensor_copy.alloc(tensor_bytes)) return 1;
  const uint64_t tensor_phys = zero_copy ? dpu.input_phys() : tensor_copy.phys();
  pre.configure(prm, alpha, beta, slots[0]->phys(), tensor_phys);
  log("pre: %ux%u YUYV -> letterbox %ux%u @ (%u,%u) in %ux%u, beta %.6f, %s", prm.src_w, prm.src_h,
      prm.act_w, prm.act_h, prm.act_x, prm.act_y,
      prm.dst_w, prm.dst_h, beta[0], zero_copy ? "zero copy into the DPU tensor" : "CPU copy");

  // --- Display ----------------------------------------------------------------------------------
  Renderer gl;
  if (!gl.init()) return 1;
  // Zero-copy video: the GPU samples the CMA ring slots directly (no per-frame texture upload).
  bool zc_display = gl.dmabuf_supported();
  for (int i = 0; zc_display && i < kSlots; i++) {
    zc_display = gl.import_yuyv(i, slots[size_t(i)]->fd(), src_w, src_h, src_stride);
  }
  log("display: %s", zc_display ? "dma-buf textures of the CMA slots (zero copy)" : "texture upload per frame");

  Shared S;
  Layers L;
  L.lowlat = opt.lowlat;
  std::thread ctl(control_thread, opt.ctl, std::ref(L));

  // --- MJPEG decode threads ---------------------------------------------------------------------
  std::mutex              jobs_m;
  std::condition_variable jobs_cv;
  std::deque<Job>         jobs;
  const size_t            max_jobs = size_t(2 * opt.decoders);  // older frames are dropped beyond this
  auto push_job = [&](Job&& j) {
    bool dropped_one = false;
    {
      std::lock_guard<std::mutex> g(jobs_m);
      if (jobs.size() >= max_jobs) {
        jobs.pop_front();
        dropped_one = true;
      }
      jobs.push_back(std::move(j));
    }
    jobs_cv.notify_one();
    if (dropped_one) {
      std::lock_guard<std::mutex> g(S.m);
      S.qdrop++;
    }
  };
  std::vector<std::thread> decoders;
  for (int t = 0; decode && t < opt.decoders; t++) {
    decoders.emplace_back([&] {
      MjpegDecoder dec;
      dec.fast_dct = opt.fast_dct;
      while (true) {
        Job j;
        {
          std::unique_lock<std::mutex> lk(jobs_m);
          jobs_cv.wait(lk, [&] { return !jobs.empty() || !g_run; });
          if (!g_run) return;
          j = std::move(jobs.front());
          jobs.pop_front();
        }
        const int  slot = int(j.id % kSlots);
        CmaBuffer& b    = *slots[slot];
        const double t0 = now_ms();
        b.begin_cpu_write();
        const bool ok = dec.decode_yuyv(j.data, j.size, b.virt(), src_stride, src_w, src_h);
        b.end_cpu_write();
        const double t1 = now_ms();
        std::lock_guard<std::mutex> g(S.m);
        S.dec_ms.add(t1 - t0);
        if (!ok) {
          if (S.corrupt++ < 5) log("decode: frame %u: %s", j.id, dec.error().c_str());
        } else if (int64_t(j.id) > S.latest_id) {
          S.latest_id = j.id;
          S.latest    = {slot, j.id, j.ts};
          S.fresh     = true;
          S.captured++;
          S.cv.notify_all();
        } else {
          S.late++;
        }
      }
    });
  }

  // --- Capture thread ---------------------------------------------------------------------------
  if (opt.mjpeg_file.empty() && !cam.start()) {
    log("camera: stream on failed");
    return 1;
  }
  std::thread cap([&] {
    uint32_t id = 0, last_seq = 0;
    bool     have_seq = false;
    if (!opt.mjpeg_file.empty()) {  // file played as a camera
      double next = now_ms();
      for (size_t k = 0; g_run; k = (k + 1) % file_frames.size()) {
        next += 1000.0 / opt.fps;
        const double wait = next - now_ms();
        if (wait > 0) usleep(useconds_t(wait * 1000));
        Job j;
        j.data = file_data.data() + file_frames[k].first;
        j.size = file_frames[k].second;
        j.id   = id++;
        j.ts   = now_ms();
        push_job(std::move(j));
      }
      return;
    }
    while (g_run) {
      V4l2Camera::Frame f;
      if (!cam.dequeue(f, 200)) continue;
      if (decode) {  // MJPEG: copy the compressed frame, give the buffer back at once
        if (!f.error) {
          Job j;
          j.owned.assign(f.data, f.data + f.bytes);
          j.data = j.owned.data();
          j.size = j.owned.size();
          j.id   = id++;
          j.ts   = f.ts_ms;
          push_job(std::move(j));
        }
        cam.requeue(f);
        std::lock_guard<std::mutex> g(S.m);
        if (have_seq && f.sequence > last_seq + 1) S.dropped += f.sequence - last_seq - 1;
        last_seq = f.sequence;
        have_seq = true;
        if (f.error) S.corrupt++;
        continue;
      }
      // Sequence gaps = frames the driver never delivered; incomplete USB frames are counted
      // separately ("bad") and are never shown or inferred.
      const bool bad = f.error || f.bytes < frame_bytes;
      if (!bad) {
        CmaBuffer& b = *slots[id % kSlots];
        b.begin_cpu_write();
        std::memcpy(b.virt(), f.data, frame_bytes);
        b.end_cpu_write();
      }
      cam.requeue(f);
      std::lock_guard<std::mutex> g(S.m);
      if (have_seq && f.sequence > last_seq + 1) S.dropped += f.sequence - last_seq - 1;
      last_seq = f.sequence;
      have_seq = true;
      if (bad) {
        S.corrupt++;
        continue;
      }
      const int slot = int(id % kSlots);
      S.latest = {slot, id++, f.ts_ms};
      S.fresh  = true;
      S.captured++;
      S.cv.notify_all();
    }
  });

  // --- Inference thread -------------------------------------------------------------------------
  std::thread inf([&] {
    while (g_run) {
      Captured c;
      {
        std::unique_lock<std::mutex> lk(S.m);
        S.cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return S.fresh || !g_run; });
        if (!S.fresh) continue;
        c       = S.latest;
        S.fresh = false;
      }
      auto r = std::make_shared<Result>();
      r->id = c.id;
      r->slot = c.slot;
      r->ts_capture = c.ts;
      const double t0 = now_ms();
      pre.set_addresses(slots[c.slot]->phys(), tensor_phys);
      pre.start();
      if (!pre.wait_done(100000)) {
        log("pre: timeout, resetting the PL video pipeline");
        pre.reset();
        pre.configure(prm, alpha, beta, slots[c.slot]->phys(), tensor_phys);
        continue;
      }
      if (!zero_copy) {
        tensor_copy.begin_cpu_read();
        std::memcpy(dpu.input_virt(), tensor_copy.virt(), tensor_bytes);
        tensor_copy.end_cpu_read();
      }
      const double t1 = now_ms();
      if (!dpu.run()) continue;
      const double t2 = now_ms();
      auto boxes = yolov5_decode(dpu.heads(), dpu.input_w(), opt.conf, opt.nms);
      for (auto& b : boxes) {
        p01ref::tensor_to_src(prm, b.x0, b.y0, b.x0, b.y0);
        p01ref::tensor_to_src(prm, b.x1, b.y1, b.x1, b.y1);
        b.x0 = std::clamp(b.x0, 0.f, float(src_w));
        b.x1 = std::clamp(b.x1, 0.f, float(src_w));
        b.y0 = std::clamp(b.y0, 0.f, float(src_h));
        b.y1 = std::clamp(b.y1, 0.f, float(src_h));
      }
      const double t3 = now_ms();
      r->boxes = std::move(boxes);
      r->t_pre = t1 - t0;
      r->t_dpu = t2 - t1;
      r->t_post = t3 - t2;
      std::lock_guard<std::mutex> g(S.m);
      S.result = r;
      S.inferred++;
    }
  });

  // --- Render loop (main thread) ----------------------------------------------------------------
  // Full-screen video: cover (fills the screen, a non-16:9 source is cropped) or fit (whole frame).
  const int    W = gl.width(), H = gl.height();
  const float  vscale = opt.view_fit ? std::min(float(W) / src_w, float(H) / src_h)
                                     : std::max(float(W) / src_w, float(H) / src_h);
  const float  vw = src_w * vscale, vh = src_h * vscale;
  const float  vx = (W - vw) / 2, vy = (H - vh) / 2;
  const std::string src_fmt = fmt("%dx%d %s", src_w, src_h, decode ? "MJPEG" : "YUYV");
  const double t_start = now_ms();
  double       t_stat = t_start, last_fps_t = t_start;
  uint64_t     last_cap = 0, last_inf = 0, shown = 0, last_shown = 0;
  Ema          cam_fps, inf_fps, disp_fps, e_pre, e_dpu, e_post, e_lat, cpu;
  cpu_busy();
  int          shown_slot = -1;
  bool         shot_done = false;
  uint64_t     rendered = 0;

  while (g_run) {
    if (opt.seconds > 0 && now_ms() - t_start > opt.seconds * 1000) break;
    Captured                latest;
    std::shared_ptr<Result> res;
    uint64_t                captured, inferred, dropped, corrupt, late, qdrop;
    double                  dec_ms;
    {
      std::lock_guard<std::mutex> g(S.m);
      latest = S.latest;
      res = S.result;
      captured = S.captured;
      inferred = S.inferred;
      dropped = S.dropped;
      corrupt = S.corrupt;
      late = S.late;
      qdrop = S.qdrop;
      dec_ms = S.dec_ms.value;
    }
    const bool sync  = !L.lowlat;
    const int  slot  = sync ? (res ? res->slot : -1) : latest.slot;
    const double tsc = sync ? (res ? res->ts_capture : 0) : latest.ts;
    if (slot < 0) { usleep(5000); continue; }
    const bool new_frame = slot != shown_slot;
    if (new_frame) {
      if (zc_display) {
        gl.use_slot(slot);
      } else {
        gl.upload_yuyv(slots[slot]->virt(), src_w, src_h);
      }
      shown_slot = slot;
      shown++;
    }

    const double t = now_ms();
    if (t - last_fps_t >= 1000) {
      cam_fps.add((captured - last_cap) * 1000.0 / (t - last_fps_t), 0.5);
      inf_fps.add((inferred - last_inf) * 1000.0 / (t - last_fps_t), 0.5);
      disp_fps.add((shown - last_shown) * 1000.0 / (t - last_fps_t), 0.5);
      last_cap = captured;
      last_inf = inferred;
      last_shown = shown;
      last_fps_t = t;
      cpu.add(cpu_busy(), 0.5);
    }
    if (res) {
      e_pre.add(res->t_pre);
      e_dpu.add(res->t_dpu);
      e_post.add(res->t_post);
    }

    gl.begin();
    if (L.video) gl.video(vx, vy, vw, vh);
    if (res && (L.boxes || L.labels)) {
      for (const auto& b : res->boxes) {
        const float x0 = vx + b.x0 * vscale, y0 = vy + b.y0 * vscale;
        const float x1 = vx + b.x1 * vscale, y1 = vy + b.y1 * vscale;
        const Rgba  c = class_color(b.cls);
        if (L.boxes) gl.frame_rect(x0, y0, x1 - x0, y1 - y0, 4.f, c);
        if (L.labels) {
          const std::string s  = fmt("%s %.2f", coco_name(b.cls), b.score);
          const float       ts = 0.75f, tw = gl.text_width(s, ts), th = gl.line_height(ts);
          const float       ly = std::max(vy, y0 - th - 2);
          gl.rect(x0, ly, tw + 10, th + 2, {c.r * 0.6f, c.g * 0.6f, c.b * 0.6f, 0.85f});
          gl.text(x0 + 5, ly + 1, s, ts, {1, 1, 1, 1});
        }
      }
    }
    if (L.stats) {  // video-analytics style: shadowed text in the bottom corners, no panel
      const float  ts = 0.7f, lh = gl.line_height(ts) + 2, m = 24;
      const Rgba   white{1, 1, 1, 0.95f}, green{0.45f, 1.f, 0.55f, 0.95f}, cyan{0.45f, 0.85f, 1.f, 0.95f};
      const char*  mode = sync ? "SYNC" : "LOW-LATENCY";
      const std::string left[3] = {
          fmt("CAM %5.1f fps  %s  drop %llu", cam_fps.value, src_fmt.c_str(),
              (unsigned long long)(dropped + qdrop + corrupt)),
          fmt("AI  %5.1f fps  YOLOv5-nano  DPU %.1f ms", inf_fps.value, e_dpu.value),
          fmt("OUT %5.1f fps  latency %.0f ms  %s", disp_fps.value, e_lat.value, mode)};
      for (int i = 0; i < 3; i++) gl.text_shadow(m, H - m - (3 - i) * lh, left[i], ts, green);
      const std::string right[3] = {
          decode ? fmt("PRE %.1f ms  DEC %.1f ms x%d  POST %.1f ms", e_pre.value, dec_ms, opt.decoders, e_post.value)
                 : fmt("PRE %.1f ms  POST %.1f ms", e_pre.value, e_post.value),
          fmt("CPU %.0f %%  OBJECTS %zu", cpu.value, res ? res->boxes.size() : size_t(0)),
          "ENCLUSTRA XU1 + ME-ST1  " + hw};
      for (int i = 0; i < 3; i++) {
        gl.text_shadow(W - m - gl.text_width(right[i], ts), H - m - (3 - i) * lh, right[i], ts,
                       i == 2 ? cyan : white);
      }
    }

    std::string shot;
    {
      std::lock_guard<std::mutex> g(L.shot_m);
      shot.swap(L.shot);
    }
    if (shot.empty() && !shot_done && !opt.shot_path.empty() && now_ms() - t_start > opt.shot_at * 1000) {
      shot = opt.shot_path;
      shot_done = true;
    }
    gl.end(shot);
    if (!shot.empty()) log("shot: %s", shot.c_str());
    rendered++;
    if (new_frame && tsc > 0) e_lat.add(now_ms() - tsc);  // capture timestamp -> buffer swap

    if (now_ms() - t_stat > 5000) {
      t_stat = now_ms();
      log("stats: cam %.1f fps drops %llu bad %llu | %spre %.2f ms dpu %.1f ms post %.2f ms | infer %.1f fps | "
          "display %.1f fps | capture->display %.0f ms | cpu %.0f%% | objects %zu",
          cam_fps.value, (unsigned long long)dropped, (unsigned long long)corrupt,
          decode ? fmt("dec %.1f ms late %llu qdrop %llu | ", dec_ms, (unsigned long long)late,
                       (unsigned long long)qdrop).c_str() : "",
          e_pre.value, e_dpu.value, e_post.value, inf_fps.value, disp_fps.value, e_lat.value, cpu.value,
          res ? res->boxes.size() : size_t(0));
    }
  }

  g_run = false;
  S.cv.notify_all();
  jobs_cv.notify_all();
  cap.join();
  for (auto& d : decoders) d.join();
  inf.join();
  ctl.join();
  cam.stop();
  log("done: rendered %llu frames, captured %llu, inferred %llu, dropped %llu, corrupt %llu, "
      "decoded late %llu, decode-queue drops %llu",
      (unsigned long long)rendered, (unsigned long long)S.captured, (unsigned long long)S.inferred,
      (unsigned long long)S.dropped, (unsigned long long)S.corrupt, (unsigned long long)S.late,
      (unsigned long long)S.qdrop);
  return 0;
}
