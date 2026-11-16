# ----------------------------------------------------------------------------------------------------
# p01_xu1_st1_bd - block design
#
#   zynq_ultra_ps_e_0   PS (board/ps_config.tcl) + PL interfaces
#   hier_dpu            DPUCZDX8G v4.1, 1x B4096, dpu_clk_mhz / 2x (dpu_hier.tcl)
#   fb_rd               AMD Video Frame Buffer Read 2.5: camera frame (YUYV8) -> AXI4-Stream Video
#   p01_preproc         HLS 2.1 (Vitis Vision): AXI4-Stream Video -> letterbox -> INT8 (RGB) stream
#   fb_wr               AMD Video Frame Buffer Write 2.5: AXI4-Stream Video -> RGB8 = DPU input tensor
#   rst_video           reset of fb_rd / p01_preproc / fb_wr: DPU_RSTn and video_rst GPIO (active low)
#   version_reg         AXI GPIO, ch1 = magic, ch2 = version (read-only)
#   video_rst           AXI GPIO, 1 output, default 1 (write 0 then 1 to reset the video pipeline)
#   DP AUX / HPD, IIC_FPGA  base ST1 EMIO ports
#
# Address map (PS view)
#   0x8F00_0000  16 MB  hier_dpu/DPUCZDX8G S_AXI   (M_AXI_HPM0_LPD, as in the DPU TRD)
#   0xA000_0000  64 KB  p01_preproc s_axi_control (M_AXI_HPM0_FPD)
#   0xA001_0000  64 KB  version_reg               (M_AXI_HPM0_FPD)
#   0xA002_0000  64 KB  fb_rd s_axi_CTRL          (M_AXI_HPM0_FPD)
#   0xA003_0000  64 KB  fb_wr s_axi_CTRL          (M_AXI_HPM0_FPD)
#   0xA004_0000  64 KB  video_rst                 (M_AXI_HPM0_FPD)
#
# PS slave ports                         Interrupts (pl_ps_irq0)
#   S_AXI_HP0_FPD  DPU data 0              [0] DPU core 0      (GIC SPI 89)
#   S_AXI_HP1_FPD  DPU data 1              [1] DPU softmax     (GIC SPI 90)
#   S_AXI_HP2_FPD  DPU softmax             [2] fb_wr           (GIC SPI 91)
#   S_AXI_HP3_FPD  fb_rd + fb_wr (axi_smc_video)  [3] fb_rd   (GIC SPI 92)
#   S_AXI_LPD      DPU instructions        [4] p01_preproc     (GIC SPI 93)
#
# Clocks: pl_clk0 100 MHz -> DPU registers, HPM0_LPD, DPU clocking wizard input
#         DPU_CLK (dpu_clk_mhz, 275 MHz) -> DPU masters, video pipeline, HPM0_FPD control fabric,
#                                           HP / LPD ports
# Requires: settings.tcl, board/ps_config.tcl and dpu_hier.tcl sourced.
# ----------------------------------------------------------------------------------------------------

create_bd_design $bd_name

# --- PS ------------------------------------------------------------------------------------------
set ps [create_bd_cell -type ip -vlnv xilinx.com:ip:zynq_ultra_ps_e:3.5 $ps_cell_name]
apply_ps_config $ps $ps_ddr
set_property -dict [list \
  CONFIG.PSU__MAXIGP2__DATA_WIDTH {32} \
  CONFIG.PSU__USE__S_AXI_GP2 {1} CONFIG.PSU__SAXIGP2__DATA_WIDTH {128} \
  CONFIG.PSU__USE__S_AXI_GP3 {1} CONFIG.PSU__SAXIGP3__DATA_WIDTH {128} \
  CONFIG.PSU__USE__S_AXI_GP4 {1} CONFIG.PSU__SAXIGP4__DATA_WIDTH {128} \
  CONFIG.PSU__USE__S_AXI_GP5 {1} CONFIG.PSU__SAXIGP5__DATA_WIDTH {128} \
  CONFIG.PSU__USE__S_AXI_GP6 {1} CONFIG.PSU__SAXIGP6__DATA_WIDTH {32} \
] $ps

# --- 100 MHz register-clock reset ----------------------------------------------------------------
set rst_reg [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_gen_reg]
connect_bd_net [get_bd_pins $ps_cell_name/pl_resetn0] [get_bd_pins rst_gen_reg/ext_reset_in]

# --- DPU -----------------------------------------------------------------------------------------
create_hier_dpu [current_bd_instance .] hier_dpu $dpu_clk_mhz
connect_bd_intf_net [get_bd_intf_pins $ps_cell_name/M_AXI_HPM0_LPD] [get_bd_intf_pins hier_dpu/S_AXI]
connect_bd_intf_net [get_bd_intf_pins hier_dpu/M_AXI_DATA0] [get_bd_intf_pins $ps_cell_name/S_AXI_HP0_FPD]
connect_bd_intf_net [get_bd_intf_pins hier_dpu/M_AXI_DATA1] [get_bd_intf_pins $ps_cell_name/S_AXI_HP1_FPD]
connect_bd_intf_net [get_bd_intf_pins hier_dpu/M_AXI_SFM]   [get_bd_intf_pins $ps_cell_name/S_AXI_HP2_FPD]
connect_bd_intf_net [get_bd_intf_pins hier_dpu/M_AXI_INSTR] [get_bd_intf_pins $ps_cell_name/S_AXI_LPD]

# --- Video pipeline: Frame Buffer Read -> p01_preproc -> Frame Buffer Write ------------------------
# 1 pixel / clock, 8 bit: AXI4-Stream TDATA 24 bit, AXI-MM 64 bit (stride multiple of 8 bytes)
set fb_rd [create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_rd:2.5 fb_rd]
set_property -dict [list \
  CONFIG.SAMPLES_PER_CLOCK {1} CONFIG.MAX_DATA_WIDTH {8} CONFIG.AXIMM_ADDR_WIDTH {64} \
  CONFIG.MAX_COLS {2048} CONFIG.MAX_ROWS {2048} \
  CONFIG.HAS_RGB8 {0} CONFIG.HAS_YUYV8 {1} \
] $fb_rd
set pre [create_bd_cell -type ip -vlnv anadologic.com:p01:p01_preproc:2.1 p01_preproc]
set fb_wr [create_bd_cell -type ip -vlnv xilinx.com:ip:v_frmbuf_wr:2.5 fb_wr]
set_property -dict [list \
  CONFIG.SAMPLES_PER_CLOCK {1} CONFIG.MAX_DATA_WIDTH {8} CONFIG.AXIMM_ADDR_WIDTH {64} \
  CONFIG.MAX_COLS {1024} CONFIG.MAX_ROWS {1024} \
  CONFIG.HAS_RGB8 {1} \
] $fb_wr
connect_bd_intf_net [get_bd_intf_pins fb_rd/m_axis_video]       [get_bd_intf_pins p01_preproc/s_axis_video]
connect_bd_intf_net [get_bd_intf_pins p01_preproc/m_axis_video] [get_bd_intf_pins fb_wr/s_axis_video]

set smc_video [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axi_smc_video]
set_property -dict [list CONFIG.NUM_SI {2} CONFIG.NUM_MI {1} CONFIG.NUM_CLKS {1}] $smc_video
connect_bd_intf_net [get_bd_intf_pins fb_rd/m_axi_mm_video] [get_bd_intf_pins axi_smc_video/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fb_wr/m_axi_mm_video] [get_bd_intf_pins axi_smc_video/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_smc_video/M00_AXI] [get_bd_intf_pins $ps_cell_name/S_AXI_HP3_FPD]

# Video pipeline reset: DPU_RSTn and the video_rst GPIO (aux reset, active low)
set vrst [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio video_rst]
set_property -dict [list \
  CONFIG.C_GPIO_WIDTH {1} CONFIG.C_ALL_OUTPUTS {1} CONFIG.C_DOUT_DEFAULT {0x00000001} \
] $vrst
set rst_video [create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset rst_video]
set_property CONFIG.C_AUX_RESET_HIGH {0} $rst_video   ;# ext_reset_in polarity follows DPU_RSTn (active low)
connect_bd_net [get_bd_pins video_rst/gpio_io_o] [get_bd_pins rst_video/aux_reset_in]

# --- Release / version register ------------------------------------------------------------------
set ver [create_bd_cell -type ip -vlnv xilinx.com:ip:axi_gpio version_reg]
set_property -dict [list \
  CONFIG.C_IS_DUAL {1} \
  CONFIG.C_ALL_INPUTS {1}   CONFIG.C_GPIO_WIDTH {32} \
  CONFIG.C_ALL_INPUTS_2 {1} CONFIG.C_GPIO2_WIDTH {32} \
] $ver
set c_magic [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant version_magic]
set_property -dict [list CONFIG.CONST_WIDTH {32} CONFIG.CONST_VAL $p01_magic] $c_magic
set c_ver   [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconstant version_value]
set_property -dict [list CONFIG.CONST_WIDTH {32} CONFIG.CONST_VAL $p01_version] $c_ver
connect_bd_net [get_bd_pins version_magic/dout] [get_bd_pins version_reg/gpio_io_i]
connect_bd_net [get_bd_pins version_value/dout] [get_bd_pins version_reg/gpio2_io_i]

# --- Control fabric on HPM0_FPD (DPU_CLK) ---------------------------------------------------------
set smc [create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect axi_smc_ctrl]
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {5} CONFIG.NUM_CLKS {1}] $smc
connect_bd_intf_net [get_bd_intf_pins $ps_cell_name/M_AXI_HPM0_FPD] [get_bd_intf_pins axi_smc_ctrl/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_smc_ctrl/M00_AXI] [get_bd_intf_pins p01_preproc/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins axi_smc_ctrl/M01_AXI] [get_bd_intf_pins version_reg/S_AXI]
connect_bd_intf_net [get_bd_intf_pins axi_smc_ctrl/M02_AXI] [get_bd_intf_pins fb_rd/s_axi_CTRL]
connect_bd_intf_net [get_bd_intf_pins axi_smc_ctrl/M03_AXI] [get_bd_intf_pins fb_wr/s_axi_CTRL]
connect_bd_intf_net [get_bd_intf_pins axi_smc_ctrl/M04_AXI] [get_bd_intf_pins video_rst/S_AXI]

# --- Clocks and resets ----------------------------------------------------------------------------
connect_bd_net [get_bd_pins $ps_cell_name/pl_clk0] \
               [get_bd_pins $ps_cell_name/maxihpm0_lpd_aclk] \
               [get_bd_pins rst_gen_reg/slowest_sync_clk] \
               [get_bd_pins hier_dpu/S_AXI_CLK] [get_bd_pins hier_dpu/CLK_IN]
connect_bd_net [get_bd_pins rst_gen_reg/peripheral_aresetn] \
               [get_bd_pins hier_dpu/S_AXI_RSTn] [get_bd_pins hier_dpu/RSTn_IN]
connect_bd_net [get_bd_pins hier_dpu/DPU_CLK] \
               [get_bd_pins $ps_cell_name/saxihp0_fpd_aclk] [get_bd_pins $ps_cell_name/saxihp1_fpd_aclk] \
               [get_bd_pins $ps_cell_name/saxihp2_fpd_aclk] [get_bd_pins $ps_cell_name/saxihp3_fpd_aclk] \
               [get_bd_pins $ps_cell_name/saxi_lpd_aclk]    [get_bd_pins $ps_cell_name/maxihpm0_fpd_aclk] \
               [get_bd_pins axi_smc_ctrl/aclk] [get_bd_pins version_reg/s_axi_aclk] \
               [get_bd_pins fb_rd/ap_clk] [get_bd_pins p01_preproc/ap_clk] [get_bd_pins fb_wr/ap_clk] \
               [get_bd_pins axi_smc_video/aclk] [get_bd_pins video_rst/s_axi_aclk] \
               [get_bd_pins rst_video/slowest_sync_clk]
connect_bd_net [get_bd_pins hier_dpu/DPU_RSTn] \
               [get_bd_pins version_reg/s_axi_aresetn] [get_bd_pins video_rst/s_axi_aresetn] \
               [get_bd_pins rst_video/ext_reset_in]
connect_bd_net [get_bd_pins rst_video/peripheral_aresetn] \
               [get_bd_pins fb_rd/ap_rst_n] [get_bd_pins p01_preproc/ap_rst_n] [get_bd_pins fb_wr/ap_rst_n]
connect_bd_net [get_bd_pins hier_dpu/DPU_RSTn_INTC] [get_bd_pins axi_smc_ctrl/aresetn] \
               [get_bd_pins axi_smc_video/aresetn]

# --- Interrupts: [1:0] DPU (TRD order), [2] fb_wr, [3] fb_rd, [4] p01_preproc ---------------------
set irq [create_bd_cell -type ip -vlnv xilinx.com:ip:xlconcat irq_concat]
set_property -dict [list CONFIG.NUM_PORTS {4} CONFIG.IN0_WIDTH {2} CONFIG.IN1_WIDTH {1} \
                         CONFIG.IN2_WIDTH {1} CONFIG.IN3_WIDTH {1}] $irq
connect_bd_net [get_bd_pins hier_dpu/INTR]          [get_bd_pins irq_concat/In0]
connect_bd_net [get_bd_pins fb_wr/interrupt]        [get_bd_pins irq_concat/In1]
connect_bd_net [get_bd_pins fb_rd/interrupt]        [get_bd_pins irq_concat/In2]
connect_bd_net [get_bd_pins p01_preproc/interrupt]  [get_bd_pins irq_concat/In3]
connect_bd_net [get_bd_pins irq_concat/dout]        [get_bd_pins $ps_cell_name/pl_ps_irq0]

# --- ST1 EMIO ports (as in the base design) --------------------------------------------------------
create_bd_port -dir O DP_AUX_OUT
create_bd_port -dir O DP_AUX_OE_N
create_bd_port -dir I DP_AUX_IN
create_bd_port -dir I DP_HPD
connect_bd_net [get_bd_ports DP_AUX_OUT]  [get_bd_pins $ps_cell_name/dp_aux_data_out]
connect_bd_net [get_bd_ports DP_AUX_OE_N] [get_bd_pins $ps_cell_name/dp_aux_data_oe_n]
connect_bd_net [get_bd_ports DP_AUX_IN]   [get_bd_pins $ps_cell_name/dp_aux_data_in]
connect_bd_net [get_bd_ports DP_HPD]      [get_bd_pins $ps_cell_name/dp_hot_plug_detect]
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:iic_rtl:1.0 IIC_FPGA
connect_bd_intf_net [get_bd_intf_ports IIC_FPGA] [get_bd_intf_pins $ps_cell_name/IIC_1]

# --- Addresses ------------------------------------------------------------------------------------
set ps_data [get_bd_addr_spaces $ps_cell_name/Data]
assign_bd_address -offset 0x8F000000 -range 16M -target_address_space $ps_data \
    [get_bd_addr_segs hier_dpu/DPUCZDX8G/S_AXI/reg0]
assign_bd_address -offset 0xA0000000 -range 64K -target_address_space $ps_data \
    [get_bd_addr_segs p01_preproc/s_axi_control/Reg]
assign_bd_address -offset 0xA0010000 -range 64K -target_address_space $ps_data \
    [get_bd_addr_segs version_reg/S_AXI/Reg]
assign_bd_address -offset 0xA0020000 -range 64K -target_address_space $ps_data \
    [get_bd_addr_segs fb_rd/s_axi_CTRL/Reg]
assign_bd_address -offset 0xA0030000 -range 64K -target_address_space $ps_data \
    [get_bd_addr_segs fb_wr/s_axi_CTRL/Reg]
assign_bd_address -offset 0xA0040000 -range 64K -target_address_space $ps_data \
    [get_bd_addr_segs video_rst/S_AXI/Reg]
assign_bd_address   ;# DPU and Frame Buffer masters -> DDR (low + high), OCM

regenerate_bd_layout
validate_bd_design
save_bd_design
