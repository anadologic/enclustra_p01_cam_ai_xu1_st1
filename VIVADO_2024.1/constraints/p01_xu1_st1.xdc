# ----------------------------------------------------------------------------------------------------
# p01_xu1_st1 constraints. Board pins, UNUSEDPIN PULLNONE and over-temperature shutdown come from
# board/ME_XU1_6EG_ST1.xdc.
# DPU and HLS timing constraints are scoped inside their IPs.
# ----------------------------------------------------------------------------------------------------

# Smaller bitstream / faster PL configuration (as in the DPU TRD)
set_property BITSTREAM.GENERAL.COMPRESS TRUE [current_design]
