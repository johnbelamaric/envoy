#include "thread_local_store.h"

namespace Stats {

ThreadLocalStoreImpl::ThreadLocalStoreImpl(RawStatDataAllocator& alloc)
    : alloc_(alloc), default_scope_(createScope("")),
      num_last_resort_stats_(default_scope_->counter("stats.overflow")) {}

ThreadLocalStoreImpl::~ThreadLocalStoreImpl() {
  destroying_ = true;
  default_scope_.reset();
  ASSERT(scopes_.empty());
}

std::list<CounterPtr> ThreadLocalStoreImpl::counters() const {
  // Handle de-dup due to overlapping scopes.
  std::list<CounterPtr> ret;
  std::unordered_set<std::string> names;
  std::unique_lock<std::mutex> lock(lock_);
  for (ScopeImpl* scope : scopes_) {
    for (auto counter : scope->central_cache_.counters_) {
      if (names.insert(counter.first).second) {
        ret.push_back(counter.second);
      }
    }
  }

  return ret;
}

ScopePtr ThreadLocalStoreImpl::createScope(const std::string& name) {
  std::unique_ptr<ScopeImpl> new_scope(new ScopeImpl(*this, name));
  std::unique_lock<std::mutex> lock(lock_);
  scopes_.emplace(new_scope.get());
  return std::move(new_scope);
}

std::list<GaugePtr> ThreadLocalStoreImpl::gauges() const {
  // Handle de-dup due to overlapping scopes.
  std::list<GaugePtr> ret;
  std::unordered_set<std::string> names;
  std::unique_lock<std::mutex> lock(lock_);
  for (ScopeImpl* scope : scopes_) {
    for (auto gauge : scope->central_cache_.gauges_) {
      if (names.insert(gauge.first).second) {
        ret.push_back(gauge.second);
      }
    }
  }

  return ret;
}

void ThreadLocalStoreImpl::initializeThreading(Event::Dispatcher& main_thread_dispatcher,
                                               ThreadLocal::Instance& tls) {
  main_thread_dispatcher_ = &main_thread_dispatcher;
  tls_ = &tls;
  tls_slot_ = tls_->allocateSlot();
  tls_->set(tls_slot_, [](Event::Dispatcher&) -> ThreadLocal::ThreadLocalObjectPtr {
    return ThreadLocal::ThreadLocalObjectPtr{new TlsCache()};
  });
}

void ThreadLocalStoreImpl::releaseScopeCrossThread(ScopeImpl* scope) {
  std::unique_lock<std::mutex> lock(lock_);
  ASSERT(scopes_.count(scope) == 1);
  scopes_.erase(scope);

  // This can happen from any thread. We post() back to the main thread which will initiate the
  // cache flush operation.
  if (!destroying_ && main_thread_dispatcher_) {
    main_thread_dispatcher_->post([this, scope]() -> void { clearScopeFromCaches(scope); });
  }
}

void ThreadLocalStoreImpl::clearScopeFromCaches(ScopeImpl* scope) {
  // Perform a cache flush on all threads.
  tls_->runOnAllThreads(
      [this, scope]() -> void { tls_->getTyped<TlsCache>(tls_slot_).scope_cache_.erase(scope); });
}

ThreadLocalStoreImpl::SafeAllocData ThreadLocalStoreImpl::safeAlloc(const std::string& name) {
  RawStatData* data = alloc_.alloc(name);
  if (!data) {
    // If we run out of stat space from the allocator (which can happen if for example allocations
    // are coming from a fixed shared memory region, we need to deal with this case the best we
    // can. We must pass back the right allocator so that free() happens on the heap.
    num_last_resort_stats_.inc();
    return {*heap_allocator_.alloc(name), heap_allocator_};
  } else {
    return {*data, alloc_};
  }
}

ThreadLocalStoreImpl::ScopeImpl::~ScopeImpl() { parent_.releaseScopeCrossThread(this); }

Counter& ThreadLocalStoreImpl::ScopeImpl::counter(const std::string& name) {
  // Determine the final name based on the prefix and the passed name.
  std::string final_name = prefix_ + name;

  // We now try to acquire a *reference* to the TLS cache shared pointer. This might remain null
  // if we don't have TLS initialized currently. The de-referenced pointer might be null if there
  // is no cache entry.
  CounterPtr* tls_ref = nullptr;
  if (parent_.tls_) {
    tls_ref = &parent_.tls_->getTyped<TlsCache>(parent_.tls_slot_)
                   .scope_cache_[this]
                   .counters_[final_name];
  }

  // If we have a valid cache entry, return it.
  if (tls_ref && *tls_ref) {
    return **tls_ref;
  }

  // We must now look in the central store so we must be locked. We grab a reference to the
  // central store location. It might contain nothing. In this case, we allocate a new stat.
  std::unique_lock<std::mutex> lock(parent_.lock_);
  CounterPtr& central_ref = central_cache_.counters_[final_name];
  if (!central_ref) {
    SafeAllocData alloc = parent_.safeAlloc(final_name);
    central_ref.reset(new CounterImpl(alloc.data_, alloc.free_));
  }

  // If we have a TLS location to store or allocation into, do it.
  if (tls_ref) {
    *tls_ref = central_ref;
  }

  // Finally we return the reference.
  return *central_ref;
}

void ThreadLocalStoreImpl::ScopeImpl::deliverHistogramToSinks(const std::string& name,
                                                              uint64_t value) {
  std::string final_name = prefix_ + name;
  for (Sink& sink : parent_.timer_sinks_) {
    sink.onHistogramComplete(final_name, value);
  }
}

void ThreadLocalStoreImpl::ScopeImpl::deliverTimingToSinks(const std::string& name,
                                                           std::chrono::milliseconds ms) {
  std::string final_name = prefix_ + name;
  for (Sink& sink : parent_.timer_sinks_) {
    sink.onTimespanComplete(final_name, ms);
  }
}

Gauge& ThreadLocalStoreImpl::ScopeImpl::gauge(const std::string& name) {
  // See comments in counter(). There is no super clean way (via templates or otherwise) to
  // share this code so I'm leaving it largely duplicated for now.
  std::string final_name = prefix_ + name;
  GaugePtr* tls_ref = nullptr;
  if (parent_.tls_) {
    tls_ref =
        &parent_.tls_->getTyped<TlsCache>(parent_.tls_slot_).scope_cache_[this].gauges_[final_name];
  }

  if (tls_ref && *tls_ref) {
    return **tls_ref;
  }

  std::unique_lock<std::mutex> lock(parent_.lock_);
  GaugePtr& central_ref = central_cache_.gauges_[final_name];
  if (!central_ref) {
    SafeAllocData alloc = parent_.safeAlloc(final_name);
    central_ref.reset(new GaugeImpl(alloc.data_, alloc.free_));
  }

  if (tls_ref) {
    *tls_ref = central_ref;
  }

  return *central_ref;
}

Timer& ThreadLocalStoreImpl::ScopeImpl::timer(const std::string& name) {
  // See comments in counter(). There is no super clean way (via templates or otherwise) to
  // share this code so I'm leaving it largely duplicated for now.
  std::string final_name = prefix_ + name;
  TimerPtr* tls_ref = nullptr;
  if (parent_.tls_) {
    tls_ref =
        &parent_.tls_->getTyped<TlsCache>(parent_.tls_slot_).scope_cache_[this].timers_[final_name];
  }

  if (tls_ref && *tls_ref) {
    return **tls_ref;
  }

  std::unique_lock<std::mutex> lock(parent_.lock_);
  TimerPtr& central_ref = central_cache_.timers_[final_name];
  if (!central_ref) {
    central_ref.reset(new TimerImpl(final_name, parent_));
  }

  if (tls_ref) {
    *tls_ref = central_ref;
  }

  return *central_ref;
}

} // Stats