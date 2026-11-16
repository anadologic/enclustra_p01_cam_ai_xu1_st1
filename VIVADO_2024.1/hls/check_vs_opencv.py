#!/usr/bin/env python3
"""Compare p01_preproc outputs with the Vitis AI YOLOv5 preprocessing done in OpenCV.

Reference (vai_library yolov3 utils.cpp, letterbox()):
    cv::resize(img, (act_w, act_h), INTER_LINEAR) into a pad-114 canvas at (act_x, act_y),
    then int8 = (pixel - mean) * scale * 2^fix   (round, or truncate as the library default).

Input: the raw dumps written by tb_p01_preproc (csim) in work/hls/dump.
Exact equality is not expected: OpenCV and the Vitis Vision functions (xf::cv::yuyv2rgb Q15,
xf::cv::resize bilinear) use different fixed-point rounding. The check requires |diff| <= 1
everywhere and reports the mean signed difference (bias) of kernel - OpenCV.

Usage: python3 check_vs_opencv.py <dump_dir>   (needs numpy + cv2, e.g. in the Vitis AI docker)
"""
import glob
import os
import sys

import cv2
import numpy as np


def load_params(path):
    with open(path) as f:
        return {k: int(v) for k, v in (kv.split("=") for kv in f.read().split())}


def opencv_reference(src, p):
    # YUYV -> RGB (as cv::cvtColor in the camera path), then letterbox
    rgb = cv2.cvtColor(src.reshape(p["src_h"], p["src_w"], 2), cv2.COLOR_YUV2RGB_YUYV)
    img = cv2.resize(rgb, (p["act_w"], p["act_h"]), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((p["dst_h"], p["dst_w"], 3), p["pad"], np.uint8)
    canvas[p["act_y"]:p["act_y"] + p["act_h"], p["act_x"]:p["act_x"] + p["act_w"]] = img
    val = canvas.astype(np.float64) * 0.00392156 * (1 << p["fix"])
    q = np.floor(val + 0.5)
    return np.clip(q, -128, 127).astype(np.int8)


def main(dump_dir):
    worst = 0
    for params_file in sorted(glob.glob(os.path.join(dump_dir, "*_params.txt"))):
        name = os.path.basename(params_file)[: -len("_params.txt")]
        p = load_params(params_file)
        bpp = 2  # YUYV
        raw = np.fromfile(os.path.join(dump_dir, name + "_src.raw"), np.uint8)
        src = np.ascontiguousarray(raw.reshape(p["src_h"], p["stride"])[:, : p["src_w"] * bpp])
        out = np.fromfile(os.path.join(dump_dir, name + "_out.raw"), np.int8)
        out = out.reshape(p["dst_h"], p["dst_w"], 3)
        ref = opencv_reference(src, p)
        sdiff = out.astype(np.int16) - ref.astype(np.int16)
        diff = np.abs(sdiff)
        exact = 100.0 * np.mean(diff == 0)
        print(f"{name:19s} max|diff| = {diff.max()}  exact = {exact:6.2f} %  "
              f"within 1 = {100.0 * np.mean(diff <= 1):6.2f} %  bias = {sdiff.mean():+.4f}  "
              f"(+1: {100.0 * np.mean(sdiff > 0):5.2f} %, -1: {100.0 * np.mean(sdiff < 0):5.2f} %)")
        worst = max(worst, int(diff.max()))
    print("OPENCV CHECK", "PASSED" if worst <= 1 else "FAILED", f"(worst |diff| = {worst})")
    return 0 if worst <= 1 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "work/hls/dump"))
