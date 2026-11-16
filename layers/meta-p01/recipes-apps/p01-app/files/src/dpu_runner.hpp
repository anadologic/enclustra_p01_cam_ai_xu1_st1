// p01-app - YOLO xmodel on the DPU through VART (RunnerExt, zero-copy input).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <vart/runner_ext.hpp>
#include <xir/attrs/attrs.hpp>
#include <xir/graph/graph.hpp>

namespace p01 {

class DpuRunner {
 public:
  struct Head {
    const int8_t* data = nullptr;  // NHWC int8
    int           h = 0, w = 0, c = 0;
    float         scale = 1.f;     // 2^-fix_point
  };

  bool load(const std::string& xmodel);
  bool run();  // DPU on the current input tensor, outputs synced for CPU read

  // Input tensor: p01_preproc writes it directly (physical address). If VART gives no physical
  // address (input_phys() == 0), the caller copies the tensor to input_virt() before run().
  uint64_t input_phys() const { return in_phys_; }
  int8_t*  input_virt() const { return in_virt_; }
  size_t   input_size() const { return in_size_; }
  int      input_w() const { return in_w_; }
  int      input_h() const { return in_h_; }
  int      input_fix() const { return in_fix_; }
  const std::vector<Head>& heads() const { return heads_; }
  std::string              kernel_name() const { return kernel_; }

 private:
  std::unique_ptr<xir::Graph>       graph_;
  std::unique_ptr<xir::Attrs>       attrs_;
  std::unique_ptr<vart::RunnerExt>  runner_;
  std::vector<vart::TensorBuffer*>  inputs_, outputs_;
  std::vector<Head>                 heads_;
  uint64_t                          in_phys_ = 0;
  int8_t*                           in_virt_ = nullptr;
  size_t                            in_size_ = 0;
  int                               in_w_ = 0, in_h_ = 0, in_fix_ = 0;
  std::string                       kernel_;
};

}  // namespace p01
