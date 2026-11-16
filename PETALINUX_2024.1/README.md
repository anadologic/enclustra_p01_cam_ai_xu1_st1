# PetaLinux 2024.1 Image (ME-XU1-6EG + ME-ST1)

Embedded Linux for the camera AI pipeline: kernel, device tree, Vitis AI runtime, Mali GPU
libraries and the `p01-app` application with its model and systemd service.

| Target | Value |
|--------|-------|
| SoM / carrier | ME-XU1-6EG-1I-D12E-NA-R5.0 on ME-ST1 |
| Tool | PetaLinux 2024.1 (host: Ubuntu 22.04, internet access, about 100 GB free disk space) |
| Machine | `refdes-xu1-st1` (Enclustra layers), override `enclustra-sd` |
| Input | `../VIVADO_2024.1/export/p01_xu1_st1.xsa` (P01 2.1.0): written by the hardware build in [../VIVADO_2024.1](../VIVADO_2024.1/), not in the repository; imported by the first command of the build below |
| Output (`p01-xu1-st1/images/linux/`) | `BOOT.BIN`, `boot.scr`, `image.ub` (kernel + device tree), `rootfs.ext4` / `rootfs.tar.gz` |

## Project contents (`p01-xu1-st1/project-spec/`)

| Part | Content |
|------|---------|
| `configs/config` | Machine, SD boot with ext4 rootfs, static IP `192.168.2.10/24`, user layers (`CONFIG_USER_LAYER_x` → `../../layers/`, below) |
| `configs/rootfs_config`, `meta-user/conf/user-rootfsconfig` | `packagegroup-petalinux-vitisai`, `p01-app`, OpenSSH, users (`petalinux` with sudo) |
| `hw-description/` | Not in the repository: created by `petalinux-config --get-hw-description` (a copy of the XSA and the PS initialisation it contains) |
| `meta-user/` | PetaLinux templates only (U-Boot `platform-top.h`, `system-user.dtsi`) and `conf/petalinuxbsp.conf`: `rm_work`, no SD card image (wic), no JFFS2 image |

User layers, shared with the Yocto build ([../layers](../layers/)):

| Layer | Content |
|-------|---------|
| `layers/meta-enclustra-amd/` (module, baseboard, refdes) | Enclustra layers (upstream `xlnx-2024.2`, ported to 2024.1: `port-2024.1/`) |
| `layers/meta-vitis/` | AMD `meta-vitis` rel-v2024.1: VART, XIR, unilog, target-factory, Vitis AI Library 3.5 |
| `layers/meta-p01/` | Product layer: `p01-app` recipe (application, start script, `p01.service`; the YOLOv5-nano model is downloaded from the AMD Vitis AI Model Zoo at build time; [README](../layers/meta-p01/recipes-apps/p01-app/README.md)), `p01-video.dtsi` (video pipeline nodes off), `p01-bootargs.dtsi` (kernel command line), DPU kernel driver, VART without XRT |

---

## Build

```bash
source <petalinux-2024.1-install>/settings.sh
cd PETALINUX_2024.1/p01-xu1-st1
# import the XSA built in VIVADO_2024.1 (before the first build, and again after every new hardware build):
petalinux-config --get-hw-description=../../VIVADO_2024.1/export/p01_xu1_st1.xsa --silentconfig
petalinux-build
petalinux-package --boot --u-boot --fpga --force       # images/linux/BOOT.BIN with the PL bitstream
```

- **Build the hardware first.** The XSA is not part of the repository:
  `../VIVADO_2024.1/scripts/build_all.sh` writes it ([hardware README](../VIVADO_2024.1/README.md);
  the XCZU6EG needs a Vivado ML Enterprise license). `petalinux-config --get-hw-description` copies
  it to `project-spec/hw-description/` and generates `project-spec/configs/plnx_syshw_data` from it,
  so it has to run before `petalinux-build`.
- **`--fpga` is required.** The device tree describes the PL blocks (DPU, `p01_preproc`, version
  register); without the bitstream in BOOT.BIN their drivers access unconfigured PL and Linux hangs
  during boot.
- The first build downloads the Yocto sources, the AMD sstate cache and the YOLOv5-nano model;
  later builds are much faster.
  `petalinux-build -c p01-app` rebuilds only the application.
- If a build ends with `ERROR: ... do_*_setscene: Fetcher failure: Unable to find file ...` lines
  but the summary says `all succeeded`, a prebuilt object was missing on the AMD sstate mirror and
  that task was built locally instead. The image is complete; the next `petalinux-build` finishes
  without these messages.
- `petalinux-config --get-hw-description` writes `HARDWARE_PATH` (an absolute path of the build
  host) into `.petalinux/metadata`; remove that line before committing.

## SD card

The card needs two partitions; `/dev/sdX` is the SD card on the host (check with `lsblk`, all data
on it is erased):

```bash
sudo parted -s /dev/sdX mklabel msdos mkpart primary fat32 4MiB 260MiB mkpart primary ext4 260MiB 100% set 1 boot on
sudo mkfs.vfat -F 32 -n BOOT /dev/sdX1
sudo dd if=images/linux/rootfs.ext4 of=/dev/sdX2 bs=4M conv=fsync status=progress
sudo e2fsck -f /dev/sdX2 && sudo resize2fs /dev/sdX2        # rootfs uses the whole partition
sudo mount /dev/sdX1 /mnt
sudo cp images/linux/BOOT.BIN images/linux/boot.scr images/linux/image.ub /mnt/
sudo umount /mnt
```

The kernel mounts the rootfs from `/dev/mmcblk1p2` (the SD card; `mmcblk0` is the eMMC of the
module), set in `p01-bootargs.dtsi`. Set the ME-ST1 boot mode to SD card (ME-ST1 user manual; the
boot mode overrides of the module are listed in `meta-enclustra-amd/README.md`).

To replace only the bitstream on a running board, copy a new `BOOT.BIN` to the boot partition
(`/run/media/boot-mmcblk1p1/`) and reboot.

## First boot

- **Console:** ST1 USB-UART, 115200 8N1.
- **Login:** `petalinux`; the password is set at the first login. `root` has no password and cannot
  log in; use `sudo`.
- **Network:** static `192.168.2.10/24` (gateway `192.168.2.1`) on the Ethernet ports
  (`configs/systemd-conf/wired.network`). Connect one ST1 port to the PC, give the PC an address in
  `192.168.2.0/24`, then `ssh petalinux@192.168.2.10`. Change the address with `petalinux-config`
  → Subsystem AUTO Hardware Settings → Ethernet Settings.
- **Demo:** `sudo p01-run.sh` starts the pipeline by hand. `p01.service` is installed but disabled;
  `sudo systemctl enable --now p01` starts it at every power-up. Options and measurements:
  [application README](../layers/meta-p01/recipes-apps/p01-app/README.md).

---

## Configuration notes

- **Device tree.** The video pipeline (Frame Buffer Read → `p01_preproc` → Frame Buffer Write, reset
  GPIO) is programmed from user space by `p01-app` through `/dev/mem`. `p01-video.dtsi` therefore
  disables the generated `fb_rd` / `fb_wr` nodes (`xlnx,axi-frmbuf-*`, Linux `xilinx_frmbuf` DMA
  driver) and `video_rst`. DPU (`xlnx,dpuczdx8g-4.1`), `p01_preproc` and `version_reg` stay enabled.
- **`system-user.dtsi`.** The file that is used comes from `meta-enclustra-baseboard` (it shadows
  the one in `meta-user`), and that layer appends its board includes in `do_configure`.
  `layers/meta-p01/recipes-bsp/device-tree/device-tree.bbappend` adds `p01-video.dtsi` and
  `p01-bootargs.dtsi` the same way, at the end, so the bootargs there replace the Enclustra default.
- **Kernel command line** (`p01-bootargs.dtsi`): `root=/dev/mmcblk1p2 rw rootfstype=ext4 rootwait`,
  `cma=700M` (camera ring buffers, DPU tensors), `clk_ignore_unused`, `consoleblank=0`, `panic=3`.
- **DPU runtime.** Vivado-flow DPU (`/dev/dpu`, kernel driver enabled by `layers/meta-p01`), VART 3.5 without XRT; the fingerprint
  `0x101000056010407` matches the Model Zoo `yolov5_nano_pt` xmodel in the image.
- **GPU.** `libmali-xlnx` r9p0; `p01-run.sh` puts the fbdev backend first in the library path (the
  rootfs default is the X11 backend).
- **SSH.** Root login is disabled; log in as `petalinux`.
