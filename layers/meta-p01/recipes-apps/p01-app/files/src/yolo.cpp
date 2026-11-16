// p01-app - YOLOv5 post-processing
#include "yolo.hpp"

#include <algorithm>
#include <cmath>

namespace p01 {

namespace {

constexpr int kAnchors = 3;
constexpr int kNo      = 85;  // x, y, w, h, objectness, 80 classes

// YOLOv5 default anchors (yolov5_nano_pt.prototxt "biases"), by stride
const float* anchors_for_stride(int stride) {
  static const float a8[]  = {10, 13, 16, 30, 33, 23};
  static const float a16[] = {30, 61, 62, 45, 59, 119};
  static const float a32[] = {116, 90, 156, 198, 373, 326};
  return stride <= 8 ? a8 : stride <= 16 ? a16 : a32;
}

inline float sigmoid(float x) { return 1.f / (1.f + std::exp(-x)); }

float iou(const Box& a, const Box& b) {
  const float ix = std::max(0.f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
  const float iy = std::max(0.f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
  const float inter = ix * iy;
  const float u = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - inter;
  return u > 0.f ? inter / u : 0.f;
}

const char* const kCoco[80] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog",
    "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
    "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
    "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors",
    "teddy bear", "hair drier", "toothbrush"};

}  // namespace

const char* coco_name(int cls) { return (cls >= 0 && cls < 80) ? kCoco[cls] : "?"; }

std::vector<Box> yolov5_decode(const std::vector<DpuRunner::Head>& heads, int input_size,
                               float conf_thr, float nms_thr, int max_boxes) {
  std::vector<Box> cand;
  const float logit_thr = std::log(conf_thr / (1.f - conf_thr));  // obj >= conf is necessary
  for (const auto& hd : heads) {
    if (hd.c != kAnchors * kNo) continue;
    const int    stride = input_size / hd.h;
    const float* anc    = anchors_for_stride(stride);
    const int8_t obj_q  = int8_t(std::clamp(std::ceil(logit_thr / hd.scale), -128.f, 127.f));
    for (int gy = 0; gy < hd.h; gy++) {
      for (int gx = 0; gx < hd.w; gx++) {
        const int8_t* cell = hd.data + (size_t(gy) * hd.w + gx) * hd.c;
        for (int a = 0; a < kAnchors; a++) {
          const int8_t* p = cell + a * kNo;
          if (p[4] < obj_q) continue;
          int best = 0;
          for (int k = 1; k < 80; k++) {
            if (p[5 + k] > p[5 + best]) best = k;
          }
          const float score = sigmoid(p[4] * hd.scale) * sigmoid(p[5 + best] * hd.scale);
          if (score < conf_thr) continue;
          const float cx = (sigmoid(p[0] * hd.scale) * 2.f - 0.5f + gx) * stride;
          const float cy = (sigmoid(p[1] * hd.scale) * 2.f - 0.5f + gy) * stride;
          const float sw = sigmoid(p[2] * hd.scale) * 2.f;
          const float sh = sigmoid(p[3] * hd.scale) * 2.f;
          const float w  = sw * sw * anc[2 * a];
          const float h  = sh * sh * anc[2 * a + 1];
          cand.push_back({cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, score, best});
        }
      }
    }
  }
  std::sort(cand.begin(), cand.end(), [](const Box& a, const Box& b) { return a.score > b.score; });
  std::vector<Box> keep;
  std::vector<bool> removed(cand.size(), false);
  for (size_t i = 0; i < cand.size() && int(keep.size()) < max_boxes; i++) {
    if (removed[i]) continue;
    keep.push_back(cand[i]);
    for (size_t j = i + 1; j < cand.size(); j++) {
      if (!removed[j] && cand[j].cls == cand[i].cls && iou(cand[i], cand[j]) > nms_thr) removed[j] = true;
    }
  }
  return keep;
}

}  // namespace p01
