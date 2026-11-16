// p01-app - VART DPU runner
#include "dpu_runner.hpp"

#include <cmath>

#include "common.hpp"

namespace p01 {

namespace {

std::vector<int32_t> zero_idx(const xir::Tensor* t) { return std::vector<int32_t>(t->get_shape().size(), 0); }

}  // namespace

bool DpuRunner::load(const std::string& xmodel) {
  graph_ = xir::Graph::deserialize(xmodel);
  const xir::Subgraph* dpu_sg = nullptr;
  for (auto* sg : graph_->get_root_subgraph()->children_topological_sort()) {
    if (sg->has_attr("device") && sg->get_attr<std::string>("device") == "DPU") {
      dpu_sg = sg;
      break;
    }
  }
  if (!dpu_sg) {
    log("dpu: no DPU subgraph in %s", xmodel.c_str());
    return false;
  }
  kernel_ = dpu_sg->get_name();
  attrs_  = xir::Attrs::create();
  runner_ = vart::RunnerExt::create_runner(dpu_sg, attrs_.get());
  inputs_  = runner_->get_inputs();
  outputs_ = runner_->get_outputs();
  if (inputs_.size() != 1) {
    log("dpu: expected 1 input tensor, got %zu", inputs_.size());
    return false;
  }

  auto* in = inputs_[0];
  const auto* it = in->get_tensor();
  const auto shape = it->get_shape();  // N H W C
  in_h_   = shape[1];
  in_w_   = shape[2];
  in_fix_ = it->get_attr<int>("fix_point");
  const size_t need = size_t(in_h_) * in_w_ * shape[3];
  std::tie(in_phys_, in_size_) = in->data_phy(zero_idx(it));
  uint64_t va = 0;
  size_t   vsz = 0;
  std::tie(va, vsz) = in->data(zero_idx(it));
  in_virt_ = reinterpret_cast<int8_t*>(va);
  log("dpu: input %s %dx%dx%d fix %d, location %d, phys 0x%llx size %zu", it->get_name().c_str(),
      in_h_, in_w_, int(shape[3]), in_fix_, int(in->get_location()), (unsigned long long)in_phys_, in_size_);
  if (!in_phys_ || in_size_ < need) {
    in_phys_ = 0;
    in_size_ = vsz;
    if (!in_virt_ || vsz < need) {
      log("dpu: input tensor has neither a physical nor a usable virtual address");
      return false;
    }
    log("dpu: no physical address - p01_preproc output is copied into the input tensor");
  }

  for (auto* out : outputs_) {
    const auto* t  = out->get_tensor();
    const auto  s  = t->get_shape();
    uint64_t    va = 0;
    size_t      sz = 0;
    std::tie(va, sz) = out->data(zero_idx(t));
    Head hd;
    hd.data  = reinterpret_cast<const int8_t*>(va);
    hd.h     = s[1];
    hd.w     = s[2];
    hd.c     = s[3];
    hd.scale = std::ldexp(1.f, -t->get_attr<int>("fix_point"));
    heads_.push_back(hd);
    log("dpu: output %s %dx%dx%d scale %g", t->get_name().c_str(), hd.h, hd.w, hd.c, hd.scale);
  }
  return true;
}

bool DpuRunner::run() {
  auto* in = inputs_[0];
  // p01_preproc wrote the tensor through the PL; no CPU data in the cache to flush, but keep the
  // VART contract (no-op cost for a clean buffer).
  in->sync_for_write(0, in->get_tensor()->get_data_size() / in->get_tensor()->get_shape()[0]);
  const auto job = runner_->execute_async(inputs_, outputs_);
  if (job.second != 0 || runner_->wait(int(job.first), -1) != 0) {
    log("dpu: execution failed");
    return false;
  }
  for (auto* out : outputs_) {
    out->sync_for_read(0, out->get_tensor()->get_data_size() / out->get_tensor()->get_shape()[0]);
  }
  return true;
}

}  // namespace p01
