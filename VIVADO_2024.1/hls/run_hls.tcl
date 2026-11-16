# ----------------------------------------------------------------------------------------------------
# p01_preproc 2.1 - Vitis HLS 2024.1 build: C simulation, synthesis, co-simulation, IP export.
#
#   cd VIVADO_2024.1
#   vitis_hls -f hls/run_hls.tcl                 (all steps)
#   vitis_hls -f hls/run_hls.tcl -tclargs nocosim
#   vitis_hls -f hls/run_hls.tcl -tclargs csim   (C simulation only)
#
# Needs:  ext/Vitis_Libraries (scripts/fetch_vitis_vision.sh): AMD Vitis Vision L1 headers
# Output: work/hls/p01_preproc (HLS project), ip_repo/p01_preproc (packaged IP, git-ignored),
#         work/hls/dump (raw test vectors for hls/check_vs_opencv.py)
# ----------------------------------------------------------------------------------------------------

set hls_dir  [file dirname [file normalize [info script]]]
set root_dir [file dirname $hls_dir]
set part     xczu6eg-ffvc900-1-i
set period   3.333
set run_cosim [expr {[lsearch $argv nocosim] < 0}]
set csim_only [expr {[lsearch $argv csim] >= 0}]
set vv_inc   [file join $root_dir ext Vitis_Libraries vision L1 include]

if {![string match "2024.1*" [version -short]]} {
  error "p01_preproc: Vitis HLS 2024.1 is required, running [version -short]"
}

if {![file exists [file join $vv_inc dnn xf_preprocess.hpp]]} {
  error "p01_preproc: Vitis Vision missing - run scripts/fetch_vitis_vision.sh first"
}

file delete -force [file join $root_dir work hls dump]
file mkdir [file join $root_dir work hls dump]
cd [file join $root_dir work hls]

open_project -reset p01_preproc
set_top p01_preproc
# Kernel: C++14 (the 2024.1 synthesis front end rejects the C++17 deduction guides of libstdc++).
# Testbench: C++17 (std::clamp in the reference model).
set inc "-D__SDSVHLS__ -I$vv_inc -I$hls_dir"
add_files      [file join $hls_dir p01_preproc.cpp] -cflags "-std=c++14 $inc"
add_files -tb  [file join $hls_dir tb_p01_preproc.cpp] -cflags "-std=c++17 $inc"
open_solution -reset sol1 -flow_target vivado
set_part $part
create_clock -period $period -name default
config_export -format ip_catalog -vendor anadologic.com -library p01 -version 2.1 \
    -display_name "p01_preproc" \
    -description "PL preprocessing (Vitis Vision): AXI4-Stream Video YUYV -> letterbox -> INT8 DPU tensor"

csim_design -argv "all [file join $root_dir work hls dump]"
if {$csim_only} { exit }
csynth_design
if {$run_cosim} {
  cosim_design -argv "cosim" -trace_level none
}
export_design

set ip_src [file join $root_dir work hls p01_preproc sol1 impl ip]
set ip_dst [file join $root_dir ip_repo p01_preproc]
file delete -force $ip_dst
file mkdir [file dirname $ip_dst]
file copy -force $ip_src $ip_dst
puts "INFO: p01_preproc IP exported to $ip_dst"
exit
