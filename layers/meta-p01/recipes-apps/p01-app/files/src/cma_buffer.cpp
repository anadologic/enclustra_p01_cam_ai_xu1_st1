// p01-app - DMA-BUF CMA heap buffer
#include "cma_buffer.hpp"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common.hpp"

namespace p01 {

namespace {

constexpr size_t kPage = 4096;

// DC CIVAC (clean + invalidate to the point of coherency) over [p, p + n). Allowed at EL0: Linux
// sets SCTLR_EL1.UCI. Needed because DMA_BUF_IOCTL_SYNC on a CMA heap buffer only maintains the
// caches for attached devices, and the PL uses these buffers by physical address (no attachment).
void cache_clean_invalidate(const void* p, size_t n) {
  uint64_t ctr;
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 0xf);  // CTR_EL0.DminLine (words)
  const uintptr_t end  = uintptr_t(p) + n;
  for (uintptr_t a = uintptr_t(p) & ~(line - 1); a < end; a += line) {
    asm volatile("dc civac, %0" : : "r"(a) : "memory");
  }
  asm volatile("dsb sy" : : : "memory");
}

uint64_t pagemap_pfn(int fd, uintptr_t va) {
  uint64_t e = 0;
  if (pread(fd, &e, sizeof(e), off_t(va / kPage) * 8) != sizeof(e)) return 0;
  if (!(e >> 63)) return 0;  // not present
  return e & ((1ULL << 55) - 1);
}

}  // namespace

CmaBuffer::~CmaBuffer() {
  if (virt_) munmap(virt_, size_);
  if (fd_ >= 0) close(fd_);
}

bool CmaBuffer::alloc(size_t bytes, const std::string& heap) {
  size_ = (bytes + kPage - 1) & ~(kPage - 1);
  const int hfd = open(heap.c_str(), O_RDWR | O_CLOEXEC);
  if (hfd < 0) {
    log("cma: cannot open %s: %s", heap.c_str(), std::strerror(errno));
    return false;
  }
  dma_heap_allocation_data a{};
  a.len      = size_;
  a.fd_flags = O_RDWR | O_CLOEXEC;
  const int r = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a);
  close(hfd);
  if (r < 0) {
    log("cma: allocation of %zu bytes failed: %s", size_, std::strerror(errno));
    return false;
  }
  fd_   = int(a.fd);
  void* p = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    log("cma: mmap failed: %s", std::strerror(errno));
    return false;
  }
  virt_ = static_cast<uint8_t*>(p);
  std::memset(virt_, 0, size_);  // fault all pages in

  const int pm = open("/proc/self/pagemap", O_RDONLY);
  if (pm < 0) {
    log("cma: cannot open pagemap (run as root)");
    return false;
  }
  const uint64_t pfn0 = pagemap_pfn(pm, uintptr_t(virt_));
  bool contiguous = pfn0 != 0;
  for (size_t off = kPage; contiguous && off < size_; off += kPage) {
    contiguous = pagemap_pfn(pm, uintptr_t(virt_) + off) == pfn0 + off / kPage;
  }
  close(pm);
  if (!contiguous) {
    log("cma: buffer is not physically contiguous (or PFNs hidden: run as root)");
    return false;
  }
  phys_ = pfn0 * kPage;
  cache_clean_invalidate(virt_, size_);  // the zeroing above must reach DDR before the PL writes
  return true;
}

void CmaBuffer::begin_cpu_write() {
  dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
  ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
}

void CmaBuffer::end_cpu_write() {
  dma_buf_sync s{DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE};
  ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
  cache_clean_invalidate(virt_, size_);  // CPU data -> DDR
}

void CmaBuffer::begin_cpu_read() {
  dma_buf_sync s{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
  ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
  cache_clean_invalidate(virt_, size_);  // drop lines cached before the PL wrote (all clean here)
}

void CmaBuffer::end_cpu_read() {
  dma_buf_sync s{DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
  ioctl(fd_, DMA_BUF_IOCTL_SYNC, &s);
}

}  // namespace p01
