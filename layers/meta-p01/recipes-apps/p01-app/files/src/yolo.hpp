// p01-app - YOLOv5 post-processing (Vitis AI Model Zoo yolov5_nano_pt, COCO, 640x640)
#pragma once

#include <string>
#include <vector>

#include "dpu_runner.hpp"

namespace p01 {

struct Box {
  float x0, y0, x1, y1;  // tensor pixels (640x640 letterbox)
  float score;
  int   cls;
};

// Decodes the three YOLOv5 heads (grid 80 / 40 / 20, 3 anchors x 85 channels each) and applies
// class-wise NMS. Box decoding: xy = (2 sigmoid(t) - 0.5 + grid) * stride, wh = (2 sigmoid(t))^2 * anchor.
std::vector<Box> yolov5_decode(const std::vector<DpuRunner::Head>& heads, int input_size,
                               float conf_thr, float nms_thr, int max_boxes = 100);

const char* coco_name(int cls);

}  // namespace p01
