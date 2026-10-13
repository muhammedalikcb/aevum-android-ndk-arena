// SPDX-License-Identifier: Apache-2.0
// Aevum - JNI bridge (dev.aevum.AevumNative).
//
// A thin, allocation-disciplined bridge from Kotlin to the C ABI. Design rules
// honored here:
//   * Natives are bound via RegisterNatives in JNI_OnLoad - no reliance on
//     symbol name mangling, and the binding fails loudly if a signature drifts.
//   * No global references are held; the only long-lived native state (the
//     arena) is owned by C++ and referenced from Kotlin as an opaque jlong.
//   * resolve() returns a direct ByteBuffer aliasing the payload in place, so
//     reads/writes are zero-copy with no per-access JNI call.
//   * The native fault-handler thread path (dirty tracker) never calls into the
//     JVM, so there is no AttachCurrentThread/Detach discipline to get wrong and
//     no risk of a native thread leaking a JNIEnv.
//   * Every jstring is released on every path, including early returns.

#include <jni.h>
#include <cstring>

#include "aevum/aevum.h"

namespace {

// RAII for GetStringUTFChars so no path leaks the chars.
class Utf {
 public:
  Utf(JNIEnv* env, jstring s) : env_(env), s_(s) {
    c_ = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
  }
  ~Utf() { if (s_ && c_) env_->ReleaseStringUTFChars(s_, c_); }
  const char* c() const { return c_; }
  Utf(const Utf&) = delete; Utf& operator=(const Utf&) = delete;
 private:
  JNIEnv* env_; jstring s_; const char* c_;
};

jlongArray makeResult(JNIEnv* env, jlong ptr, jint status) {
  jlongArray arr = env->NewLongArray(2);
  if (!arr) return nullptr;
  jlong vals[2] = { ptr, static_cast<jlong>(status) };
  env->SetLongArrayRegion(arr, 0, 2, vals);
  return arr;
}

inline aev_arena* A(jlong p) { return reinterpret_cast<aev_arena*>(p); }

// --- native method implementations -----------------------------------------

jlongArray nCreateFile(JNIEnv* env, jclass, jstring path, jlong size) {
  Utf p(env, path);
  int32_t st = AEV_INVALID_ARG;
  aev_arena* a = p.c() ? aev_create_file(p.c(), static_cast<uint64_t>(size), &st) : nullptr;
  return makeResult(env, reinterpret_cast<jlong>(a), st);
}
jlongArray nAttachFile(JNIEnv* env, jclass, jstring path) {
  Utf p(env, path);
  int32_t st = AEV_INVALID_ARG;
  aev_arena* a = p.c() ? aev_attach_file(p.c(), &st) : nullptr;
  return makeResult(env, reinterpret_cast<jlong>(a), st);
}
jlongArray nCreateMemfd(JNIEnv* env, jclass, jstring name, jlong size) {
  Utf p(env, name);
  int32_t st = AEV_INVALID_ARG;
  aev_arena* a = p.c() ? aev_create_memfd(p.c(), static_cast<uint64_t>(size), &st) : nullptr;
  return makeResult(env, reinterpret_cast<jlong>(a), st);
}
jlongArray nAttachFd(JNIEnv* env, jclass, jint fd) {
  int32_t st = AEV_INVALID_ARG;
  aev_arena* a = aev_attach_fd(fd, &st);
  return makeResult(env, reinterpret_cast<jlong>(a), st);
}
void nClose(JNIEnv*, jclass, jlong ptr) { aev_close(A(ptr)); }
jint nFd(JNIEnv*, jclass, jlong ptr) { return aev_fd(A(ptr)); }
jlong nAlloc(JNIEnv*, jclass, jlong ptr, jlong size) {
  return static_cast<jlong>(aev_alloc(A(ptr), static_cast<uint64_t>(size)));
}
jint nFree(JNIEnv*, jclass, jlong ptr, jlong h) {
  return aev_free(A(ptr), static_cast<aev_handle>(h));
}
jobject nResolve(JNIEnv* env, jclass, jlong ptr, jlong h) {
  void* p = aev_resolve(A(ptr), static_cast<aev_handle>(h));
  if (!p) return nullptr;
  uint64_t cap = aev_capacity(A(ptr), static_cast<aev_handle>(h));
  if (cap == 0) return nullptr;
  return env->NewDirectByteBuffer(p, static_cast<jlong>(cap));  // zero-copy alias
}
jlong nCapacity(JNIEnv*, jclass, jlong ptr, jlong h) {
  return static_cast<jlong>(aev_capacity(A(ptr), static_cast<aev_handle>(h)));
}
jint nSetRoot(JNIEnv*, jclass, jlong ptr, jint idx, jlong h) {
  return aev_set_root(A(ptr), static_cast<uint32_t>(idx), static_cast<aev_handle>(h));
}
jlong nGetRoot(JNIEnv*, jclass, jlong ptr, jint idx) {
  return static_cast<jlong>(aev_get_root(A(ptr), static_cast<uint32_t>(idx)));
}
jint nCommit(JNIEnv*, jclass, jlong ptr) { return aev_commit(A(ptr)); }
jint nEnableIncremental(JNIEnv*, jclass, jlong ptr) { return aev_enable_incremental(A(ptr)); }
jlong nBytesInUse(JNIEnv*, jclass, jlong ptr) {
  return static_cast<jlong>(aev_bytes_in_use(A(ptr)));
}

// --- managed heap (Aevum-Defrag) ---

jint nMhRegisterLayout(JNIEnv* env, jclass, jint typeId, jintArray offsets, jint size) {
  jint* o = offsets ? env->GetIntArrayElements(offsets, nullptr) : nullptr;
  const jsize n = offsets ? env->GetArrayLength(offsets) : 0;
  const jint rc = aev_mh_register_layout(static_cast<uint32_t>(typeId),
                                         reinterpret_cast<const uint32_t*>(o),
                                         static_cast<uint32_t>(n), static_cast<uint32_t>(size));
  if (o) env->ReleaseIntArrayElements(offsets, o, JNI_ABORT);
  return rc;
}
jlongArray nMhCreate(JNIEnv* env, jclass, jlong arenaPtr, jlong spaceBytes) {
  int32_t st = AEV_INVALID_ARG;
  aev_mheap* m = aev_mh_create(reinterpret_cast<aev_arena*>(arenaPtr),
                               static_cast<uint64_t>(spaceBytes), &st);
  return makeResult(env, reinterpret_cast<jlong>(m), st);
}
jlongArray nMhAttach(JNIEnv* env, jclass, jlong arenaPtr) {
  int32_t st = AEV_INVALID_ARG;
  aev_mheap* m = aev_mh_attach(reinterpret_cast<aev_arena*>(arenaPtr), &st);
  return makeResult(env, reinterpret_cast<jlong>(m), st);
}
inline aev_mheap* M(jlong p) { return reinterpret_cast<aev_mheap*>(p); }
void  nMhClose(JNIEnv*, jclass, jlong p) { aev_mh_close(M(p)); }
jlong nMhAlloc(JNIEnv*, jclass, jlong p, jint t) { return static_cast<jlong>(aev_mh_alloc(M(p), static_cast<uint32_t>(t))); }
// A view may cover at most the object's allocated payload. A negative size, or
// one past the object, would let Kotlin touch the next object or, at the end of
// a semispace, memory past the mapping.
inline bool viewFits(jlong p, jlong ref, jint size) {
  return size >= 0 &&
         static_cast<uint64_t>(size) <= aev_mh_capacity(M(p), static_cast<aev_handle>(ref));
}
jobject nMhRead(JNIEnv* env, jclass, jlong p, jlong ref, jint size) {
  if (!viewFits(p, ref, size)) return nullptr;
  void* q = aev_mh_read(M(p), static_cast<aev_handle>(ref));
  return q ? env->NewDirectByteBuffer(q, size) : nullptr;
}
jobject nMhBeginWrite(JNIEnv* env, jclass, jlong p, jlong ref, jint size) {
  // Always begin: on a null return the Kotlin side still calls mhEndWrite.
  void* q = aev_mh_begin_write(M(p), static_cast<aev_handle>(ref));
  return q && viewFits(p, ref, size) ? env->NewDirectByteBuffer(q, size) : nullptr;
}
void  nMhEndWrite(JNIEnv*, jclass, jlong p) { aev_mh_end_write(M(p)); }
jint  nMhSetRoot(JNIEnv*, jclass, jlong p, jint i, jlong ref) { return aev_mh_set_root(M(p), static_cast<uint32_t>(i), static_cast<aev_handle>(ref)); }
jlong nMhGetRoot(JNIEnv*, jclass, jlong p, jint i) { return static_cast<jlong>(aev_mh_get_root(M(p), static_cast<uint32_t>(i))); }
jlong nMhReadRef(JNIEnv*, jclass, jlong p, jlong ref, jint off) { return static_cast<jlong>(aev_mh_read_ref(M(p), static_cast<aev_handle>(ref), static_cast<uint32_t>(off))); }
jint  nMhWriteRef(JNIEnv*, jclass, jlong p, jlong ref, jint off, jlong child) { return aev_mh_write_ref(M(p), static_cast<aev_handle>(ref), static_cast<uint32_t>(off), static_cast<aev_handle>(child)); }
jlongArray nMhCompact(JNIEnv* env, jclass, jlong p) {
  uint64_t live = 0, recl = 0;
  int32_t st = aev_mh_compact(M(p), &live, &recl);
  jlongArray arr = env->NewLongArray(3);
  if (!arr) return nullptr;
  jlong vals[3] = { static_cast<jlong>(st), static_cast<jlong>(live), static_cast<jlong>(recl) };
  env->SetLongArrayRegion(arr, 0, 3, vals);
  return arr;
}
jlong nMhBytesInUse(JNIEnv*, jclass, jlong p) { return static_cast<jlong>(aev_mh_bytes_in_use(M(p))); }

const JNINativeMethod kMethods[] = {
  {"createFile",        "(Ljava/lang/String;J)[J",                reinterpret_cast<void*>(nCreateFile)},
  {"attachFile",        "(Ljava/lang/String;)[J",                 reinterpret_cast<void*>(nAttachFile)},
  {"createMemfd",       "(Ljava/lang/String;J)[J",                reinterpret_cast<void*>(nCreateMemfd)},
  {"attachFd",          "(I)[J",                                  reinterpret_cast<void*>(nAttachFd)},
  {"close",             "(J)V",                                   reinterpret_cast<void*>(nClose)},
  {"fd",                "(J)I",                                   reinterpret_cast<void*>(nFd)},
  {"alloc",             "(JJ)J",                                  reinterpret_cast<void*>(nAlloc)},
  {"free",              "(JJ)I",                                  reinterpret_cast<void*>(nFree)},
  {"resolve",           "(JJ)Ljava/nio/ByteBuffer;",              reinterpret_cast<void*>(nResolve)},
  {"capacity",          "(JJ)J",                                  reinterpret_cast<void*>(nCapacity)},
  {"setRoot",           "(JIJ)I",                                 reinterpret_cast<void*>(nSetRoot)},
  {"getRoot",           "(JI)J",                                  reinterpret_cast<void*>(nGetRoot)},
  {"commit",            "(J)I",                                   reinterpret_cast<void*>(nCommit)},
  {"enableIncremental", "(J)I",                                   reinterpret_cast<void*>(nEnableIncremental)},
  {"bytesInUse",        "(J)J",                                   reinterpret_cast<void*>(nBytesInUse)},
  {"mhRegisterLayout",  "(I[II)I",                                reinterpret_cast<void*>(nMhRegisterLayout)},
  {"mhCreate",          "(JJ)[J",                                 reinterpret_cast<void*>(nMhCreate)},
  {"mhAttach",          "(J)[J",                                  reinterpret_cast<void*>(nMhAttach)},
  {"mhClose",           "(J)V",                                   reinterpret_cast<void*>(nMhClose)},
  {"mhAlloc",           "(JI)J",                                  reinterpret_cast<void*>(nMhAlloc)},
  {"mhRead",            "(JJI)Ljava/nio/ByteBuffer;",             reinterpret_cast<void*>(nMhRead)},
  {"mhBeginWrite",      "(JJI)Ljava/nio/ByteBuffer;",             reinterpret_cast<void*>(nMhBeginWrite)},
  {"mhEndWrite",        "(J)V",                                   reinterpret_cast<void*>(nMhEndWrite)},
  {"mhSetRoot",         "(JIJ)I",                                 reinterpret_cast<void*>(nMhSetRoot)},
  {"mhGetRoot",         "(JI)J",                                  reinterpret_cast<void*>(nMhGetRoot)},
  {"mhReadRef",         "(JJI)J",                                 reinterpret_cast<void*>(nMhReadRef)},
  {"mhWriteRef",        "(JJIJ)I",                                reinterpret_cast<void*>(nMhWriteRef)},
  {"mhCompact",         "(J)[J",                                  reinterpret_cast<void*>(nMhCompact)},
  {"mhBytesInUse",      "(J)J",                                   reinterpret_cast<void*>(nMhBytesInUse)},
};

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("dev/aevum/AevumNative");
  if (!cls) return JNI_ERR;  // an ExceptionDescribe-worthy failure; binding aborts load
  jint rc = env->RegisterNatives(cls, kMethods,
                                 static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0])));
  env->DeleteLocalRef(cls);
  return rc == 0 ? JNI_VERSION_1_6 : JNI_ERR;
}
