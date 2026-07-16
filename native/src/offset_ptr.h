// SPDX-License-Identifier: Apache-2.0
// Aevum - self-relative (base-independent) pointer.
//
// A raw pointer stored inside a persisted/shared region is worthless the moment
// the region is re-mapped at a different virtual address (ASLR guarantees it
// will be). A self-relative pointer stores the signed byte distance from its
// OWN address to the target. When the whole region is relocated - via mmap of a
// memfd at a fresh base, or a bulk memcpy - every pointer and its target shift
// by the identical delta, so the stored distance is invariant and the entire
// object graph stays valid with ZERO fixup. That invariant is the mechanism
// behind O(1) reattach across process death.
//
// This is a straightforward application of the classic offset-pointer idea
// (boost offset_ptr); the representation is standard and the reattach behavior
// follows from it directly.

#ifndef AEVUM_OFFSET_PTR_H_
#define AEVUM_OFFSET_PTR_H_

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace aevum {

// Null is encoded as offset value 1. Justification: within an Aevum arena every
// allocation is at least 16-byte aligned and every offset_ptr field is at least
// 8-byte aligned, so a real (this,target) distance is either 0 (a legitimate
// self-reference) or a multiple of 8 with magnitude >= 8. The value 1 is
// therefore unreachable for any real target and is a safe, collision-free null
// sentinel. Reserving 0 instead would clash with legitimate self-references.
inline constexpr int64_t kNullOffset = 1;

template <typename T>
class OffsetPtr {
 public:
  OffsetPtr() noexcept : rel_(kNullOffset) {}
  OffsetPtr(std::nullptr_t) noexcept : rel_(kNullOffset) {}  // NOLINT(runtime/explicit)
  explicit OffsetPtr(T* p) noexcept { assign(p); }

  // Copy/assign resolve the OTHER pointer to an absolute address, then re-encode
  // relative to THIS address. This is what makes ordinary pointer assignment
  // work across differently-located fields. Bulk region relocation does NOT go
  // through here - it relies on the raw bytes being position-independent.
  OffsetPtr(const OffsetPtr& o) noexcept { assign(o.get()); }
  OffsetPtr& operator=(const OffsetPtr& o) noexcept { assign(o.get()); return *this; }
  OffsetPtr& operator=(T* p) noexcept { assign(p); return *this; }
  OffsetPtr& operator=(std::nullptr_t) noexcept { rel_ = kNullOffset; return *this; }

  T* get() const noexcept {
    if (rel_ == kNullOffset) return nullptr;
    return reinterpret_cast<T*>(reinterpret_cast<intptr_t>(this) + rel_);
  }

  T& operator*()  const noexcept { return *get(); }
  T* operator->() const noexcept { return get(); }
  explicit operator bool() const noexcept { return rel_ != kNullOffset; }

  bool operator==(const OffsetPtr& o) const noexcept { return get() == o.get(); }
  bool operator!=(const OffsetPtr& o) const noexcept { return get() != o.get(); }
  bool isNull() const noexcept { return rel_ == kNullOffset; }

  // The raw encoded distance - used by the swizzle/persistence layer and the
  // allocator's intrusive free list, never by application code.
  int64_t raw() const noexcept { return rel_; }
  void setRaw(int64_t r) noexcept { rel_ = r; }

 private:
  void assign(T* p) noexcept {
    if (p == nullptr) { rel_ = kNullOffset; return; }
    rel_ = reinterpret_cast<intptr_t>(p) - reinterpret_cast<intptr_t>(this);
    // A real target can collide with the sentinel only if it lands exactly one
    // byte ahead of the field, which alignment forbids; assert-free by design.
  }

  int64_t rel_;
};

static_assert(sizeof(OffsetPtr<int>) == sizeof(int64_t),
              "OffsetPtr must be exactly one 64-bit word for stable on-disk layout");

// A base-relative handle: an offset measured from the arena base rather than
// from the field itself. This is the STABLE form handed across the JNI boundary
// and stored in the arena's root table - it survives being copied to any
// address (it does not depend on where the handle variable lives), unlike a
// self-relative field. Resolution requires the arena base.
struct Handle {
  uint64_t off;  // byte offset from arena base; 0 == null root slot.
  static constexpr uint64_t kNull = 0;
  bool isNull() const noexcept { return off == kNull; }
};

}  // namespace aevum

#endif  // AEVUM_OFFSET_PTR_H_
