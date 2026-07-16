// SPDX-License-Identifier: Apache-2.0
#include "safepoint.h"

#include <cstdlib>
#include <new>

namespace aevum {
namespace {

// Global mutator registry: a lock-free singly-linked stack. Nodes are pushed on
// first use by each thread and never removed (only flagged dead on thread exit),
// so the compactor can traverse it without hazard pointers. The node count is
// bounded by the number of distinct threads that ever touch the heap - a mobile
// app's bounded pool - so leaking dead nodes is acceptable and keeps traversal
// wait-free.
std::atomic<MutatorContext*> g_list{nullptr};

// thread_local holder so a context is released back to "dead" when its thread
// exits, without freeing memory the compactor might be reading.
struct MutatorTLS {
  MutatorContext* ctx = nullptr;
  ~MutatorTLS() noexcept {
    if (ctx) ctx->live.store(false, std::memory_order_release);
  }
};
thread_local MutatorTLS t_tls;

}  // namespace

MutatorContext* mutatorSelf() noexcept {
  MutatorContext* c = t_tls.ctx;
  if (c != nullptr) return c;

  c = new (std::nothrow) MutatorContext();
  if (c == nullptr) return nullptr;  // OOM: callers treat a null context as "no SATB"
  c->satb_storage = static_cast<void**>(std::calloc(kSatbCap, sizeof(void*)));
  if (c->satb_storage == nullptr) { delete c; return nullptr; }
  c->satb.init(c->satb_storage, kSatbCap);

  // Publish onto the registry (lock-free push).
  MutatorContext* head = g_list.load(std::memory_order_acquire);
  do {
    c->next = head;
  } while (!g_list.compare_exchange_weak(head, c,
                                         std::memory_order_release,
                                         std::memory_order_acquire));
  t_tls.ctx = c;
  return c;
}

MutatorContext* mutatorListHead() noexcept {
  return g_list.load(std::memory_order_acquire);
}

}  // namespace aevum
