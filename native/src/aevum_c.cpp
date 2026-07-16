// SPDX-License-Identifier: Apache-2.0
// Aevum - C ABI implementation over the C++ Arena.
//
// Each aev_arena* is a heap-owned Arena. All error paths return a status; no
// exception ever crosses this boundary (the core is noexcept throughout).

#include "aevum/aevum.h"

#include <new>

#include "arena.h"
#include "compactor.h"
#include "type_registry.h"
#include "self_healing_ptr.h"

using namespace aevum;

namespace {
inline Arena* self(aev_arena* a) { return reinterpret_cast<Arena*>(a); }
inline aev_arena* opaque(Arena* a) { return reinterpret_cast<aev_arena*>(a); }
inline void setStatus(int32_t* s, Status v) { if (s) *s = static_cast<int32_t>(v); }
inline ManagedHeap* mself(aev_mheap* h) { return reinterpret_cast<ManagedHeap*>(h); }
}  // namespace

extern "C" {

aev_arena* aev_create_file(const char* path, uint64_t size_bytes, int32_t* status) {
  auto* a = new (std::nothrow) Arena();
  if (!a) { setStatus(status, Status::kNoMemory); return nullptr; }
  Status s = Arena::createFile(path, size_bytes, a);
  setStatus(status, s);
  if (!ok(s)) { delete a; return nullptr; }
  return opaque(a);
}

aev_arena* aev_attach_file(const char* path, int32_t* status) {
  auto* a = new (std::nothrow) Arena();
  if (!a) { setStatus(status, Status::kNoMemory); return nullptr; }
  Status s = Arena::attachFile(path, a);
  setStatus(status, s);
  if (!ok(s)) { delete a; return nullptr; }
  return opaque(a);
}

aev_arena* aev_create_memfd(const char* name, uint64_t size_bytes, int32_t* status) {
  auto* a = new (std::nothrow) Arena();
  if (!a) { setStatus(status, Status::kNoMemory); return nullptr; }
  Status s = Arena::createMemfd(name, size_bytes, a);
  setStatus(status, s);
  if (!ok(s)) { delete a; return nullptr; }
  return opaque(a);
}

aev_arena* aev_attach_fd(int fd, int32_t* status) {
  auto* a = new (std::nothrow) Arena();
  if (!a) { setStatus(status, Status::kNoMemory); return nullptr; }
  Status s = Arena::attachFd(fd, a);
  setStatus(status, s);
  if (!ok(s)) { delete a; return nullptr; }
  return opaque(a);
}

void aev_close(aev_arena* a) { if (a) delete self(a); }  // ~Arena flushes + unmaps

int aev_fd(const aev_arena* a) {
  return a ? reinterpret_cast<const Arena*>(a)->fd() : -1;
}

aev_handle aev_alloc(aev_arena* a, uint64_t size) {
  return a ? self(a)->allocate(size).off : AEV_NULL;
}

int32_t aev_free(aev_arena* a, aev_handle h) {
  if (!a) return AEV_INVALID_ARG;
  return self(a)->free(Handle{h}) ? AEV_OK : AEV_INVALID_ARG;
}

void* aev_resolve(aev_arena* a, aev_handle h) {
  return a ? self(a)->resolve(Handle{h}) : nullptr;
}

uint64_t aev_capacity(aev_arena* a, aev_handle h) {
  return a ? self(a)->capacityOf(Handle{h}) : 0;
}

int32_t aev_set_root(aev_arena* a, uint32_t idx, aev_handle h) {
  if (!a) return AEV_INVALID_ARG;
  return static_cast<int32_t>(self(a)->setRoot(idx, Handle{h}));
}

aev_handle aev_get_root(aev_arena* a, uint32_t idx) {
  return a ? self(a)->getRoot(idx).off : AEV_NULL;
}

int32_t aev_commit(aev_arena* a) {
  if (!a) return AEV_INVALID_ARG;
  return static_cast<int32_t>(self(a)->commit());
}

int32_t aev_enable_incremental(aev_arena* a) {
  if (!a) return AEV_INVALID_ARG;
  return static_cast<int32_t>(self(a)->enableIncrementalCheckpoint());
}

uint64_t aev_bytes_in_use(aev_arena* a) {
  return a ? self(a)->bytesInUse() : 0;
}

/* ---- managed heap ---- */

int32_t aev_mh_register_layout(uint32_t type_id, const uint32_t* offsets,
                               uint32_t count, uint32_t size) {
  return static_cast<int32_t>(TypeRegistry::registerLayout(type_id, offsets, count, size));
}

aev_mheap* aev_mh_create(aev_arena* a, uint64_t space_bytes, int32_t* status) {
  if (!a) { setStatus(status, Status::kInvalidArg); return nullptr; }
  auto* h = new (std::nothrow) ManagedHeap();
  if (!h) { setStatus(status, Status::kNoMemory); return nullptr; }
  Status s = ManagedHeap::create(self(a), space_bytes, h);
  setStatus(status, s);
  if (!ok(s)) { delete h; return nullptr; }
  return reinterpret_cast<aev_mheap*>(h);
}

aev_mheap* aev_mh_attach(aev_arena* a, int32_t* status) {
  if (!a) { setStatus(status, Status::kInvalidArg); return nullptr; }
  auto* h = new (std::nothrow) ManagedHeap();
  if (!h) { setStatus(status, Status::kNoMemory); return nullptr; }
  Status s = ManagedHeap::attach(self(a), h);
  setStatus(status, s);
  if (!ok(s)) { delete h; return nullptr; }
  return reinterpret_cast<aev_mheap*>(h);
}

void aev_mh_close(aev_mheap* h) { if (h) delete mself(h); }

aev_handle aev_mh_alloc(aev_mheap* h, uint32_t type_id) {
  return h ? mself(h)->alloc(type_id).off : AEV_NULL;
}
void* aev_mh_read(aev_mheap* h, aev_handle ref) {
  return h ? mself(h)->resolveRead(ManagedRef{ref}) : nullptr;
}
void* aev_mh_begin_write(aev_mheap* h, aev_handle ref) {
  return h ? mself(h)->beginWrite(ManagedRef{ref}) : nullptr;
}
void aev_mh_end_write(aev_mheap* h) { if (h) mself(h)->endWrite(); }
aev_handle aev_mh_ref_of(aev_mheap* h, void* payload) {
  return h ? mself(h)->refOf(payload).off : AEV_NULL;
}
int32_t aev_mh_set_root(aev_mheap* h, uint32_t idx, aev_handle ref) {
  return h ? static_cast<int32_t>(mself(h)->setRoot(idx, ManagedRef{ref})) : AEV_INVALID_ARG;
}
aev_handle aev_mh_get_root(aev_mheap* h, uint32_t idx) {
  return h ? mself(h)->getRoot(idx).off : AEV_NULL;
}
aev_handle aev_mh_read_ref(aev_mheap* h, aev_handle ref, uint32_t field_offset) {
  if (!h) return AEV_NULL;
  void* payload = mself(h)->resolveRead(ManagedRef{ref});
  if (!payload) return AEV_NULL;
  auto* fld = reinterpret_cast<SelfHealingRefBase*>(static_cast<uint8_t*>(payload) + field_offset);
  void* child = fld->resolvePayload();
  return child ? mself(h)->refOf(child).off : AEV_NULL;
}
int32_t aev_mh_write_ref(aev_mheap* h, aev_handle ref, uint32_t field_offset, aev_handle child) {
  if (!h) return AEV_INVALID_ARG;
  void* payload = mself(h)->beginWrite(ManagedRef{ref});
  if (!payload) { mself(h)->endWrite(); return AEV_INVALID_ARG; }
  void* childp = child ? mself(h)->resolveRead(ManagedRef{child}) : nullptr;
  auto* fld = reinterpret_cast<SelfHealingRefBase*>(static_cast<uint8_t*>(payload) + field_offset);
  fld->storeRef(childp);
  mself(h)->endWrite();
  return AEV_OK;
}
int32_t aev_mh_compact(aev_mheap* h, uint64_t* out_live, uint64_t* out_reclaimed) {
  if (!h) return AEV_INVALID_ARG;
  CompactStats st;
  Status s = mself(h)->compact(&st);
  if (out_live) *out_live = st.live_objects;
  if (out_reclaimed) *out_reclaimed = st.reclaimed_bytes;
  return static_cast<int32_t>(s);
}
uint64_t aev_mh_bytes_in_use(aev_mheap* h) {
  return h ? mself(h)->liveBumpBytes() : 0;
}

}  // extern "C"
