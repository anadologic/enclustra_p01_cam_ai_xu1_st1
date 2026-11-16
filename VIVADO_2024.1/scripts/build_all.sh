#!/usr/bin/env bash
# ----------------------------------------------------------------------------------------------------
# Full hardware build: DPU IP + Vitis Vision fetch -> HLS IP -> Vivado project ->
# bitstream + XSA.
#
#   cd VIVADO_2024.1
#   scripts/build_all.sh            (HLS with C/RTL co-simulation)
#   HLS_ARGS=nocosim scripts/build_all.sh
#
# XILINX_ROOT: folder that contains Vivado/2024.1 and Vitis_HLS/2024.1 (default: /tools/Xilinx,
# the AMD installer default). Example: XILINX_ROOT=/opt/Xilinx scripts/build_all.sh
# ----------------------------------------------------------------------------------------------------
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
xroot="${XILINX_ROOT:-/tools/Xilinx}"
cd "$root"

# shellcheck disable=SC1091
source "$xroot/Vivado/2024.1/settings64.sh"
# shellcheck disable=SC1091
source "$xroot/Vitis_HLS/2024.1/settings64.sh"

scripts/fetch_dpu_ip.sh
scripts/fetch_vitis_vision.sh
vitis_hls -f hls/run_hls.tcl ${HLS_ARGS:+-tclargs $HLS_ARGS}
vivado -mode batch -nojournal -log work/create_project.log -source scripts/create_project.tcl
vivado -mode batch -nojournal -log work/build.log -source scripts/build.tcl
