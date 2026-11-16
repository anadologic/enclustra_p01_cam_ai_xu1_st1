# ----------------------------------------------------------------------------------------------------
# ME_XU1_6EG_ST1 - board constraints (ME-XU1-6EG on ME-ST1)
# Pin locations and I/O standards: Enclustra Mercury_XU1_ST1_Reference_Design 2024.1_v1.1.3,
# reference_design/src/Mercury_XU1_ST1.tcl.
# ----------------------------------------------------------------------------------------------------

# PL over-temperature shutdown (as in the Enclustra reference design)
set_property BITSTREAM.CONFIG.OVERTEMPSHUTDOWN ENABLE [current_design]

# Important! Do not remove: keeps every unused PL pin high impedance (Enclustra requirement).
set_property BITSTREAM.CONFIG.UNUSEDPIN PULLNONE [current_design]

# DisplayPort AUX / HPD (PS EMIO)
set_property -dict {PACKAGE_PIN AG9   IOSTANDARD LVCMOS18} [get_ports {DP_HPD}]
set_property -dict {PACKAGE_PIN AG4   IOSTANDARD LVCMOS18} [get_ports {DP_AUX_IN}]
set_property -dict {PACKAGE_PIN AH11  IOSTANDARD LVCMOS18} [get_ports {DP_AUX_OE}]
set_property -dict {PACKAGE_PIN AJ1   IOSTANDARD LVCMOS18} [get_ports {DP_AUX_OUT}]

# I2C FPGA bus (PS I2C1 EMIO)
set_property -dict {PACKAGE_PIN C14   IOSTANDARD LVCMOS18} [get_ports {I2C_SCL_FPGA}]
set_property -dict {PACKAGE_PIN C13   IOSTANDARD LVCMOS18} [get_ports {I2C_SDA_FPGA}]
