/* SPDX-License-Identifier: Apache-2.0
 * Aevum - public C ABI.
 *
 * A stable C surface over the Aevum arena for NDK/C++ consumers and for the
 * JNI bridge. The arena is a GC-invisible, position-independent, persistable
 * region: allocations are addressed by a durable `aev_handle` (a base-relative
 * offset) that stays valid across process death and re-mapping at a new base.
 *
 * Thread-safety: alloc/free/resolve/root ops are safe to call concurrently on
 * one arena from any thread. create/attach/close are not; serialize them.
 * All functions are non-throwing and never call back into the JVM.
 */
#ifndef AEVUM_PUBLIC_H_
#define AEVUM_PUBLIC_H_

#include <stdint.h>

#if defined(_WIN32)
#  define AEV_API
#else
#  define AEV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aev_arena aev_arena;   /* opaque owner of one fd + mapping */
typedef uint64_t aev_handle;          /* durable base-relative offset; 0 == null */

#define AEV_NULL           ((aev_handle)0)

/* Status codes mirror aevum::Status. */
#define AEV_OK              0
#define AEV_NO_MEMORY      (-1)
#define AEV_INVALID_ARG    (-2)
#define AEV_UNSUPPORTED    (-3)
#define AEV_CORRUPT        (-4)
#define AEV_ALREADY        (-5)
#define AEV_IO_ERROR       (-6)
#define AEV_PROBE_FAILED   (-7)

/* --- lifecycle. Return NULL on failure and write the status via *status. --- */
AEV_API aev_arena* aev_create_file (const char* path, uint64_t size_bytes, int32_t* status);
AEV_API aev_arena* aev_attach_file (const char* path,                      int32_t* status);
AEV_API aev_arena* aev_create_memfd(const char* name, uint64_t size_bytes, int32_t* status);
AEV_API aev_arena* aev_attach_fd   (int fd,                                int32_t* status);

/* Flush, unmap, close, and free the arena object. Safe on NULL. */
AEV_API void       aev_close(aev_arena* a);

/* The backing memfd (for SCM_RIGHTS handoff), or -1 for file-backed arenas. */
AEV_API int        aev_fd(const aev_arena* a);

/* --- allocation --- */
AEV_API aev_handle aev_alloc   (aev_arena* a, uint64_t size);   /* AEV_NULL if exhausted / too large */
AEV_API int32_t    aev_free    (aev_arena* a, aev_handle h);    /* AEV_OK, or AEV_INVALID_ARG on double-free */
AEV_API void*      aev_resolve (aev_arena* a, aev_handle h);    /* absolute ptr in THIS mapping, or NULL */
AEV_API uint64_t   aev_capacity(aev_arena* a, aev_handle h);    /* usable payload bytes of the block */

/* --- durable graph roots (0..63) --- */
AEV_API int32_t    aev_set_root(aev_arena* a, uint32_t idx, aev_handle h);
AEV_API aev_handle aev_get_root(aev_arena* a, uint32_t idx);

/* --- durability --- */
AEV_API int32_t    aev_commit(aev_arena* a);                 /* crash-consistent checkpoint */
AEV_API int32_t    aev_enable_incremental(aev_arena* a);     /* AEV_OK / AEV_PROBE_FAILED (non-fatal) */
AEV_API uint64_t   aev_bytes_in_use(aev_arena* a);

/* ------------------------------------------------------------------------ *
 * Aevum-Defrag managed heap (C ABI).                                         *
 * A copying, no-stop-the-world object heap carved from an arena. Types are   *
 * registered by the byte offsets of their reference fields (no native trace  *
 * code needed), which is what lets a JVM/Kotlin layer drive it.              *
 * ------------------------------------------------------------------------ */
typedef struct aev_mheap aev_mheap;

/* Register a managed type: `offsets` are the byte offsets of the object's
 * self-healing reference fields; `size` is the payload size. Idempotent.
 * Returns AEV_INVALID_ARG (and leaves any earlier registration untouched) if
 * `size` is 0 or above 0x7FFFFFF0, `count` exceeds 32, or an offset is not
 * 8-aligned, does not fit a whole 8-byte field inside `size`, or is repeated. */
AEV_API int32_t   aev_mh_register_layout(uint32_t type_id, const uint32_t* offsets,
                                         uint32_t count, uint32_t size);

AEV_API aev_mheap* aev_mh_create(aev_arena* a, uint64_t space_bytes, int32_t* status);
AEV_API aev_mheap* aev_mh_attach(aev_arena* a, int32_t* status);
AEV_API void       aev_mh_close(aev_mheap* h);

AEV_API aev_handle aev_mh_alloc(aev_mheap* h, uint32_t type_id);   /* managed ref, or 0 */
AEV_API void*      aev_mh_read(aev_mheap* h, aev_handle ref);      /* canonical read ptr */
AEV_API void*      aev_mh_begin_write(aev_mheap* h, aev_handle ref); /* pair w/ end_write */
AEV_API void       aev_mh_end_write(aev_mheap* h);
AEV_API aev_handle aev_mh_ref_of(aev_mheap* h, void* canonical_payload);

AEV_API int32_t    aev_mh_set_root(aev_mheap* h, uint32_t idx, aev_handle ref);
AEV_API aev_handle aev_mh_get_root(aev_mheap* h, uint32_t idx);

/* Resolve the reference field at `field_offset` inside object `ref` to the child
 * managed ref (following forwarding), or set it. These wrap the self-healing
 * pointer + WriteScope so a JVM caller can walk and mutate the graph without
 * touching pointer internals. */
AEV_API aev_handle aev_mh_read_ref(aev_mheap* h, aev_handle ref, uint32_t field_offset);
AEV_API int32_t    aev_mh_write_ref(aev_mheap* h, aev_handle ref, uint32_t field_offset,
                                    aev_handle child);

/* Run one concurrent compaction cycle. Writes live-object / reclaimed-byte
 * counts through the out params if non-NULL. Returns AEV_OK or a status. */
AEV_API int32_t    aev_mh_compact(aev_mheap* h, uint64_t* out_live, uint64_t* out_reclaimed);
AEV_API uint64_t   aev_mh_bytes_in_use(aev_mheap* h);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* AEVUM_PUBLIC_H_ */
