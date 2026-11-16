# ----------------------------------------------------------------------------------------------------
# ME_XU1_6EG_ST1 - Zynq UltraScale+ PS configuration (zynq_ultra_ps_e 3.5, Vivado 2024.1)
#
# Board-level settings (MIO, DDR, GTR, clocks) are taken from the Enclustra reference design
#   github.com/enclustra/Mercury_XU1_ST1_Reference_Design, tag 2024.1_v1.1.3,
#   reference_design/scripts/Mercury_XU1_ST1_bd.tcl
# plus the PS clock fixes Enclustra released in 2024.2_v1.2.3 ("Adjust PS reference clock
# frequency to remove rounding errors"): PSS_REF_CLK 33.333333 MHz, PL0/PL1 from IOPLL.
# Property order follows the Enclustra script, because the PS IP derives defaults in sequence.
#
# Usage in any Vivado 2024.1 block design on part xczu6eg-ffvc900-1-i:
#   source <path>/ps_config.tcl
#   apply_ps_config [get_bd_cells zynq_ultra_ps_e_0] PS_D12E
#
# Original Enclustra copyright notice (MIT-style licence) for the configuration values:
#   Copyright (c) 2024 by Enclustra GmbH, Switzerland. Permission is hereby granted, free of
#   charge, to any person obtaining a copy of this hardware, software, firmware, and associated
#   documentation files (the "Product"), to deal in the Product without restriction, including
#   without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
#   and/or sell copies of the Product, and to permit persons to whom the Product is furnished to
#   do so, subject to the following conditions: The above copyright notice and this permission
#   notice shall be included in all copies or substantial portions of the Product.
#   THE PRODUCT IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
# ----------------------------------------------------------------------------------------------------

proc apply_ps_config {ps ps_ddr} {

  # --- MIO bank voltages -------------------------------------------------------------------------
  set_property -dict [ list \
    CONFIG.PSU_BANK_0_IO_STANDARD {LVCMOS18} \
    CONFIG.PSU_BANK_1_IO_STANDARD {LVCMOS18} \
    CONFIG.PSU_BANK_2_IO_STANDARD {LVCMOS18} \
    CONFIG.PSU_BANK_3_IO_STANDARD {LVCMOS18} \
  ] $ps

  # --- DisplayPort: GTR lane 0/1 (Dual Lower), ref clk 3 = 27 MHz; AUX/HPD via EMIO ---------------
  set_property -dict [ list \
    CONFIG.PSU__DISPLAYPORT__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__DP__REF_CLK_SEL {Ref Clk3} \
    CONFIG.PSU__DP__LANE_SEL {Dual Lower} \
    CONFIG.PSU__CRF_APB__TOPSW_MAIN_CTRL__SRCSEL {DPLL} \
    CONFIG.PSU__CRF_APB__DP_VIDEO_REF_CTRL__SRCSEL {VPLL} \
    CONFIG.PSU__CRF_APB__DP_AUDIO_REF_CTRL__SRCSEL {DPLL} \
    CONFIG.PSU__CRF_APB__DP_STC_REF_CTRL__SRCSEL {DPLL} \
  ] $ps

  # --- Peripherals ---------------------------------------------------------------------------------
  #  QSPI x4 (MIO 0..6, boot)        SD0 eMMC 8-bit (MIO 13..22)   SD1 microSD (MIO 46..51, CD)
  #  I2C0 (MIO 10..11, ST1 I2C bus)  UART0 (MIO 38..39, FTDI)      TTC0
  #  GEM0 (MIO 26..37) + MDIO (MIO 76..77), KSZ9031 PHY addr 3
  #  GEM3 (MIO 64..75), KSZ9031 PHY addr 7 on the shared MDIO bus
  #  USB0 (MIO 52..63), USB3320C ULPI PHY, ref clk 2 = 100 MHz
  #  PL clocks: pl_clk0 = 100 MHz, pl_clk1 = 50 MHz (IOPLL)
  set_property -dict [ list \
    CONFIG.PSU__PSS_REF_CLK__FREQMHZ {33.333333} \
    CONFIG.PSU__QSPI__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__CRL_APB__QSPI_REF_CTRL__FREQMHZ {200} \
    CONFIG.PSU__CRL_APB__QSPI_REF_CTRL__SRCSEL {RPLL} \
    CONFIG.PSU__QSPI__GRP_FBCLK__ENABLE {1} \
    CONFIG.PSU__QSPI__PERIPHERAL__DATA_MODE {x4} \
    CONFIG.PSU__SD0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__SD0__SLOT_TYPE {eMMC} \
    CONFIG.PSU__SD0__PERIPHERAL__IO {MIO 13 .. 22} \
    CONFIG.PSU__SD0__DATA_TRANSFER_MODE {8Bit} \
    CONFIG.PSU__SD1__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__SD1__SLOT_TYPE {SD 2.0} \
    CONFIG.PSU__SD1__PERIPHERAL__IO {MIO 46 .. 51} \
    CONFIG.PSU__SD1__GRP_CD__ENABLE {1} \
    CONFIG.PSU__I2C0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__I2C0__PERIPHERAL__IO {MIO 10 .. 11} \
    CONFIG.PSU__UART0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__UART0__PERIPHERAL__IO {MIO 38 .. 39} \
    CONFIG.PSU__TTC0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__ENET0__GRP_MDIO__ENABLE {1} \
    CONFIG.PSU__ENET0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__ENET3__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__ENET3__GRP_MDIO__ENABLE {0} \
    CONFIG.PSU__USB0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__USB__RESET__MODE {Disable} \
    CONFIG.PSU__USB0__REF_CLK_SEL {Ref Clk2} \
    CONFIG.PSU__USB0__REF_CLK_FREQ {100} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__FPGA_PL1_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {100} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__SRCSEL {IOPLL} \
    CONFIG.PSU__CRL_APB__PL1_REF_CTRL__FREQMHZ {50} \
    CONFIG.PSU__CRL_APB__PL1_REF_CTRL__SRCSEL {IOPLL} \
    CONFIG.PSU__GPIO0_MIO__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__GPIO1_MIO__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__GPIO2_MIO__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU_MIO_7_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_12_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_23_PULLUPDOWN {disable} \
  ] $ps

  # --- PS DDR4 (64-bit + ECC, DDR4-2400T, x16 devices) ---------------------------------------------
  switch -- $ps_ddr {
    PS_D11E {
      # 4 Gbit (256M x16) devices -> 2 GB
      set_property -dict [ list \
        CONFIG.PSU__DDRC__SPEED_BIN {DDR4_2400T} \
        CONFIG.PSU__DDRC__CWL {12} \
        CONFIG.PSU__DDRC__DEVICE_CAPACITY {4096 MBits} \
        CONFIG.PSU__DDRC__DRAM_WIDTH {16 Bits} \
        CONFIG.PSU__DDRC__ROW_ADDR_COUNT {15} \
        CONFIG.PSU__DDRC__BG_ADDR_COUNT {1} \
        CONFIG.PSU__DDRC__ECC {Enabled} \
        CONFIG.PSU__DDRC__PARITY_ENABLE {1} \
        CONFIG.PSU__DDRC__BUS_WIDTH {64 Bit} \
      ] $ps
    }
    PS_D12E {
      # 8 Gbit (512M x16) devices -> 4 GB
      set_property -dict [ list \
        CONFIG.PSU__DDRC__SPEED_BIN {DDR4_2400T} \
        CONFIG.PSU__DDRC__CWL {12} \
        CONFIG.PSU__DDRC__DEVICE_CAPACITY {8192 MBits} \
        CONFIG.PSU__DDRC__DRAM_WIDTH {16 Bits} \
        CONFIG.PSU__DDRC__ROW_ADDR_COUNT {16} \
        CONFIG.PSU__DDRC__BG_ADDR_COUNT {1} \
        CONFIG.PSU__DDRC__ECC {Enabled} \
        CONFIG.PSU__DDRC__PARITY_ENABLE {1} \
        CONFIG.PSU__DDRC__BUS_WIDTH {64 Bit} \
      ] $ps
    }
    default {
      error "apply_ps_config: unknown PS DDR preset '$ps_ddr' (expected PS_D11E or PS_D12E)"
    }
  }

  # --- eMMC pull-ups off, PL-to-PS interrupt group 0 ----------------------------------------------
  set_property -dict [ list \
    CONFIG.PSU_MIO_13_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_14_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_15_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_16_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_17_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_18_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_19_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_20_PULLUPDOWN {disable} \
    CONFIG.PSU_MIO_21_PULLUPDOWN {disable} \
    CONFIG.PSU__SD0__PERIPHERAL__IO {MIO 13 .. 22} \
    CONFIG.PSU__SD0__DATA_TRANSFER_MODE {8Bit} \
    CONFIG.PSU__USE__IRQ0 {1} \
  ] $ps

  # --- I2C1 on EMIO: ST1 "I2C FPGA" bus (pins in constraints) -------------------------------------
  set_property -dict [ list \
    CONFIG.PSU__I2C1__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__I2C1__PERIPHERAL__IO {EMIO} \
  ] $ps

  # --- USB 3.0 on GTR lane 2 ------------------------------------------------------------------------
  set_property -dict [ list \
    CONFIG.PSU__USB3_0__PERIPHERAL__ENABLE {1} \
    CONFIG.PSU__USB3_0__PERIPHERAL__IO {GT Lane2} \
  ] $ps

  # --- PL interfaces (project-side, not board-specific) ---------------------------------------------
  # M_AXI_HPM0_FPD + M_AXI_HPM0_LPD masters for PL peripherals. Enable HP/HPC slave ports
  # in the project as needed.
  set_property -dict [ list \
    CONFIG.PSU__USE__M_AXI_GP0 {1} \
    CONFIG.PSU__USE__M_AXI_GP2 {1} \
  ] $ps
}
