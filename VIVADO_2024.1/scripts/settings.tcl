# ----------------------------------------------------------------------------------------------------
# p01_xu1_st1 - Vivado 2024.1 settings
#
# Board settings (part, PS DDR preset, Vivado version check), the PS configuration and the board
# pin constraints of ME-XU1-6EG + ME-ST1 are in board/.
# Requires: root_dir (this VIVADO_2024.1 folder) set by the calling script.
# ----------------------------------------------------------------------------------------------------

set board_dir [file join $root_dir board]
source [file join $board_dir board_settings.tcl]

set project_name p01_xu1_st1
set bd_name      ${project_name}_bd
set top_name     ${project_name}_top
set ps_cell_name zynq_ultra_ps_e_0

# Release identification, read by Linux from version_reg (AXI GPIO, both channels inputs)
set p01_magic    0x50303120   ;# "P01 "
set p01_version  0x00020100   ;# 2.1.0 (major << 16 | minor << 8 | patch)

# DPUCZDX8G v4.1: 1x B4096, TRD default options -> fingerprint of the Model Zoo
# zcu102 / zcu104 / kv260 xmodels (YOLOv5-nano)
# 275 MHz (550 MHz DSP clock): at 300 / 600 MHz the -1 speed grade closed with only +0.001 ns
# WNS on the DSP clock. YOLOv5-nano still runs far above the 30 fps camera rate.
set dpu_clk_mhz         275
set dpu_fingerprint     0x101000056010407

set ip_repo_dir [file join $root_dir ip_repo]
