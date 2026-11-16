SUMMARY = "USB camera -> PL preprocessing -> DPU YOLOv5-nano -> Mali GPU overlay on DisplayPort"
DESCRIPTION = "p01-app with its start script, the YOLOv5-nano xmodel and the p01 systemd service \
(installed, disabled by default). Needs the P01 2.1 bitstream (version_reg 2.1.0)."
# The p01-app sources of this recipe: MIT (LICENSE in the repository root). The YOLOv5-nano xmodel that
# the recipe downloads and installs is third-party and keeps its own licence (see MODEL_URI below).
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# p01_preproc_ref.hpp is shared with the HLS testbench and stays in VIVADO_2024.1/hls.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${THISDIR}/../../../../VIVADO_2024.1/hls:"

SRC_URI = " \
    file://src \
    file://Makefile \
    file://p01-run.sh \
    file://p01.service \
    file://p01_preproc_ref.hpp \
    ${MODEL_URI} \
"

# YOLOv5-nano for the ZCU102 / ZCU104 / KV260 DPU (fingerprint 0x101000056010407): AMD Vitis AI
# Model Zoo 3.0. Downloaded at build time, not kept in the repository (YOLOv5: Ultralytics, AGPL-3.0).
MODEL_TARBALL = "yolov5_nano_pt-zcu102_zcu104_kv260-r3.0.0.tar.gz"
MODEL_URI = "https://www.xilinx.com/bin/public/openDownload?filename=${MODEL_TARBALL};downloadfilename=${MODEL_TARBALL};name=model;subdir=model"
SRC_URI[model.sha256sum] = "46919842d14718c5f2f8f7ffef2ee540de0ee3e5595591ec96652bce75def622"

S = "${WORKDIR}"

DEPENDS = "vart xir unilog glog virtual/egl virtual/libgles2 jpeg"
RDEPENDS:${PN} += "libmali-xlnx"

inherit systemd

SYSTEMD_SERVICE:${PN} = "p01.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

do_compile() {
    oe_runmake
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/p01-app ${D}${bindir}/p01-app
    install -m 0755 ${S}/p01-run.sh ${D}${bindir}/p01-run.sh

    install -d ${D}${datadir}/p01/models/yolov5_nano_pt
    install -m 0644 ${WORKDIR}/model/yolov5_nano_pt/yolov5_nano_pt.xmodel ${D}${datadir}/p01/models/yolov5_nano_pt/

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/p01.service ${D}${systemd_system_unitdir}/p01.service
}

FILES:${PN} += "${datadir}/p01 ${systemd_system_unitdir}/p01.service"
