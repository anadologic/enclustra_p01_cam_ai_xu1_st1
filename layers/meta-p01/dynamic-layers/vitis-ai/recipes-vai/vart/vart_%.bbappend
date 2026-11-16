# DPU is integrated with the Vivado flow (IP in block design, no xclbin).
# Vitis AI >= 2.0 requires VART to be built without XRT in this case.
PACKAGECONFIG:remove = "vitis"
