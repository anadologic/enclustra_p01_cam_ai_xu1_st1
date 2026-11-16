# Shared Yocto Layers

Layers used by both Linux builds: the PetaLinux project ([../PETALINUX_2024.1](../PETALINUX_2024.1/),
as `CONFIG_USER_LAYER_x` in `p01-xu1-st1/project-spec/configs/config`) and the Yocto build
([../YOCTO_2024.1](../YOCTO_2024.1/), with `bitbake-layers add-layer`). All are for AMD rel-v2024.1 /
Yocto langdale.

| Layer | Collection | Content |
|-------|------------|---------|
| [meta-p01](meta-p01/) | `p01` | Product layer: `p01-app` recipe (application, start script, `p01.service`, model download; [README](meta-p01/recipes-apps/p01-app/README.md)), device-tree additions (`p01-video.dtsi`: video pipeline nodes off; `p01-bootargs.dtsi`: kernel command line), DPU kernel driver (`CONFIG_XILINX_DPU=y`), VART built without XRT |
| [meta-enclustra-amd](meta-enclustra-amd/) | `enclustra-module`, `enclustra-baseboard`, `enclustra-refdes` | Enclustra layers (upstream <https://github.com/enclustra/meta-enclustra-amd> `xlnx-2024.2`), ported to 2024.1: changes in `port-2024.1/` |
| [meta-vitis](meta-vitis/) | `vitis-ai` | AMD `meta-vitis` rel-v2024.1 (upstream commit in `SOURCE.txt`): VART, XIR, unilog, target-factory, Vitis AI Library 3.5. Needed for PetaLinux 2024.1, which does not include it; the Yocto build uses the same commit from the AMD `repo` manifest. |

The `p01-app` recipe takes `p01_preproc_ref.hpp` from `../VIVADO_2024.1/hls` (the reference model is
shared with the HLS testbench), so `meta-p01` is used inside this repository.

What each build tool generates itself stays with the build: PetaLinux `project-spec/meta-user`
(PetaLinux templates) and `configs/`; Yocto `YOCTO_2024.1/meta-p01-yocto` (machine, users, network,
image content).
