# Video pipeline nodes (p01-video.dtsi) and kernel command line (p01-bootargs.dtsi).
# meta-enclustra-baseboard supplies the system-user.dtsi that is used and adds its includes to it in
# do_configure; these two files are added the same way, at the end.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " file://p01-video.dtsi file://p01-bootargs.dtsi"
do_configure:append() {
    echo "#include \"p01-video.dtsi\"" >> ../system-user.dtsi
    echo "#include \"p01-bootargs.dtsi\"" >> ../system-user.dtsi
}
