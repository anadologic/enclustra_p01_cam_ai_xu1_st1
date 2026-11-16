# DPUCZDX8G kernel driver (/dev/dpu) for the Vivado-flow DPU in the P01 block design.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " file://p01-dpu.cfg"
KERNEL_FEATURES:append = " p01-dpu.cfg"
