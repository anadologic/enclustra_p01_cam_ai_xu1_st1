# ----------------------------------------------------------------------------------------------------
# Re-creates the p01_xu1_st1 Vivado 2024.1 project in ../work/
#
#   cd VIVADO_2024.1
#   scripts/fetch_dpu_ip.sh                      (once: DPU IP -> ip_repo/)
#   scripts/fetch_vitis_vision.sh                (once: Vitis Vision library -> ext/)
#   vitis_hls -f hls/run_hls.tcl                 (p01_preproc IP -> ip_repo/)
#   vivado -mode batch -source scripts/create_project.tcl
# ----------------------------------------------------------------------------------------------------

set root_dir [file dirname [file dirname [file normalize [info script]]]]
source [file join $root_dir scripts settings.tcl]
source [file join $board_dir ps_config.tcl]
source [file join $root_dir bd dpu_hier.tcl]

foreach ip {DPUCZDX8G_v4_1_0 p01_preproc} {
  if {![file exists [file join $ip_repo_dir $ip component.xml]]} {
    error "${project_name}: ip_repo/$ip missing - run scripts/fetch_dpu_ip.sh and vitis_hls -f hls/run_hls.tcl first"
  }
}

create_project $project_name [file join $root_dir work] -part $part -force
set_property target_language    VHDL  [current_project]
set_property simulator_language Mixed [current_project]
set_property ip_repo_paths $ip_repo_dir [current_project]
update_ip_catalog

source [file join $root_dir bd bd.tcl]

set bd_file [get_files ${bd_name}.bd]
generate_target all $bd_file
add_files -norecurse [make_wrapper -files $bd_file -top]

add_files -norecurse [file join $root_dir hdl ${top_name}.vhd]
add_files -fileset constrs_1 -norecurse [file join $board_dir ${board_name}.xdc]
add_files -fileset constrs_1 -norecurse [file join $root_dir constraints ${project_name}.xdc]
set_property top $top_name [current_fileset]
update_compile_order -fileset sources_1

puts "INFO: ${project_name} project created in [file join $root_dir work]"
