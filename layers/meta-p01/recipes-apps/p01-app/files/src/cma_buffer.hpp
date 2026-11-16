// p01-app - physically contiguous, cached buffer from the DMA-BUF CMA heap.
//
// The physical address (needed by the PL) is read from /proc/self/pagemap, which requires
// root. The buffer comes from the CMA heap, so it is contiguous; alloc() verifies every page.
// The PL is not an attached dma-buf device, so the sync calls also do the cache maintenance
// themselves (DC CIVAC); begin_cpu_read() expects no CPU writes since the last end_cpu_write().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace p01 {

class CmaBuffer {
 public:
  ~CmaBuffer();
  bool     alloc(size_t bytes, const std::string& heap = "/dev/dma_heap/reserved");
  void     begin_cpu_write();  // DMA_BUF_IOCTL_SYNC start: before CPU writes
  void     end_cpu_write();    // DMA_BUF_IOCTL_SYNC end:   cache flush, PL may read afterwards
  void     begin_cpu_read();   // DMA_BUF_IOCTL_SYNC start: cache invalidate after PL writes
  void     end_cpu_read();
  uint8_t* virt() const { return virt_; }
  uint64_t phys() const { return phys_; }
  int      fd() const { return fd_; }  // dma-buf (e.g. for EGL_EXT_image_dma_buf_import)
  size_t   size() const { return size_; }

 private:
  int      fd_ = -1;
  uint8_t* virt_ = nullptr;
  uint64_t phys_ = 0;
  size_t   size_ = 0;
};

}  // namespace p01
