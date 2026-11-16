# USB Camera AI Inference Pipeline on Enclustra ME-XU1-6EG + ME-ST1

A single-board AI video pipeline on a Zynq UltraScale+ MPSoC. A USB camera is captured with V4L2,
preprocessed in the PL with AMD Video Frame Buffer IPs and AMD Vitis Vision library functions, run
through YOLOv5-nano on the DPU, and composited with the detections by the Mali-400 GPU on the mini
DisplayPort output.

Release **2.1.1** (hardware release 2.1.0, `version_reg` 2.1.0): the hardware (Vivado 2024.1) and the Linux image are built from this repository
with the commands below. The Linux image can be built with **PetaLinux 2024.1** or with **plain Yocto**
(AMD rel-v2024.1); both use the same layers from `layers/` and give the same image. The application,
its model and its systemd service are part of the image. File, IP and application names use the
prefix `p01`.

---

## Hardware

| Item | Part | Notes |
|------|------|-------|
| SoM | Enclustra Mercury+ **ME-XU1-6EG-1I-D12E-NA-R5.0** | `xczu6eg-ffvc900-1-i`, 4 GB PS DDR4 |
| Carrier | Enclustra Mercury+ **ME-ST1** | Boot from SD card |
| Camera | USB UVC camera with MJPEG, e.g. **Logitech C270** (tested) | ST1 USB port. C270: MJPEG up to 1280×960 @ 30 fps |
| Display | mini DisplayPort monitor, 1920×1080 | PS DisplayPort controller |
| Other | SD card (≥ 4 GB), USB cable for the ST1 UART (115200 8N1), Ethernet cable to the PC | - |

## Tools

| Tool | Version | Used for |
|------|---------|----------|
| AMD Vivado, Vitis HLS | 2024.1; **Vivado ML Enterprise license** for the XCZU6EG (see step 1 of the build) | Block design, `p01_preproc` HLS IP, bitstream, XSA |
| AMD PetaLinux | 2024.1 | Linux image (kernel 6.6, Yocto langdale) |
| Yocto (alternative to PetaLinux) | AMD rel-v2024.1 = Yocto 4.1.4 langdale, `repo` tool, Yocto buildtools 4.1.4 | The same Linux image without the PetaLinux tool |
| Vitis AI runtime | 3.5 (`meta-vitis` rel-v2024.1, in `layers/`) | VART / XIR on the board |
| DPUCZDX8G IP | 4.1 (downloaded by `scripts/fetch_dpu_ip.sh`) | DPU in the PL |
| AMD Vitis Vision | Vitis Libraries `v2024.1_update2` (downloaded by `scripts/fetch_vitis_vision.sh`) | Image functions in `p01_preproc` |
| Build host | Ubuntu 22.04 LTS, internet access, about 100 GB free disk space per Linux build | Downloads: DPU IP, Vitis Vision, Yocto sources and sstate, YOLOv5-nano model |

---

## Design

| Topic | Implementation |
|-------|----------------|
| Camera | USB UVC (V4L2). **MJPEG** (decoded on the CPU with libjpeg-turbo) up to **1280×960 @ 30 fps** by default, the next smaller mode if the camera does not have it (e.g. 1280×720), **YUYV** when the camera has no suitable MJPEG mode. |
| PL preprocessing | AMD Video Frame Buffer Read (YUYV8) → `p01_preproc` 2.1 (Vitis Vision: `yuyv2rgb`, `resize` + `insertBorder` letterbox, `preProcess` INT8) → AMD Video Frame Buffer Write (RGB8), written straight into the DPU input tensor (zero copy) |
| DPU | DPUCZDX8G v4.1, 1× B4096, 275 / 550 MHz, fingerprint `0x101000056010407` (same as the ZCU102 / ZCU104 / KV260 reference, so Model Zoo xmodels run unchanged) |
| Model | YOLOv5-nano, COCO, 640×640 (Vitis AI Model Zoo 3.0 `yolov5_nano_pt`), downloaded by the `p01-app` recipe at build time, in the image under `/usr/share/p01/models` |
| Composition | Mali-400 MP2, OpenGL ES 2.0, EGL fbdev: the camera frame is imported as a dma-buf texture (zero copy, YUYV → RGB in the shader), boxes, labels and statistics on top |
| Display | mini DisplayPort, 1920×1080, full-screen video, statistics in the bottom corners |
| Overlay timing | Default: the video is delayed by the inference latency, so each box is drawn on its own frame. Runtime switch: low-latency mode. |
| Start-up | `p01.service` in the image, **disabled by default** |

```
 USB UVC camera (MJPEG ≤ 1280×960 @ 30 fps, or YUYV)
        │ USB 2.0, uvcvideo (V4L2)
        ▼
 MJPEG: libjpeg-turbo decode (2 threads) ─► YUYV frame N in a CMA buffer (physical address, frame ID N)
                              │                                        │
                              ▼                                        │ same buffer, no copy
  PL fb_rd → p01_preproc (Vitis Vision) → fb_wr, AXI4-Stream Video:  │
     YUYV → RGB, letterbox resize to 640×640,                          │
     normalise → INT8, written into the DPU input tensor                │
                              ▼                                        │
  DPU 1× B4096 (VART): YOLOv5-nano                                     │
                              ▼                                        │
  CPU: decode + NMS, boxes mapped back to the camera resolution        │
                              ▼                                        ▼
  Mali-400 OpenGL ES 2.0: video texture (frame N) + boxes / labels / confidence / stats
                              ▼
  mini DisplayPort (PS DisplayPort controller) → 1920×1080 monitor
```

## Repository

```
VIVADO_2024.1/                  hardware: block design, HLS kernel, build scripts
  board/                        ME-XU1-6EG + ME-ST1 board settings, PS configuration, pin constraints
  hls/                          p01_preproc (Vitis Vision) kernel, reference model, testbench
  export/                       written by the hardware build, not in the repository:
                                p01_xu1_st1.xsa = hardware description with bitstream, the input
                                of both Linux builds
  README.md                     block design, address map, registers, verification
  PREPROC_VITIS_VISION.md       PL preprocessing: sources, versions, build steps, results
layers/                         Yocto layers used by both Linux builds
  meta-p01/                     product layer: p01-app recipe (application, start script, systemd
                                service, model download), device-tree additions, DPU driver, VART
  meta-enclustra-amd/           Enclustra module / baseboard / reference-design layers (ported to 2024.1)
  meta-vitis/                   AMD meta-vitis rel-v2024.1 (Vitis AI 3.5)
PETALINUX_2024.1/
  p01-xu1-st1/                  PetaLinux project (project-spec/ and .petalinux/metadata)
  README.md                     image build, SD card, first boot, network
YOCTO_2024.1/
  meta-p01-yocto/               Yocto-only settings (machine, users, network, image content)
  README.md                     Yocto build step by step
LICENSE                         MIT licence of the project files (see Licence below)
```

---

## Build

The order is fixed: **hardware first, then Linux.** The hardware build writes the XSA (the hardware
description with the bitstream); it is not part of the repository, and both Linux builds start
from it.

### 1. Hardware (always first: it writes the XSA)

```bash
cd VIVADO_2024.1
XILINX_ROOT=/tools/Xilinx scripts/build_all.sh    # folder that contains Vivado/2024.1 and Vitis_HLS/2024.1
```

Downloads the DPU IP and Vitis Vision, builds the `p01_preproc` IP (C simulation, synthesis, C/RTL
co-simulation), creates the Vivado project in `work/`, and writes `export/p01_xu1_st1.xsa` (with
bitstream). It stops with an error if timing is not met or the DPU fingerprint differs. About one
hour. Details: [VIVADO_2024.1/README.md](VIVADO_2024.1/README.md).

> **Vivado license:** the XCZU6EG is not one of the devices of the free Vivado ML Standard Edition
> (its Zynq UltraScale+ list covers ZU1 to ZU5 and ZU7). Synthesis and implementation of this design
> need a Vivado ML Enterprise license (node-locked or floating; AMD also offers a 30-day evaluation
> license). From Vivado 2026.1 AMD uses tiered licenses; check that the tier covers the XCZU6EG.
> Without a license, `synth_design` stops with `[Common 17-345] A valid license was not found for
> feature 'Synthesis' and/or device 'xczu6eg'`; Vivado License Manager → *View License Status*
> shows the installed licenses. PetaLinux and Yocto themselves need no Vivado license, but they
> build from the XSA that this step writes, so the step cannot be skipped.

### 2a. Linux image with PetaLinux

```bash
source <petalinux-2024.1-install>/settings.sh
cd PETALINUX_2024.1/p01-xu1-st1
# import the XSA of step 1 (before the first build, and again after every new hardware build):
petalinux-config --get-hw-description=../../VIVADO_2024.1/export/p01_xu1_st1.xsa --silentconfig
petalinux-build
petalinux-package --boot --u-boot --fpga --force       # BOOT.BIN with the PL bitstream
```

`petalinux-config --get-hw-description` copies the XSA into the project
(`project-spec/hw-description/`, created by this command).

`--fpga` is required: without the bitstream in BOOT.BIN, Linux stops during boot when the drivers
of the PL blocks in the device tree access them. Output in `images/linux/`. Details, SD card layout
and first boot: [PETALINUX_2024.1/README.md](PETALINUX_2024.1/README.md).

### 2b. Linux image with Yocto (alternative)

The same image with the AMD Yocto layers (`repo` manifest `rel-v2024.1`), the layers in `layers/` and
`YOCTO_2024.1/meta-p01-yocto`. All commands: [YOCTO_2024.1/README.md](YOCTO_2024.1/README.md).
Important points:

- **The XSA of step 1 must exist:** the machine configuration reads
  `VIVADO_2024.1/export/p01_xu1_st1.xsa` (`HDF_PATH`).
- **Yocto buildtools are required** on Ubuntu 22.04: the current Ubuntu `tar` (security update of
  June 2026) breaks the Yocto langdale packaging step (`do_package`: `Cannot open: Bad address`).
  The buildtools of Yocto 4.1.4 bring their own `tar`; every Yocto terminal starts with
  `source buildtools/environment-setup-x86_64-pokysdk-linux`.
- **Host-specific setting:** `XILINX_SDK_TOOLCHAIN` in `YOCTO_2024.1/build/conf/local.conf` is the
  path of the Vitis 2024.1 installation (XSCT generates the device tree, FSBL and PMU firmware from
  the XSA); change it to your installation.
- **Shared with the PetaLinux project:** the Yocto build uses the download folder of the PetaLinux
  project (`PETALINUX_2024.1/p01-xu1-st1/build/downloads`, `DL_DIR`) and its build results
  (`PETALINUX_2024.1/p01-xu1-st1/build/sstate-cache`, first `SSTATE_MIRRORS` entry), then the AMD
  sstate server. Building PetaLinux first makes the Yocto build much shorter. Without a PetaLinux
  build, Yocto creates that download folder and builds the missing parts itself.
- Machine `p01-xu1-st1-xczu6eg` (the name must contain `eg`: the Enclustra layer derives the SoC
  variant from it; EG = with Mali-400 GPU). Output in `YOCTO_2024.1/build/tmp/deploy/images/p01-xu1-st1-xczu6eg/`.

### 3. SD card and first boot (summary)

| Partition | Format | PetaLinux (`images/linux/`) | Yocto (`tmp/deploy/images/p01-xu1-st1-xczu6eg/`) |
|-----------|--------|-----------------------------|----------------------------------------------------|
| 1 | FAT32, ≥ 256 MB | `BOOT.BIN`, `boot.scr`, `image.ub` | `boot.bin` (copied as `BOOT.BIN`), `boot.scr`, `Image`, `system.dtb` |
| 2 | ext4 | `rootfs.ext4` written with `dd` | `petalinux-image-minimal-p01-xu1-st1-xczu6eg.ext4` written with `dd` |

Log in on the serial console as `petalinux`; the password is set at the first login (root login is
disabled, use `sudo`). The board's Ethernet has the static address `192.168.2.10/24`.

## Run

```bash
sudo p01-run.sh                        # USB camera, MJPEG up to 1280x960 @ 30 fps, full screen on DP
sudo p01-run.sh --pl-test              # PL preprocessing self-test (no camera or DPU needed)
sudo systemctl enable --now p01        # start at every power-up (disabled by default)
journalctl -u p01 -f                   # camera modes, selected mode, statistics every 5 s
```

Options, the service settings, runtime control and measurements:
[application README](layers/meta-p01/recipes-apps/p01-app/README.md).

---

## Results (hardware release 2.1.0)

| Check | Result |
|-------|--------|
| HLS `p01_preproc` | C simulation 7 cases and C/RTL co-simulation pass, max \|diff\| 1 int8 LSB against the reference model; estimated Fmax 289.6 MHz |
| Vivado implementation | WNS +0.156 ns, WHS +0.006 ns, no critical warnings; LUT 34 %, BRAM 38 %, DSP 38 % |
| PL self-test on the board | YUYV 640×480 1.63 ms, 1280×720 4.12 ms, 1280×960 4.98 ms, 1920×1080 8.33 ms; output equal to the C simulation |
| Camera mode selection | C270: MJPEG 1280×960 (auto); `--max-size 1280x720` → MJPEG 1280×720; `--format yuyv` → YUYV 640×480 |
| Pipeline, C270 MJPEG 1280×960 | 30 fps camera / inference / display, no frames lost by the application; MJPEG decode 20–23 ms, PL preprocessing 5.0 ms, DPU 8.3–8.8 ms; capture → display 94–101 ms; CPU 22 % |
| Pipeline, C270 YUYV 640×480 | 30 fps, PL preprocessing 1.66 ms, capture → display 71 ms, CPU 5 % |
| Yocto image | Device tree identical to the PetaLinux one; same rootfs content (application, model, service, VART, network, users); boots and runs on the board |

---

## Licence

The files written for this project are released under the **MIT licence** ([LICENSE](LICENSE)):
the Vivado scripts and block design, the `p01_preproc` HLS kernel with its testbench and reference
model, the `p01-app` application, the layers `meta-p01` and `meta-p01-yocto`, the PetaLinux project
configuration and the documentation. Everyone may use, copy, change and redistribute them, also in
commercial products, as long as the copyright notice and the licence text are kept.

They are provided **"as is", without warranty of any kind**. ANADOLOGIC is not liable for any claim
or damage that results from their use; whoever uses them does so at their own risk.

The components listed below are not covered by this licence: they keep their own.

## Third-party components

| Component | Where | Licence |
|-----------|-------|---------|
| Enclustra Yocto layers (`meta-enclustra-amd`, ported to 2024.1) | `layers/meta-enclustra-amd/` | MIT (`LICENSE`); upstream and port notes in `port-2024.1/` |
| AMD `meta-vitis` rel-v2024.1 | `layers/meta-vitis/` | MIT (upstream commit in `SOURCE.txt`) |
| PetaLinux project template files (U-Boot `platform-top.h`, `system-user.dtsi`, layer files) | `PETALINUX_2024.1/p01-xu1-st1/project-spec/meta-user/` | MIT (`COPYING.MIT`) |
| AMD Yocto layers (poky, meta-xilinx, meta-xilinx-tools, meta-petalinux, meta-openembedded, ...) | downloaded with `repo` to `YOCTO_2024.1/sources/` (not in the repository) | Licences of the respective layers |
| ME-XU1 + ME-ST1 PS configuration and pin constraints | `VIVADO_2024.1/board/` | Values from the Enclustra Mercury XU1 ST1 reference design, MIT-style (notice in `ps_config.tcl`) |
| AMD DPUCZDX8G IP | downloaded to `VIVADO_2024.1/ip_repo/` (not in the repository) | AMD licence |
| AMD Vitis Vision library | downloaded to `VIVADO_2024.1/ext/` (not in the repository) | Apache-2.0 |
| YOLOv5-nano xmodel | downloaded at build time by the `p01-app` recipe from the AMD Vitis AI Model Zoo 3.0 (not in the repository) | YOLOv5 is by Ultralytics (AGPL-3.0); used here for evaluation and training only |
