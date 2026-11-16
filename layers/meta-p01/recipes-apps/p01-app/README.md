# p01-app: camera AI application (PetaLinux recipe)

USB camera → PL preprocessing (Frame Buffer Read → `p01_preproc` → Frame Buffer Write) → DPU
YOLOv5-nano → Mali-400 GPU composition → mini DisplayPort, on ME-XU1-6EG + ME-ST1. Built and
installed by this recipe as part of the PetaLinux image of
[p01-xu1-st1](../../../../PETALINUX_2024.1/p01-xu1-st1/) and the Yocto build ([YOCTO_2024.1](../../../../YOCTO_2024.1/)); needs the P01 **2.1** bitstream (`version_reg` 2.1.0).

| Item | Value |
|------|-------|
| Camera | Any USB UVC camera. **MJPEG** (decoded on the CPU) by default, **YUYV** when the camera has no suitable MJPEG mode. Size: the largest mode up to `--max-size` (default **1280×960**) at 30 fps, e.g. 1280×960 on the Logitech C270, 1280×720 on cameras without 1280×960. |
| PL | AMD Video Frame Buffer Read (YUYV8) → `p01_preproc` 2.1 (AMD Vitis Vision: YUYV → RGB, letterbox 640×640, INT8) → AMD Video Frame Buffer Write (RGB8) into the DPU input tensor. See [VIVADO_2024.1/PREPROC_VITIS_VISION.md](../../../../VIVADO_2024.1/PREPROC_VITIS_VISION.md). |
| DPU | DPUCZDX8G B4096 @ 275 MHz, `yolov5_nano_pt` |
| GPU | Mali-400 MP2, OpenGL ES 2.0, EGL **fbdev** backend on `/dev/fb0` (1920×1080, RGB565). Video textures are the CMA ring slots themselves (`EGL_EXT_image_dma_buf_import`, zero copy). |
| Screen | Full-screen video (`--view cover`, default: a non-16:9 source is cropped; `--view fit`: whole frame with bars). Statistics as outlined text in the bottom corners. |

## Recipe

| File | Installed as |
|------|--------------|
| `p01-app.bb` | Builds `files/src` with `files/Makefile` against VART 3.5, XIR, glog, libmali-xlnx (EGL / GLES2) and libjpeg-turbo. `p01_preproc_ref.hpp` is taken from `VIVADO_2024.1/hls` (shared with the HLS testbench). |
| `files/src/` | `/usr/bin/p01-app` |
| `files/p01-run.sh` | `/usr/bin/p01-run.sh`: fbdev libMali first in the library path (links in `/run/p01/lib`), framebuffer console unbound, then `p01-app` |
| `files/p01.service` | `/lib/systemd/system/p01.service`, **disabled by default** |
| YOLOv5-nano xmodel, downloaded at build time (`MODEL_URI` in `p01-app.bb`) | `/usr/share/p01/models/yolov5_nano_pt/` (the default `--model`) |

The recipe is in the shared layer `layers/meta-p01`. PetaLinux adds the package through
`CONFIG_p01-app` (`project-spec/meta-user/conf/user-rootfsconfig`, `configs/rootfs_config`), Yocto
through `YOCTO_2024.1/meta-p01-yocto`. Build only the application with PetaLinux:

```bash
cd PETALINUX_2024.1/p01-xu1-st1
petalinux-build -c p01-app
```

**Model.** The model is not kept in the repository. `do_fetch` downloads the AMD Vitis AI Model Zoo
3.0 archive `yolov5_nano_pt-zcu102_zcu104_kv260-r3.0.0.tar.gz` (checked against its SHA-256 in the
recipe) and the recipe installs `yolov5_nano_pt/yolov5_nano_pt.xmodel` from it (MD5
`54934783deb174e4c782ce7c6bbcf27e`); its fingerprint matches the P01 DPU. YOLOv5 is by Ultralytics
(AGPL-3.0); the model is used here for evaluation and training only.

## Start at power-up (optional)

The service is installed but **not enabled**: after boot nothing starts. To start the demo at
every power-up:

```bash
sudo systemctl enable --now p01      # stop: sudo systemctl disable --now p01
journalctl -u p01 -f                 # camera modes, selected mode, statistics every 5 s
```

The camera options are in `P01_ARGS` in `p01.service` (`--format`, `--size`, `--max-size`,
`--fps`, `--dev`, `--view`, ...; all listed in the file). Change them without editing the
installed file:

```bash
sudo systemctl edit p01
#   [Service]
#   Environment="P01_ARGS=--format mjpeg --size auto --max-size 1280x720 --fps 30"
sudo systemctl restart p01
```

At start the log lists the camera's MJPEG and YUYV modes (size and highest fps) and the selected
one, for example (Logitech C270):

```
camera: MJPEG modes: 640x480@30 ... 1280x720@30 1280x960@30
camera: YUYV modes: 640x480@30 ... 1280x720@8 1280x960@8
camera: selected MJPEG 1280x960 (auto: largest up to 1280x960 at 30 fps)
```

## Run by hand

```bash
sudo p01-run.sh                                   # defaults: --format mjpeg --size auto --max-size 1280x960
sudo p01-run.sh --format yuyv --size 640x480      # no decoding: lowest CPU load and latency
sudo p01-run.sh --pl-test                         # PL pipeline self-test (no camera, no DPU)
```

Options: `--dev /dev/video0 --format mjpeg|yuyv --size auto|WxH --max-size 1280x960 --fps 30
--model file.xmodel --conf 0.4 --nms 0.45 --lowlat --view cover|fit --seconds N
--shot <after_s> <file.ppm>`. Test and measurement modes: `--pl-test`, `--test-display`,
`--capture-only [--record file]`, `--mjpeg-file file.mjpg` (an MJPEG file played as a camera),
`--bench-mjpeg file.mjpg`, `--sim-cameras N --mjpeg-file file.mjpg [--decoders D]`.

C270 through `p01.service` (2026-10-26): MJPEG 1280×960 (auto) → 30 fps, decode
22–23 ms, PL preprocessing 5.0 ms, CPU 22 %, capture → display 94–101 ms; `--max-size 1280x720` →
MJPEG 1280×720; `--format yuyv` → YUYV 640×480, 28–30 fps, CPU 5 %, 71 ms. The DPU input is the
same 640×640 letterbox in all cases; MJPEG 1280×960 gives the sharpest picture on screen.

Screen overlay (toggle with `stats on|off`): bottom left `CAM` (camera fps, format, drops), `AI`
(inference fps, DPU time), `OUT` (display fps, capture → display latency, SYNC / LOW-LATENCY);
bottom right PL preprocessing / MJPEG decode / post-processing times, CPU load, object count,
board and release.

Runtime control (overlay layers on / off, mode switch, screenshot):

```bash
echo "boxes off"      > /tmp/p01.ctl    # video | boxes | labels | stats  on | off | toggle
echo "mode lowlat"    > /tmp/p01.ctl    # mode sync | lowlat
echo "shot /tmp/a.ppm" > /tmp/p01.ctl
echo quit             > /tmp/p01.ctl
```

## Data path

```
 USB camera ──► uvcvideo (V4L2 mmap) ──► MJPEG: copy ─► decode threads (libjpeg-turbo → YUYV)
                                     └─► YUYV:  copy ─────────────────────────────┐
                                                                                   ▼
                                         CMA ring slot (8 slots, DMA-BUF CMA heap, cache cleaned)
                                                   │ physical address (pagemap)
                                                   ▼
               fb_rd ─AXI4-Stream─► p01_preproc ─AXI4-Stream─► fb_wr (PL, /dev/mem registers)
                                                   │
                                                   ▼
               VART input tensor (TensorBuffer::data_phy, zero copy)
                                                   ▼
               DPU (VART RunnerExt) ──► 3 YOLO heads ──► decode + NMS (CPU) ──► boxes in camera pixels
                                                   ▼
 same CMA slot ──► EGLImage texture (dma-buf, no copy; YUYV → RGB in the shader) + boxes / labels /
                   stats text ──► fbdev ──► DP
```

| Thread | Work |
|--------|------|
| capture | V4L2 dequeue; YUYV: copy into the next CMA slot; MJPEG: copy the compressed frame into the decode queue |
| decode (MJPEG, `--decoders`, default 2) | libjpeg-turbo raw decode → limited-range YUYV in the CMA slot |
| infer | Newest frame: PL pipeline (start `fb_wr`, `p01_preproc`, `fb_rd`; poll `fb_wr` done; reset + reconfigure on timeout) → DPU → decode / NMS → boxes mapped back with `p01ref::tensor_to_src()` |
| main | Composition at display rate. **SYNC** (default): the frame the boxes belong to. **LOW-LATENCY**: the newest frame with the latest boxes. |

## Measured on hardware (ME-XU1-6EG-1I-D12E + ME-ST1)

10-minute run, Logitech C270 YUYV 640×480, SYNC mode (`--seconds 600`, 2026-10-22):

| Item | Result |
|------|--------|
| Camera | 28.6–30.1 fps |
| Frames delivered / inferred | 17 708 / 17 707 |
| Frames skipped by the camera / UVC | 110 (0.6 %) |
| Incomplete frames (`V4L2_BUF_FLAG_ERROR`, never shown) | 0 |
| PL preprocessing (YUYV → RGB + letterbox + INT8, start → tensor in DDR) | 1.64–1.66 ms |
| DPU YOLOv5-nano (B4096 @ 275 MHz, VART run + wait) | 8.2–8.4 ms |
| Decode + NMS (CPU) | 3.1–5.4 ms (more objects in the scene) |
| Capture → display (V4L2 timestamp → `eglSwapBuffers`), SYNC | 68–77 ms |
| Zero copy | Frame Buffer Write writes the VART input tensor (HOST_PHY) |
| Errors / timeouts | none |

The camera skips frames at the C270 / UVC level: the same happens with `--capture-only` (no
processing) and `uvcvideo` debugfs stats show 0 errors. Capture → display includes the ≈ 33 ms
USB transfer of a frame and up to one display refresh.

`--pl-test`: YUYV 640×480 1.63 ms, 1280×720 4.12 ms, 1280×960 4.98 ms, 1920×1080 8.33 ms, all
within 1 LSB of the reference model with exact padding (details in
[PREPROC_VITIS_VISION.md](../../../../VIVADO_2024.1/PREPROC_VITIS_VISION.md)).

**Cache maintenance:** `DMA_BUF_IOCTL_SYNC` on a CMA heap buffer only maintains the caches for
attached devices, and the PL uses the buffers by physical address. `CmaBuffer` therefore also runs
`DC CIVAC` over the buffer in `end_cpu_write()` / `begin_cpu_read()`; without it the PL can read
stale lines of a camera frame (`--pl-test` checks for this).

## MJPEG cameras (CPU decode)

USB 2.0 webcams deliver 1080p30 only as MJPEG. ZU6EG has no VCU, so `--format mjpeg` decodes the
frames on the CPU and feeds the unchanged YUYV pipeline:

```
camera (MJPEG) -> copy of the compressed frame -> queue (max 2 x decoders, oldest dropped)
  -> N decode threads: libjpeg-turbo raw Y/Cb/Cr (no colour conversion) -> NEON pack to YUYV
     in the CMA slot -> cache clean -> Frame Buffer Read (YUYV8) -> p01_preproc -> DPU, GPU
```

- libjpeg-turbo 2.1.5.1 (`jpeg` in `DEPENDS`, `libjpeg.so.62` in the image). Missing Huffman tables
  (UVC MJPEG) are filled in by libjpeg-turbo.
- Supported layouts: 4:2:2 as Y H2V1 + chroma 1×1 (webcams, C270) or Y H2V2 + chroma 1×2 (ffmpeg),
  and 4:2:0 (chroma rows repeated).
- `--mjpeg-file` plays a recorded or generated MJPEG file at `--fps` like a camera (1080p tests
  without a 1080p camera); `--capture-only --format mjpeg --record f.mjpg` records camera frames.

Measured on the board (2026-10-23), decode + YUYV pack per frame on one A53 core,
`--bench-mjpeg` (scales linearly up to 4 threads):

| Stream | Frame size | ISLOW | Cores for 30 fps |
|--------|-----------|-------|------------------|
| C270 MJPEG 1280×960 (real camera, 4:2:2) | 83 KB | 19.3 ms | 0.6 |
| 1080p 4:2:2 (C270 recording upscaled + noise, ffmpeg q6) | 68 KB | 25.5 ms | 0.8 |
| 1080p 4:2:2, q3 | 139 KB | 29.8 ms (IFAST 23.9 ms) | 0.9 |
| 1080p 4:2:2, q2 (high quality) | 328 KB | 40.5 ms | 1.2 |

Full pipeline, 2 decode threads, SYNC mode:

| Source | Camera / inference | Decode | PL preprocessing | Capture → display | CPU (4 cores) | Video on screen |
|--------|--------------------|--------|------------------|-------------------|---------------|-----------------|
| C270 MJPEG 1280×960 live (`p01` service) | 30 / 30 fps, 0 lost | 20–23 ms | 5.0 ms | 94–101 ms | 22 % | 30 fps |
| 1080p MJPEG file (q3) at 30 fps | 30 / 30 fps, 0 lost, 0 late | 30.3 ms | 8.42 ms | 72 ms | 28 % | 30 fps |
| C270 YUYV 640×480 live | 30 / 30 fps | - | 1.65 ms | 72–77 ms | 6 % | 30 fps |

### CPU capacity for 1080p30 MJPEG cameras (2026-10-26)

`--sim-cameras N --mjpeg-file f.mjpg --decoders D --seconds 20`: N cameras each deliver a frame
every 1/30 s (copied like a camera buffer), D threads decode into CMA slots with the application's
cache maintenance; frames are dropped when the queue is full. Streams: the C270 recorded in MJPEG
1280×960, cropped to 16:9, scaled to 1920×1080 with sensor-like noise, 4:2:2, two JPEG qualities.

| Cores | Cameras | Frame size | DCT | Delivered | Dropped | Decode latency avg / max | CPU busy |
|-------|---------|-----------|-----|-----------|---------|--------------------------|----------|
| 4 | 1 | 189 KB | ISLOW | 30.0 fps | 0 | 32.6 / 33.1 ms | 0.97 cores (24 %) |
| 4 | 1 | 383 KB | ISLOW | 30.0 fps | 0 | 43.2 / 43.6 ms | 1.31 cores (33 %) |
| 4 | 2 | 189 KB | ISLOW | 2 × 30.0 fps | 0 | 33.0 / 40.0 ms | 1.99 cores (50 %) |
| 4 | 2 | 383 KB | ISLOW | 2 × 30.0 fps | 0 | 43.8 / 55.0 ms | 2.59 cores (65 %) |

Per frame and core, decode time follows t ≈ 21.6 ms + 0.058 ms × frame KB (189 KB → 32.6 ms,
383 KB → 43.8 ms predicted; 32.6 / 43.2 ms measured). Conclusions:

- **One 1080p30 MJPEG camera uses about 1 to 1.3 of the 4 cores**; two would also fit (50–65 %).
- The decisive number is the camera's real frame size: record it (`--capture-only --format mjpeg
  --record f.mjpg`) and repeat the test with that file.

### Display path

- **Pixel selection.** A YUYV texel holds two pixels. The Mali-400 fragment shader computes in
  fp16, which cannot resolve `floor(v_uv.x * width)` (above 1024 not even whole pixels); choosing
  Y0 / Y1 that way takes luma from the neighbouring pixel on scaled video (jagged, vertically
  striped edges). The shader therefore does no arithmetic on texture coordinates: the Y0 / Y1
  choice comes from a `width x 1` parity texture (0, 1, 0, 1 ...) looked up with the same
  coordinate. Check on a 1080p frame shown 1:1: screen vs. expected RGB mean difference 0.25, 0 % of
  14 000 edge pixels closer to the swapped-pixel result, in both screen halves.
- **Scaling.** A non-1:1 source (C270 640×480 → 3×) is converted YUV → RGB at its own size into an
  offscreen texture and then drawn bilinearly (two passes). 1:1 (1080p) is drawn directly.
  Importing the buffer as a per-pixel luma view (DRM `GR88`) for bilinear sampling in one pass is
  tried first, but libmali r9p0 rejects it.
- **MJPEG colour range.** JPEG YCbCr is full range; the camera YUYV path, `yuyv2rgb` in the PL and
  the shader use BT.601 limited range. The MJPEG decoder rescales while packing
  (Y' = Y·220/256 + 16, C' = C·225/256 + 16), so MJPEG and YUYV sources have the same range, on
  screen and at the DPU input.
- **Video textures.** libmali-xlnx r9p0 (fbdev) supports `EGL_EXT_image_dma_buf_import` +
  `GL_OES_EGL_image`: each CMA slot is imported once as a 960×1080 ABGR8888 EGLImage (the YUYV
  bytes) and the shader reads it directly. A per-frame `glTexSubImage2D` upload of the 4 MB 1080p
  frame instead limits the screen to about 22 fps and adds about 40 ms of latency (measured with a
  1080p MJPEG file).

Test modes: `--capture-only` (camera frame rate and sequence gaps), `--test-display` (GPU path only),
`--pl-test` (PL pipeline only: YUYV test frames 640×480, 1280×720, 1280×960 and 1920×1080 through
the hardware, compared with the reference model; no camera or DPU needed).

## Files

| File | Purpose |
|------|---------|
| `files/src/main.cpp` | Options, threads, composition layout, statistics, control FIFO, test modes |
| `files/src/camera_v4l2.*` | UVC capture (MJPEG or YUYV), mode discovery and auto size, fixed frame rate (`exposure_auto_priority` = 0) |
| `files/src/mjpeg.*` | MJPEG → YUYV decoder (libjpeg raw data + NEON pack, full → limited range), MJPEG stream splitter |
| `files/src/cma_buffer.*` | DMA-BUF CMA heap buffers with physical address (contiguity checked), cache maintenance |
| `files/src/pl_regs.*` | Frame Buffer Read / Write, `p01_preproc`, `video_rst` and `version_reg` registers (`/dev/mem`), polled `ap_done` |
| `files/src/dpu_runner.*` | VART RunnerExt, zero-copy input (`data_phy`), output fix-point scales |
| `files/src/yolo.*` | YOLOv5 decode (int8 objectness pre-filter) + class-wise NMS, COCO names |
| `files/src/renderer.*` | EGL fbdev + GLES2: dma-buf video textures, YUYV video shader, batched rectangles and text |
| `files/src/font_atlas.hpp` | Generated glyph atlas (DejaVu Sans Mono) |
| `VIVADO_2024.1/hls/p01_preproc_ref.hpp` | Shared with the HLS testbench: letterbox geometry, quantisation parameters, test pattern, reference output, box mapping |
