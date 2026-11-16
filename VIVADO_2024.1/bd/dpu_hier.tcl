# ----------------------------------------------------------------------------------------------------
# hier_dpu - DPUCZDX8G v4.1, 1x B4096, with its clocking and interrupt concat.
#
# Derived from the Vitis AI 3.0 DPU TRD (Vivado flow): trd_prj.tcl run in Vivado 2024.1 with
# DPU_NUM=1, DPU_CLK_MHz=300 and all other options at the TRD defaults, then exported with
# write_bd_tcl. The DPU options are kept as in the TRD so that the generated arch.json has the
# fingerprint of the Model Zoo zcu102 / zcu104 / kv260 xmodels (0x101000056010407).
#
# Pins
#   S_AXI          DPU registers (32-bit AXI4), on S_AXI_CLK (100 MHz register clock)
#   M_AXI_DATA0/1  DPU feature-map / weight ports (128-bit)  -> S_AXI_HP0_FPD / S_AXI_HP1_FPD
#   M_AXI_SFM      softmax port (128-bit)                    -> S_AXI_HP2_FPD
#   M_AXI_INSTR    instruction port (32-bit)                 -> S_AXI_LPD
#   CLK_IN/RSTn_IN clocking wizard input (100 MHz) and active-low reset
#   DPU_CLK        dpu_clk_mhz output; clocks the DPU masters and the PS HP / LPD ports
#   DPU_RSTn       peripheral reset in the DPU_CLK domain (for other logic on DPU_CLK)
#   DPU_RSTn_INTC  interconnect reset in the DPU_CLK domain
#   INTR[1:0]      [0] = DPU core 0, [1] = softmax (TRD order: pl_ps_irq0[0], pl_ps_irq0[1])
# ----------------------------------------------------------------------------------------------------

proc create_hier_dpu { parent name dpu_clk_mhz } {
  set old_inst [current_bd_instance .]
  current_bd_instance $parent
  set hier [create_bd_cell -type hier $name]
  current_bd_instance $hier

  create_bd_intf_pin -mode Slave  -vlnv xilinx.com:interface:aximm_rtl:1.0 S_AXI
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_DATA0
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_DATA1
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_SFM
  create_bd_intf_pin -mode Master -vlnv xilinx.com:interface:aximm_rtl:1.0 M_AXI_INSTR
  create_bd_pin -dir I -type clk S_AXI_CLK
  create_bd_pin -dir I -type rst S_AXI_RSTn
  create_bd_pin -dir I -type clk CLK_IN
  create_bd_pin -dir I -type rst RSTn_IN
  create_bd_pin -dir O -type clk DPU_CLK
  create_bd_pin -dir O -from 0 -to 0 -type rst DPU_RSTn
  create_bd_pin -dir O -from 0 -to 0 -type rst DPU_RSTn_INTC
  create_bd_pin -dir O -from 1 -to 0 -type intr INTR

  # Clocking: clk_dsp = 2x (with clock enable for DPU clock gating), clk_dpu = 1x
  set clk_wiz [create_bd_cell -type ip -vlnv xilinx.com:ip:clk_wiz dpu_clk_wiz]
  set_property -dict [list \
    CONFIG.CLKOUT1_DRIVES {Buffer_with_CE} \
    CONFIG.CLKOUT1_MATCHED_ROUTING {true} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ [expr {2 * $dpu_clk_mhz}] \
    CONFIG.CLKOUT2_MATCHED_ROUTING {true} \
    CONFIG.CLKOUT2_REQUESTED_OUT_FREQ $dpu_clk_mhz \
    CONFIG.CLKOUT2_USED {true} \
    CONFIG.CLK_OUT1_PORT {clk_dsp} \
    CONFIG.CLK_OUT2_PORT {clk_dpu} \
    CONFIG.PRIMITIVE {Auto} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
    CONFIG.USE_LOCKED {true} \
    CONFIG.USE_RESET {true} \
  ] $clk_wiz
  set rst_dpu [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_gen_clk]
  set rst_dsp [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_gen_clk_dsp]

  # DPU (TRD defaults, 1 core)
  set dpu [create_bd_cell -type ip -vlnv xilinx.com:ip:dpuczdx8g:4.1 DPUCZDX8G]
  set_property -dict [list \
    CONFIG.VER_DPU_NUM {1} \
    CONFIG.ARCH {4096} \
    CONFIG.ARCH_IMG_BKGRP {2} \
    CONFIG.LOAD_AUGM {1} \
    CONFIG.CONV_RELU_ADDON {3} \
    CONFIG.ALU_PARALLEL_USER {4} \
    CONFIG.ALU_LEAKYRELU {0} \
    CONFIG.CONV_WR_PARALLEL {1} \
    CONFIG.SFM_ENA {1} \
    CONFIG.SAVE_ARGMAX_ENA {1} \
    CONFIG.S_AXI_CLK_INDEPENDENT {1} \
    CONFIG.CLK_GATING_ENA {1} \
    CONFIG.CONV_DSP_CASC_MAX {4} \
    CONFIG.CONV_DSP_ACCU_ENA {1} \
    CONFIG.URAM_N_USER {0} \
    CONFIG.TIMESTAMP_ENA {1} \
  ] $dpu

  set irq [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat dpu_concat_irq_inner]
  set_property CONFIG.NUM_PORTS {2} $irq

  # AXI
  connect_bd_intf_net [get_bd_intf_pins S_AXI]       [get_bd_intf_pins DPUCZDX8G/S_AXI]
  connect_bd_intf_net [get_bd_intf_pins M_AXI_DATA0] [get_bd_intf_pins DPUCZDX8G/DPU0_M_AXI_DATA0]
  connect_bd_intf_net [get_bd_intf_pins M_AXI_DATA1] [get_bd_intf_pins DPUCZDX8G/DPU0_M_AXI_DATA1]
  connect_bd_intf_net [get_bd_intf_pins M_AXI_SFM]   [get_bd_intf_pins DPUCZDX8G/SFM_M_AXI]
  connect_bd_intf_net [get_bd_intf_pins M_AXI_INSTR] [get_bd_intf_pins DPUCZDX8G/DPU0_M_AXI_INSTR]

  # Clocks and resets (as in the TRD hier_dpu_clk)
  connect_bd_net [get_bd_pins CLK_IN]  [get_bd_pins dpu_clk_wiz/clk_in1]
  connect_bd_net [get_bd_pins RSTn_IN] [get_bd_pins dpu_clk_wiz/resetn] \
                 [get_bd_pins rst_gen_clk/ext_reset_in] [get_bd_pins rst_gen_clk_dsp/ext_reset_in]
  connect_bd_net [get_bd_pins DPUCZDX8G/dpu_2x_clk_ce] [get_bd_pins dpu_clk_wiz/clk_dsp_ce]
  connect_bd_net [get_bd_pins dpu_clk_wiz/clk_dpu] [get_bd_pins DPU_CLK] \
                 [get_bd_pins rst_gen_clk/slowest_sync_clk] [get_bd_pins DPUCZDX8G/m_axi_dpu_aclk]
  connect_bd_net [get_bd_pins dpu_clk_wiz/clk_dsp] \
                 [get_bd_pins rst_gen_clk_dsp/slowest_sync_clk] [get_bd_pins DPUCZDX8G/dpu_2x_clk]
  connect_bd_net [get_bd_pins dpu_clk_wiz/locked] \
                 [get_bd_pins rst_gen_clk/dcm_locked] [get_bd_pins rst_gen_clk_dsp/dcm_locked]
  connect_bd_net [get_bd_pins rst_gen_clk_dsp/peripheral_aresetn] [get_bd_pins DPUCZDX8G/dpu_2x_resetn]
  connect_bd_net [get_bd_pins rst_gen_clk/peripheral_aresetn] [get_bd_pins DPU_RSTn] \
                 [get_bd_pins DPUCZDX8G/m_axi_dpu_aresetn]
  connect_bd_net [get_bd_pins rst_gen_clk/interconnect_aresetn] [get_bd_pins DPU_RSTn_INTC]
  connect_bd_net [get_bd_pins S_AXI_CLK]  [get_bd_pins DPUCZDX8G/s_axi_aclk]
  connect_bd_net [get_bd_pins S_AXI_RSTn] [get_bd_pins DPUCZDX8G/s_axi_aresetn]

  # Interrupts
  connect_bd_net [get_bd_pins DPUCZDX8G/dpu0_interrupt] [get_bd_pins dpu_concat_irq_inner/In0]
  connect_bd_net [get_bd_pins DPUCZDX8G/sfm_interrupt]  [get_bd_pins dpu_concat_irq_inner/In1]
  connect_bd_net [get_bd_pins dpu_concat_irq_inner/dout] [get_bd_pins INTR]

  current_bd_instance $old_inst
  return $hier
}
