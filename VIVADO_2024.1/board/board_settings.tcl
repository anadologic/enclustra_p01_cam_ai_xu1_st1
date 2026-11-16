# ----------------------------------------------------------------------------------------------------
# ME_XU1_6EG_ST1 - board settings (Vivado 2024.1), sourced by scripts/settings.tcl
#
# Module   : Enclustra Mercury+ ME-XU1-6EG-1I-D12E-NA-R5.0
# Carrier  : Enclustra Mercury+ ST1 (ME-ST1)
#
# Part number: XCZU6EG-1FFVC900I (Mercury+ XU1 user manual, table 1, "6EG-1I" models)
# PS DDR4    : D12E = 5x 8 Gbit (512M x16) DDR4-2400, 64-bit + ECC = 4 GB
#              (Mercury+ XU1 user manual, table 25 / 26; Enclustra preset PS_D12E)
# ----------------------------------------------------------------------------------------------------

set board_name   ME_XU1_6EG_ST1
set module_code  ME-XU1-6EG-1I-D12E-NA-R5.0
set baseboard    ME-ST1
set part         xczu6eg-ffvc900-1-i
set ps_ddr       PS_D12E

set bd_name      ${board_name}_bd
set top_name     ${board_name}_top
set ps_cell_name zynq_ultra_ps_e_0

set vivado_required 2024.1
if {![string match "${vivado_required}*" [version -short]]} {
  error "${board_name}: Vivado ${vivado_required} is required, running [version -short]"
}
