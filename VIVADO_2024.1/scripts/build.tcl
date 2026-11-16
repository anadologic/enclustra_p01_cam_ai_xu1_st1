# ----------------------------------------------------------------------------------------------------
# Builds p01_xu1_st1 (synthesis -> bitstream), checks timing and the DPU fingerprint, and exports
# the XSA and arch.json to ../export/
#
#   cd VIVADO_2024.1
#   vivado -mode batch -source scripts/build.tcl
# ----------------------------------------------------------------------------------------------------

set root_dir [file dirname [file dirname [file normalize [info script]]]]
source [file join $root_dir scripts settings.tcl]

if {[catch {current_project}]} {
  open_project [file join $root_dir work ${project_name}.xpr]
}

reset_run synth_1
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1

if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
  error "${project_name}: implementation failed, see [get_property DIRECTORY [get_runs impl_1]]"
}

open_run impl_1
set report_dir [file join $root_dir work reports]
file mkdir $report_dir
report_utilization    -file [file join $report_dir ${project_name}_utilization.rpt]
report_timing_summary -file [file join $report_dir ${project_name}_timing_summary.rpt]

set worst_path [get_timing_paths -quiet -max_paths 1 -nworst 1 -setup]
if {[llength $worst_path] == 0} {
  puts "INFO: ${project_name} has no constrained timing paths"
} else {
  set wns [get_property SLACK $worst_path]
  puts "INFO: ${project_name} worst setup slack: $wns ns"
  if {$wns < 0} {
    error "${project_name}: timing not met (WNS $wns ns)"
  }
}
set whs [get_property SLACK [get_timing_paths -quiet -max_paths 1 -nworst 1 -hold]]
puts "INFO: ${project_name} worst hold slack: $whs ns"
if {$whs ne "" && $whs < 0} {
  error "${project_name}: hold timing not met (WHS $whs ns)"
}

# DPU arch.json: the fingerprint must match the xmodels used by the application
set arch [lindex [glob -nocomplain [file join $root_dir work ${project_name}.gen sources_1 bd $bd_name ip * arch.json]] 0]
if {$arch eq ""} {
  error "${project_name}: arch.json not found"
}
set fh [open $arch r]; set arch_text [read $fh]; close $fh
if {![regexp -nocase "\"fingerprint\":\"$dpu_fingerprint\"" $arch_text]} {
  error "${project_name}: DPU fingerprint mismatch: $arch_text (expected $dpu_fingerprint)"
}
puts "INFO: ${project_name} DPU arch.json: $arch_text"

file mkdir [file join $root_dir export]
file copy -force $arch [file join $root_dir export arch.json]
write_hw_platform -fixed -include_bit -force -file [file join $root_dir export ${project_name}.xsa]
puts "INFO: exported [file join $root_dir export ${project_name}.xsa] and arch.json"
