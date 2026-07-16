// SPDX-License-Identifier: Apache-2.0
#include "arena.h"

#include <cstring>
#include <utility>

#if defined(__linux__)
#  include <unistd.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <fcntl.h>
#  include <cerrno>
#elif defined(__APPLE__)
#  include <unistd.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <cerrno>
#endif

namespace aevum {

// atomics over mmap'd shared memory must be lock-free for the layout to be
// valid across processes/mappings; every A14/15 SoC satisfies this.
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "Aevum requires lock-free 64-bit atomics");

namespace {

#if defined(__linux__)
int aevum_memfd_create(const char* name, unsigned flags) noexcept {
#  if defined(SYS_memfd_create)
  return static_cast<int>(::syscall(SYS_memfd_create, name, flags));
#  else
  (void)name; (void)flags; errno = ENOSYS; return -1;
#  endif
}
#endif

void msyncRegion(void* base, uint64_t size) noexcept {
#if defined(MS_SYNC)
  ::msync(base, static_cast<size_t>(size), MS_SYNC);
#else
  (void)base; (void)size;
#endif
}

uint32_t slotCrc(CommitSlot slot) noexcept {
  slot.crc = 0;
  slot.reserved = 0;
  return crc32c(&slot, sizeof(CommitSlot));
}

}  // namespace

void Arena::moveFrom(Arena& o) noexcept {
  fd_ = o.fd_; owns_unlink_ = o.owns_unlink_;
  std::memcpy(path_, o.path_, sizeof(path_));
  base_ = o.base_; size_ = o.size_; hdr_ = o.hdr_;
  alloc_ = Allocator(base_, hdr_);
  dirty_ = std::move(o.dirty_);
  o.fd_ = -1; o.base_ = nullptr; o.size_ = 0; o.hdr_ = nullptr; o.owns_unlink_ = false;
}

void Arena::close() noexcept {
  // Disarm write-protection BEFORE unmapping so the tracker never mprotects a
  // freed range and the global registry slot is released first.
  dirty_.disarm();
  if (base_ != nullptr) {
    msyncRegion(base_, size_);
    ::munmap(base_, static_cast<size_t>(size_));
    base_ = nullptr; hdr_ = nullptr;
  }
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
  size_ = 0;
  alloc_ = Allocator();
}

Status Arena::mapAndBind(int fd, uint64_t size, bool creating) noexcept {
  void* p = ::mmap(nullptr, static_cast<size_t>(size),
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) return Status::kIoError;
  base_ = p;
  size_ = size;
  hdr_ = reinterpret_cast<RegionHeader*>(base_);
  fd_ = fd;
  if (creating) {
    formatFresh();
    alloc_ = Allocator(base_, hdr_);
    return Status::kOk;
  }
  Status s = adoptExisting();
  if (!ok(s)) { close(); return s; }
  alloc_ = Allocator(base_, hdr_);
  return Status::kOk;
}

void Arena::formatFresh() noexcept {
  std::memset(hdr_, 0, sizeof(RegionHeader));
  hdr_->magic = kMagic;
  hdr_->format_version = kFormatVer;
  hdr_->flags = 0;
  hdr_->region_bytes = size_;
  hdr_->data_start = alignUp(sizeof(RegionHeader), kPageSize);
  Allocator::initClasses(hdr_);

  hdr_->live_bump.store(hdr_->data_start, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kMaxClasses; ++i)
    hdr_->live_freelist[i].store(packHead(0, 0), std::memory_order_relaxed);
  for (uint32_t i = 0; i < kNumRoots; ++i)
    hdr_->live_root[i].store(Handle::kNull, std::memory_order_relaxed);
  hdr_->commit_lock.store(0, std::memory_order_relaxed);

  // Seed both checkpoint slots so attach always finds a valid one.
  CommitSlot& a = hdr_->commit[0];
  std::memset(&a, 0, sizeof(CommitSlot));
  a.generation = 1;
  a.bump = hdr_->data_start;
  a.crc = slotCrc(a);
  CommitSlot& b = hdr_->commit[1];
  std::memset(&b, 0, sizeof(CommitSlot));
  b.generation = 0;
  b.crc = slotCrc(b);
  hdr_->active_commit.store(0, std::memory_order_release);

  msyncRegion(base_, size_);
}

Status Arena::adoptExisting() noexcept {
  if (hdr_->magic != kMagic) return Status::kCorrupt;
  if (hdr_->format_version != kFormatVer) return Status::kUnsupported;
  if (hdr_->region_bytes != size_) return Status::kCorrupt;

  // Choose the newest checkpoint whose CRC validates.
  int chosen = -1;
  uint64_t bestGen = 0;
  for (int s = 0; s < 2; ++s) {
    const CommitSlot& slot = hdr_->commit[s];
    if (slotCrc(slot) != slot.crc) continue;         // torn / never-written
    if (chosen < 0 || slot.generation >= bestGen) { chosen = s; bestGen = slot.generation; }
  }
  if (chosen < 0) return Status::kCorrupt;

  const CommitSlot& slot = hdr_->commit[chosen];
  // Restore the live control block from the durable snapshot, discarding any
  // uncommitted mutations that a crash may have left half-applied.
  hdr_->live_bump.store(slot.bump, std::memory_order_relaxed);
  for (uint32_t i = 0; i < hdr_->num_classes && i < kMaxClasses; ++i)
    hdr_->live_freelist[i].store(packHead(slot.freelist[i], 0), std::memory_order_relaxed);
  for (uint32_t i = hdr_->num_classes; i < kMaxClasses; ++i)
    hdr_->live_freelist[i].store(packHead(0, 0), std::memory_order_relaxed);
  for (uint32_t i = 0; i < kNumRoots; ++i)
    hdr_->live_root[i].store(slot.root[i], std::memory_order_relaxed);
  hdr_->commit_lock.store(0, std::memory_order_relaxed);  // clear a lock orphaned by a dead writer
  hdr_->active_commit.store(static_cast<uint64_t>(chosen), std::memory_order_release);
  return Status::kOk;
}

Status Arena::createFile(const char* path, uint64_t size_bytes, Arena* out) noexcept {
  if (!path || !out || size_bytes < sizeof(RegionHeader) + kPageSize) return Status::kInvalidArg;
  const uint64_t size = alignUp(size_bytes, kPageSize);
  int fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return Status::kIoError;
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) { ::close(fd); return Status::kIoError; }
  std::strncpy(out->path_, path, sizeof(out->path_) - 1);
  return out->mapAndBind(fd, size, /*creating=*/true);
}

Status Arena::attachFile(const char* path, Arena* out) noexcept {
  if (!path || !out) return Status::kInvalidArg;
  int fd = ::open(path, O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) return Status::kIoError;
  struct stat st{};
  if (::fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < sizeof(RegionHeader)) {
    ::close(fd); return Status::kCorrupt;
  }
  std::strncpy(out->path_, path, sizeof(out->path_) - 1);
  return out->mapAndBind(fd, static_cast<uint64_t>(st.st_size), /*creating=*/false);
}

Status Arena::createMemfd(const char* name, uint64_t size_bytes, Arena* out) noexcept {
#if defined(__linux__)
  if (!name || !out || size_bytes < sizeof(RegionHeader) + kPageSize) return Status::kInvalidArg;
  const uint64_t size = alignUp(size_bytes, kPageSize);
#  if defined(MFD_CLOEXEC) && defined(MFD_ALLOW_SEALING)
  int fd = aevum_memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
#  else
  int fd = aevum_memfd_create(name, 0);
#  endif
  if (fd < 0) return Status::kUnsupported;
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) { ::close(fd); return Status::kIoError; }
#  if defined(F_ADD_SEALS) && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW)
  // Freeze the size so no holder can shrink the region out from under a mapping.
  ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
#  endif
  return out->mapAndBind(fd, size, /*creating=*/true);
#else
  (void)name; (void)size_bytes; (void)out;
  return Status::kUnsupported;
#endif
}

Status Arena::attachFd(int fd, Arena* out) noexcept {
#if defined(__linux__)
  if (fd < 0 || !out) return Status::kInvalidArg;
  struct stat st{};
  if (::fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) < sizeof(RegionHeader))
    return Status::kCorrupt;
  int dup = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);   // take our own owned handle
  if (dup < 0) return Status::kIoError;
  return out->mapAndBind(dup, static_cast<uint64_t>(st.st_size), /*creating=*/false);
#else
  (void)fd; (void)out;
  return Status::kUnsupported;
#endif
}

Status Arena::setRoot(uint32_t idx, Handle h) noexcept {
  if (!valid() || idx >= kNumRoots) return Status::kInvalidArg;
  hdr_->live_root[idx].store(h.off, std::memory_order_release);
  return Status::kOk;
}

Handle Arena::getRoot(uint32_t idx) noexcept {
  if (!valid() || idx >= kNumRoots) return Handle{Handle::kNull};
  return Handle{ hdr_->live_root[idx].load(std::memory_order_acquire) };
}

Status Arena::enableIncrementalCheckpoint() noexcept {
  if (!valid()) return Status::kInvalidArg;
  if (dirty_.armed()) return Status::kOk;
  const uint64_t hi = hdr_->live_bump.load(std::memory_order_acquire);
  return dirty_.arm(base_, hdr_->data_start, hi);
}

namespace {
struct FlushCtx { void* base; };
void flushRun(void* ctx, uint64_t off, uint64_t len) {
  auto* c = static_cast<FlushCtx*>(ctx);
#if defined(MS_SYNC)
  ::msync(static_cast<uint8_t*>(c->base) + off, static_cast<size_t>(len), MS_SYNC);
#else
  (void)c; (void)off; (void)len;
#endif
}
}  // namespace

Status Arena::commit() noexcept {
  if (!valid()) return Status::kInvalidArg;

  // Serialize concurrent committers within this process. Bounded spin; a writer
  // that dies here leaves active_commit pointing at the last good slot, and the
  // next attach clears this lock.
  uint32_t expected = 0;
  while (!hdr_->commit_lock.compare_exchange_weak(expected, 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
    expected = 0;
#if defined(__linux__) || defined(__APPLE__)
    ::sched_yield();
#endif
  }

  const uint64_t cur = hdr_->active_commit.load(std::memory_order_acquire);
  const uint64_t nxt = 1 - cur;
  CommitSlot& slot = hdr_->commit[nxt];

  slot.generation = hdr_->commit[cur].generation + 1;
  slot.bump = hdr_->live_bump.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < kMaxClasses; ++i)
    slot.freelist[i] = headOff(hdr_->live_freelist[i].load(std::memory_order_acquire));
  for (uint32_t i = 0; i < kNumRoots; ++i)
    slot.root[i] = hdr_->live_root[i].load(std::memory_order_acquire);
  slot.reserved = 0;
  slot.crc = 0;
  slot.crc = crc32c(&slot, sizeof(CommitSlot));

  // Flush data BEFORE publishing the new slot.
  if (dirty_.armed()) {
    // Header (offset 0..data_start) holds the checkpoint slots and is never
    // write-protected, so flush it explicitly; then flush only dirtied pages.
    const uint64_t bump = hdr_->live_bump.load(std::memory_order_acquire);
    msyncRegion(base_, hdr_->data_start);
    FlushCtx ctx{base_};
    dirty_.drainAndRearm(bump, &ctx, &flushRun);
  } else {
    msyncRegion(base_, size_);
  }
  hdr_->active_commit.store(nxt, std::memory_order_release);
  // Persist the pointer flip so a crash after this point recovers the new slot.
  msyncRegion(base_, hdr_->data_start);

  hdr_->commit_lock.store(0, std::memory_order_release);
  return Status::kOk;
}

}  // namespace aevum
