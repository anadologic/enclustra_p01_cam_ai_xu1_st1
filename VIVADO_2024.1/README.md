# Vivado 2024.1 Hardware Design (ME-XU1-6EG + ME-ST1)

FPGA design of the camera AI pipeline (USB camera, YUYV frames in DDR). **Status: implemented.**
See [Verification](#verification) for the results.

| Target | Value |
|--------|-------|
| SoM    | ME-XU1-6EG-1I-D12E-NA-R5.0, Vivado part **`xczu6eg-ffvc900-1-i`** |
| Carrier| ME-ST1 |
| Tools  | Vivado 2024.1, Vitis HLS 2024.1, AMD Vitis Vision library (Vitis Libraries `v2024.1_update2`) |
| License | Vivado ML Enterprise (the XCZU6EG is not supported by the free Standard Edition), see [Build](#build) |
| PS     | [board/](board/): `board_settings.tcl` (part, DDR preset), `ps_config.tcl` (PS configuration from the Enclustra Mercury XU1 ST1 reference design 2024.1_v1.1.3, with the PS clock fixes of 2024.2_v1.2.3), `ME_XU1_6EG_ST1.xdc` (board pins) |
| Output | `export/p01_xu1_st1.xsa` (with bitstream) + `export/arch.json`, written by the build (not in the repository). The XSA is the input of [../PETALINUX_2024.1](../PETALINUX_2024.1/) and [../YOCTO_2024.1](../YOCTO_2024.1/) |

---

## Build

> **License:** the free Vivado ML Standard Edition does not support the XCZU6EG (its Zynq
> UltraScale+ list covers ZU1 to ZU5 and ZU7), so `synth_design` and the implementation steps need a
> Vivado ML Enterprise license (node-locked or floating; AMD also offers a 30-day evaluation
> license; from Vivado 2026.1 a tier that covers the XCZU6EG). Without it, synthesis stops with
> `[Common 17-345] A valid license was not found for feature 'Synthesis' and/or device 'xczu6eg'`.
> The XSA is not in the repository: this flow has to run before the Linux images can be built.

```bash
cd VIVADO_2024.1
XILINX_ROOT=/tools/Xilinx scripts/build_all.sh   # everything below, in order (XILINX_ROOT: folder with Vivado/2024.1, Vitis_HLS/2024.1)
HLS_ARGS=nocosim scripts/build_all.sh            # the same without C/RTL co-simulation
# or step by step (Vivado + Vitis HLS 2024.1 settings64.sh sourced):
scripts/fetch_dpu_ip.sh              # AMD DPUCZDX8G v4.1 -> ip_repo/ (download, MD5 checked)
scripts/fetch_vitis_vision.sh        # AMD Vitis Vision L1 -> ext/ (pinned commit, sparse checkout)
vitis_hls -f hls/run_hls.tcl         # p01_preproc: csim, csynth, cosim, IP -> ip_repo/
vivado -mode batch -source scripts/create_project.tcl   # -> work/p01_xu1_st1.xpr
vivado -mode batch -source scripts/build.tcl            # bitstream, timing + fingerprint check, XSA
```

Needs internet access for the two downloads. `ip_repo/`, `ext/`, `work/` and `export/` are generated
and are not part of the repository. The DPU IP is AMD proprietary and the Vitis Vision library is an
external repository, so both are downloaded rather than committed.

**PL preprocessing:** [PREPROC_VITIS_VISION.md](PREPROC_VITIS_VISION.md) lists every repository, IP
version, library function and build step of the preprocessing pipeline.

## Files

| Path | Purpose |
|------|---------|
| `scripts/settings.tcl` | Project name, release ID, DPU clock and fingerprint; sources `board/board_settings.tcl` |
| `board/` | ME-XU1-6EG + ME-ST1: board settings, PS configuration (`apply_ps_config`), board pin constraints |
| `scripts/fetch_dpu_ip.sh` | Downloads the Vitis AI 3.0 DPU TRD and extracts `DPUCZDX8G_v4_1_0` |
| `scripts/fetch_vitis_vision.sh` | Downloads Vitis Libraries `v2024.1_update2` (commit `3c8a3d5`), `vision/L1` only |
| `scripts/create_project.tcl`, `scripts/build.tcl`, `scripts/build_all.sh` | Project creation, build, full flow |
| `bd/dpu_hier.tcl` | `hier_dpu`: DPU + clocking wizard + resets + IRQ concat, derived from the TRD |
| `bd/bd.tcl` | Block design |
| `hls/p01_preproc.{hpp,cpp}` | Preprocessing kernel 2.1: AXI4-Stream Video in / out (YUYV in, RGB out), built from Vitis Vision functions |
| `hls/p01_preproc_ref.hpp` | Reference model (Vitis AI preprocessing, equal to OpenCV within 1 LSB). Also the source for the letterbox geometry, registers, test pattern and box mapping in Linux. |
| `hls/tb_p01_preproc.cpp`, `hls/run_hls.tcl` | Testbench (models Frame Buffer Read / Write streams) and HLS flow |
| `hls/check_vs_opencv.py` | Compares the kernel output with the OpenCV-based Vitis AI preprocessing |
| `hdl/p01_xu1_st1_top.vhd` | Top level (board I/O: DisplayPort AUX / HPD, I2C FPGA bus) |
| `constraints/p01_xu1_st1.xdc` | Project constraints (bitstream compression). Board pins: `board/ME_XU1_6EG_ST1.xdc`. |

---

## Block design

```
                     ┌──────────────────────── zynq_ultra_ps_e_0 (PS) ─────────────────────────────┐
 pl_clk0 100 MHz ────┤ M_AXI_HPM0_LPD ──► hier_dpu/S_AXI (DPU registers)                              │
                     │ M_AXI_HPM0_FPD ──► axi_smc_ctrl ─┬► p01_preproc/s_axi_control                  │
                     │                                  ├► version_reg (AXI GPIO, read-only)          │
                     │                                  ├► fb_rd/s_axi_CTRL, fb_wr/s_axi_CTRL         │
                     │                                  └► video_rst (AXI GPIO, 1 output)             │
                     │ S_AXI_HP0_FPD  ◄── DPU data 0     S_AXI_HP1_FPD ◄── DPU data 1                  │
                     │ S_AXI_HP2_FPD  ◄── DPU softmax    S_AXI_LPD     ◄── DPU instructions            │
                     │ S_AXI_HP3_FPD  ◄── axi_smc_video ◄── fb_rd + fb_wr (AXI-MM 64 bit)              │
                     │ pl_ps_irq0[4:0] ◄── {p01_preproc, fb_rd, fb_wr, DPU softmax, DPU core 0}         │
                     └──────────────────────────────────────────────────────────────────────────────┘
 Video pipeline (AXI4-Stream Video, 1 pixel / clock, 24 bit):
   fb_rd (Video Frame Buffer Read 2.5, YUYV8) ─► p01_preproc 2.1 (Vitis Vision) ─► fb_wr (Video Frame Buffer Write 2.5, RGB8)
   rst_video (Processor System Reset): DPU_RSTn + video_rst GPIO -> ap_rst_n of the three video IPs
 hier_dpu: clk_wiz (pl_clk0 → 275 MHz clk_dpu + 550 MHz clk_dsp with CE) + DPUCZDX8G 1x B4096
 DPU_CLK (275 MHz) also clocks the video pipeline, both SmartConnects, the GPIOs and all HP/LPD ports.
```

### Address map

| Address | Size | Slave | Master port |
|---------|------|-------|-------------|
| `0x8F00_0000` | 16 MB | DPUCZDX8G registers | M_AXI_HPM0_LPD (as in the DPU TRD) |
| `0xA000_0000` | 64 KB | `p01_preproc` control | M_AXI_HPM0_FPD |
| `0xA001_0000` | 64 KB | `version_reg`: `0x00` = magic `0x50303120` ("P01 "), `0x08` = version `0x00020100` (2.1.0) | M_AXI_HPM0_FPD |
| `0xA002_0000` | 64 KB | `fb_rd` (Video Frame Buffer Read) control | M_AXI_HPM0_FPD |
| `0xA003_0000` | 64 KB | `fb_wr` (Video Frame Buffer Write) control | M_AXI_HPM0_FPD |
| `0xA004_0000` | 64 KB | `video_rst` GPIO: bit 0 = 1 run (default), 0 = reset of the video pipeline | M_AXI_HPM0_FPD |

The DPU masters and the Frame Buffer masters see DDR low (2 GB) + DDR high (2 GB at `0x8_0000_0000`).

### Interrupts

| `pl_ps_irq0` bit | GIC SPI | Source |
|------------------|---------|--------|
| 0 | 89 | DPU core 0 (TRD position, expected by the zocl / VART device tree) |
| 1 | 90 | DPU softmax |
| 2 | 91 | `fb_wr` frame done |
| 3 | 92 | `fb_rd` frame done |
| 4 | 93 | `p01_preproc` done |

The application polls ap_done; the interrupts are wired for later use.

### DPU

| Parameter | Value |
|-----------|-------|
| IP | DPUCZDX8G v4.1 (Vitis AI 3.0 TRD; Vitis AI v4.0 lists it as verified with 2024.1) |
| Configuration | B4096, 1 core, RAM usage low, channel augmentation, depthwise conv, average pool, ReLU + LeakyReLU + ReLU6, softmax, DSP48 usage high, no URAM, clock gating |
| Clocks | 275 MHz (`m_axi_dpu_aclk`), 550 MHz (`dpu_2x_clk`), 100 MHz register clock. 300 / 600 MHz also closed timing, but with only +0.001 ns WNS on the DSP clock (-1 speed grade), so 275 MHz is used for margin. |
| Resources (IP estimate) | 724 DSP, 259 BRAM36 |
| **Fingerprint** | **`0x101000056010407`**. Equal to the Vitis AI Model Zoo `yolov5_nano_pt` xmodel for zcu102 / zcu104 / kv260, so the xmodel runs without recompiling. `build.tcl` fails if `arch.json` differs. |

### Video pipeline

| IP | Configuration |
|----|---------------|
| `fb_rd` Video Frame Buffer Read 2.5 | 1 sample / clock, 8 bit, max 2048 × 2048, memory format YUYV8 (12), 64-bit addresses, AXI-MM 64 bit |
| `p01_preproc` 2.1 (HLS) | `AXIvideo2xfMat` → `yuyv2rgb` → `resize` (bilinear) → `insertBorder` → `preProcess` → `xfMat2AXIvideo` (Vitis Vision), project glue for the AXI4-Stream RGB order |
| `fb_wr` Video Frame Buffer Write 2.5 | 1 sample / clock, 8 bit, max 1024 × 1024, memory format RGB8 (20) = bytes R, G, B, 64-bit addresses |

Registers set by Linux (values for the default USB camera mode, C270 MJPEG 1280×960 decoded to
YUYV → YOLOv5-nano 640×640):

| Block | Offset | Register | Value |
|-------|--------|----------|-------|
| `fb_rd` | `0x10` / `0x18` / `0x20` | width / height / stride (bytes, multiple of 8) | 1280 / 960 / 2560 |
| `fb_rd` | `0x28` | memory format | 12 = YUYV8 |
| `fb_rd` | `0x30` (64 bit) | frame address | CMA slot |
| `p01_preproc` | `0x10` / `0x18` | `src_w` / `src_h` | 1280 / 960 |
| `p01_preproc` | `0x20` / `0x28` | `rsz_w` / `rsz_h` (letterbox size, centred by `insertBorder`) | 640 / 480 |
| `p01_preproc` | `0x30` / `0x38` | `dst_w` / `dst_h` | 640 / 640 |
| `p01_preproc` | `0x40` | `pad` | 114 |
| `p01_preproc` | `0x48`–`0x58` / `0x60`–`0x70` | `alpha0..2` / `beta0..2` (float): int8 = round((pixel − alpha) × beta) | 0 / 0.25098 (scale 1/255 × 2^6) |
| `fb_wr` | `0x10` / `0x18` / `0x20` / `0x28` | width / height / stride / format | 640 / 640 / 1920 / 20 = RGB8 |
| `fb_wr` | `0x30` (64 bit) | tensor address | VART input TensorBuffer |
| all three | `0x00` | AP_CTRL: bit 0 start, bit 1 done (clear on read), bit 2 idle | start `fb_wr`, `p01_preproc`, `fb_rd`; poll `fb_wr` done |

The values come from `p01ref::letterbox_params()` and `p01ref::quant_params()` in
`hls/p01_preproc_ref.hpp`. Box mapping back to camera pixels uses `p01ref::tensor_to_src()`.

---

## Verification

Release **2.1.0** (2026-10-26). The full log is in [PREPROC_VITIS_VISION.md](PREPROC_VITIS_VISION.md).

| Check | Result |
|-------|--------|
| HLS C simulation: 7 YUYV cases (640×480, 1280×960, 1280×720, 1920×1080, 96×64, upscale 38×30, portrait 120×200) vs. the reference model, streams modelled like Frame Buffer Read / Write | **PASS**: max \|diff\| 1 int8 LSB, padding exact, TUSER / TLAST correct. Exact: 640×480 and 1920×1080 99.98 %, resized cases 91.5–93.9 % |
| Kernel vs. OpenCV Vitis AI preprocessing (`check_vs_opencv.py`, Vitis AI 3.5 docker) | **PASS**: max \|diff\| 1 everywhere. The difference is one-sided (kernel lower, bias −0.06…−0.10 LSB) because `xf::cv::resize` truncates the bilinear result where OpenCV rounds. |
| C/RTL co-simulation (3 small cases) | **PASS** |
| HLS synthesis @ 300 MHz target | Estimated Fmax 289.6 MHz; 9 BRAM18, 28 DSP, 12.5k FF, 12.7k LUT (estimate) |
| Vivado implementation 2.1.0 (275 / 550 MHz) | Timing met: WNS **+0.156 ns**, WHS +0.006 ns. No errors, no critical warnings in project creation, synthesis or implementation. |
| Utilization (XCZU6EG) | LUT 72.8k (34 %), BRAM36 268.5 (38 %), DSP 747 (38 %), URAM 0. Video pipeline after routing: `fb_rd` 1.6k LUT / 2 BRAM36, `p01_preproc` 4.1k LUT / 4 BRAM36 + 3 BRAM18 / 28 DSP, `fb_wr` 2.0k LUT / 2 BRAM36 / 1 DSP, `axi_smc_video` 3.3k LUT: about 11.0k LUT in total |
| DPU fingerprint of the built design (`export/arch.json`) | `0x101000056010407`, checked by `build.tcl` |
| Exported | `export/p01_xu1_st1.xsa` (bitstream included), `export/arch.json` |

**On hardware (2026-10-26), `p01-app --pl-test`** (test frames through `fb_rd` → `p01_preproc` →
`fb_wr`, compared with the reference model):

| Case | Time | max \|diff\| | Exact | Padding errors |
|------|------|-----------|-------|----------------|
| YUYV 640×480 | 1.63 ms | 1 | 99.98 % | 0 |
| YUYV 1280×720 | 4.12 ms | 1 | 93.85 % | 0 |
| YUYV 1280×960 (C270 MJPEG mode, decoded) | 4.98 ms | 1 | 91.79 % | 0 |
| YUYV 1920×1080 | 8.33 ms | 1 | 99.98 % | 0 |

The exact percentages equal the C simulation, so the hardware output matches it bit for bit. The
time is one pixel per clock at 275 MHz on the larger of input and output (1280×960 = 1.2 M pixels
≈ 4.5 ms). With the camera (C270 MJPEG 1280×960, `p01` service) the pipeline runs at 30 fps with
5.0 ms PL preprocessing per frame.
