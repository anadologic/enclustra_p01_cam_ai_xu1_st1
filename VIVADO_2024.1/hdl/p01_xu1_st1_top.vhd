-- ---------------------------------------------------------------------------------------------------
-- p01_xu1_st1_top
--
-- Top level of p01_xu1_st1 (ME-XU1-6EG-1I-D12E R5.0 + ME-ST1). Board I/O (board/ME_XU1_6EG_ST1.xdc):
-- DisplayPort AUX / HPD (active-high OE on the ST1) and the I2C FPGA bus.
-- All PL logic (DPU, video pipeline) sits inside the block design.
-- ---------------------------------------------------------------------------------------------------

library ieee;
use ieee.std_logic_1164.all;

entity p01_xu1_st1_top is
  port (
    -- DisplayPort
    DP_HPD       : in    std_logic;
    DP_AUX_IN    : in    std_logic;
    DP_AUX_OE    : out   std_logic;
    DP_AUX_OUT   : out   std_logic;

    -- I2C FPGA
    I2C_SCL_FPGA : inout std_logic;
    I2C_SDA_FPGA : inout std_logic
  );
end entity p01_xu1_st1_top;

architecture rtl of p01_xu1_st1_top is

  signal dp_aux_oe_n : std_logic;

begin

  i_bd : entity work.p01_xu1_st1_bd_wrapper
    port map (
      DP_AUX_OUT      => DP_AUX_OUT,
      DP_AUX_OE_N     => dp_aux_oe_n,
      DP_AUX_IN       => DP_AUX_IN,
      DP_HPD          => DP_HPD,
      IIC_FPGA_scl_io => I2C_SCL_FPGA,
      IIC_FPGA_sda_io => I2C_SDA_FPGA
    );

  DP_AUX_OE <= not dp_aux_oe_n;

end architecture rtl;
