#pragma once
#include <concepts>
#include <memory>
#include <functional>
#include <tbb/concurrent_hash_map.h>
#include <atomic>
#include <list>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace tnt::caching {
template<typename T>
concept has_size_method = requires(const T& t) {
    { t.size() } noexcept -> std::same_as<size_t>;
};

template<typename T>
size_t get_size(const T& value) requires has_size_method<T> {
    return value.size();
}

// Size-related concepts
template<typename T>
concept HasCapacity = requires(const T& value) {
    { value.capacity() } noexcept -> std::convertible_to<size_t>;
};

template<typename T>
concept HasSize = requires(const T& value) {
    { value.size() } noexcept -> std::convertible_to<size_t>;
};

template<typename T>
concept HasValueType = requires { typename T::value_type; };

// Size calculation functions
template<typename T>
    requires HasCapacity<T>
constexpr size_t get_size(const T& value) noexcept {
    return sizeof(T) + value.capacity() * sizeof(typename T::value_type);
}

template<typename T>
    requires (!HasCapacity<T>) && HasSize<T> && HasValueType<T>
constexpr size_t get_size(const T& value) noexcept {
    return sizeof(T) + value.size() * sizeof(typename T::value_type);
}

template<typename T>
    requires (!HasCapacity<T>) && HasSize<T> && (!HasValueType<T>)
constexpr size_t get_size(const T& value) noexcept {
    return sizeof(T) + value.size();
}

template<typename T>
    requires (!HasCapacity<T>) && (!HasSize<T>)
constexpr size_t get_size(const T& value) noexcept {
    return sizeof(T);
}

namespace detail {
    template<typename F>
    struct fetcher_traits;

    template<typename F, typename K, typename V>
    struct fetcher_traits<V(F::*)(const K&) const> {
        using key_type = K;
        using value_type = V;
    };

    template<typename F>
    struct fetcher_traits : fetcher_traits<decltype(&F::operator())> {};

    template<typename F>
    using fetcher_key_t = typename fetcher_traits<std::remove_cvref_t<F>>::key_type;

    template<typename F>
    using fetcher_value_t = typename fetcher_traits<std::remove_cvref_t<F>>::value_type;

    template<typename F, typename K>
    concept NoexceptFetcher = requires(F f, const K& k) {
        { f(k) } noexcept;
    };
}
template<typename Key, typename Value>
class LockFreeLRUCache {
private:
    struct Node {
        const Key key;
        std::unique_ptr<Value> value;
        const size_t memory_size;
        std::atomic<Node*> next{nullptr};
        std::atomic<Node*> prev{nullptr};
        std::atomic<bool> valid{true};
        std::atomic<uint32_t> ref_count{0};

        Node(Key k, std::unique_ptr<Value> v, size_t size)
            : key(std::move(k))
            , value(std::move(v))
            , memory_size(size) {}
    };

    class NodeGuard {
        Node* ptr_;
        void acquire(Node* p) {
            ptr_ = p;
            if (ptr_) {
                ptr_->ref_count.fetch_add(1, std::memory_order_acquire);
            }
        }
        void release() {
            if (ptr_ && ptr_->ref_count.fetch_sub(1, std::memory_order_release) == 1) {
                delete ptr_;
            }
        }
    public:
        NodeGuard(Node* p = nullptr) : ptr_(nullptr) { acquire(p); }
        ~NodeGuard() { release(); }
        NodeGuard(NodeGuard&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
        NodeGuard& operator=(NodeGuard&& other) noexcept {
            if (this != &other) {
                release();
                ptr_ = other.ptr_;
                other.ptr_ = nullptr;
            }
            return *this;
        }
        NodeGuard(const NodeGuard&) = delete;
        NodeGuard& operator=(const NodeGuard&) = delete;
        
        Node* operator->() const { return ptr_; }
        Node* get() const { return ptr_; }
        Node* release_ownership() { auto p = ptr_; ptr_ = nullptr; return p; }
    };

    struct HashCompare {
        static size_t hash(const Key& key) {
            return std::hash<Key>{}(key);
        }
        static bool equal(const Key& k1, const Key& k2) {
            return k1 == k2;
        }
    };

    std::atomic<Node*> head_{nullptr};
    std::atomic<Node*> tail_{nullptr};
    tbb::concurrent_hash_map<Key, Node*, HashCompare> index_;
    std::atomic<size_t> total_memory_{0};
    const size_t max_memory_;

public:
    explicit LockFreeLRUCache(size_t max_memory) : max_memory_(max_memory) {
        auto sentinel = new Node(Key{}, nullptr, 0);
        head_.store(sentinel, std::memory_order_release);
        tail_.store(sentinel, std::memory_order_release);
    }

    ~LockFreeLRUCache() {
        auto current = head_.load(std::memory_order_acquire);
        while (current) {
            auto next = current->next.load(std::memory_order_acquire);
            delete current;
            current = next;
        }
    }

    bool insert(Key key, std::unique_ptr<Value> value, size_t size) {
        if (size > max_memory_) return false;

        evict_if_needed(size);
        
        auto new_node = new Node(key, std::move(value), size);
        NodeGuard guard(new_node);

        typename decltype(index_)::accessor accessor;
        if (!index_.insert(accessor, key)) {
            return false;
        }

        accessor->second = new_node;
        
        while (true) {
            auto head = head_.load(std::memory_order_acquire);
            new_node->next.store(head, std::memory_order_release);
            
            if (head_.compare_exchange_weak(head, new_node,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
                if (head) {
                    head->prev.store(new_node, std::memory_order_release);
                }
                break;
            }
        }

        total_memory_.fetch_add(size, std::memory_order_release);
        guard.release_ownership();
        return true;
    }

    std::unique_ptr<Value> get(const Key& key) {
        typename decltype(index_)::accessor accessor;
        if (!index_.find(accessor, key)) {
            return nullptr;
        }

        NodeGuard guard(accessor->second);
        if (!guard->valid.load(std::memory_order_acquire)) {
            index_.erase(accessor);
            return nullptr;
        }

        // Move to front
        while (guard->prev.load(std::memory_order_acquire) != nullptr) {
            auto current = guard.get();
            auto prev = current->prev.load(std::memory_order_acquire);
            auto next = current->next.load(std::memory_order_acquire);

            if (!prev || !current->valid.load(std::memory_order_acquire)) break;

            auto prev_prev = prev->prev.load(std::memory_order_acquire);
            
            // Update links
            current->prev.store(prev_prev, std::memory_order_release);
            current->next.store(prev, std::memory_order_release);
            prev->prev.store(current, std::memory_order_release);
            prev->next.store(next, std::memory_order_release);
            
            if (prev_prev) {
                prev_prev->next.store(current, std::memory_order_release);
            } else {
                if (head_.compare_exchange_strong(prev, current,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
                    break;
                }
                continue;
            }

            if (next) {
                next->prev.store(prev, std::memory_order_release);
            }
            break;
        }

        return std::unique_ptr<Value>(guard->value.release());
    }

    void remove(const Key& key) {
        typename decltype(index_)::accessor accessor;
        if (!index_.find(accessor, key)) {
            return;
        }

        NodeGuard guard(accessor->second);
        if (!guard->valid.load(std::memory_order_acquire)) {
            index_.erase(accessor);
            return;
        }

        guard->valid.store(false, std::memory_order_release);
        
        auto current = guard.get();
        auto prev = current->prev.load(std::memory_order_acquire);
        auto next = current->next.load(std::memory_order_acquire);

        if (prev) prev->next.store(next, std::memory_order_release);
        if (next) next->prev.store(prev, std::memory_order_release);

        total_memory_.fetch_sub(current->memory_size, std::memory_order_release);
        index_.erase(accessor);
    }

    size_t memory_usage() const noexcept {
        return total_memory_.load(std::memory_order_relaxed);
    }

private:
    void evict_if_needed(size_t needed_space) {
        while (total_memory_.load(std::memory_order_acquire) + needed_space > max_memory_) {
            auto tail_node = tail_.load(std::memory_order_acquire);
            if (!tail_node) break;

            auto prev = tail_node->prev.load(std::memory_order_acquire);
            if (!prev) break;

            remove(prev->key);
        }
    }
};
template<typename F>
class CachingFactory {
    struct CacheEntry;
    struct Deleter;
    struct PendingFetch;
    struct HashCompare;

    using key_type = detail::fetcher_key_t<F>;
    using value_type = detail::fetcher_value_t<F>;
    using ValuePtr = std::shared_ptr<value_type>;
    using WeakValuePtr = std::weak_ptr<value_type>;
    using cache_map = std::unordered_map<key_type, CacheEntry>;
    using cache_iterator = typename cache_map::iterator;
    using ActiveMap = tbb::concurrent_hash_map<key_type, WeakValuePtr, HashCompare>;
    using PendingMap = tbb::concurrent_hash_map<key_type, std::shared_ptr<PendingFetch>, HashCompare>;


    struct CacheEntry {
        std::unique_ptr<value_type> value;
        typename std::list<key_type>::iterator lru_iter;
        size_t memory_size;
    };

    struct Deleter {
        Deleter(CachingFactory& f, key_type k) 
            : factory(f), key(std::move(k)) {}
        
        void operator()(value_type* ptr) { 
            // Deleter called before ptr deletion
            factory.release(key, std::unique_ptr<value_type>(ptr));
        }
        
        CachingFactory& factory;
        key_type key;
    };

    struct PendingFetch {
        std::mutex mutex;
        std::condition_variable cv;
        ValuePtr value;
        bool completed = false;
        bool transitioning = false;  // New flag
        std::exception_ptr error;
    };

    struct HashCompare {
        static size_t hash(const key_type& key) {
            return std::hash<key_type>{}(key);
        }
        static bool equal(const key_type& k1, const key_type& k2) {
            return k1 == k2;
        }
    };

    F fetcher_;
    LockFreeLRUCache<key_type, value_type> cache_;
    ActiveMap active_items_;
    PendingMap pending_fetches_;
    std::atomic<size_t> current_memory_{0};

public:
    explicit CachingFactory(F fetcher, size_t max_memory_bytes = 1024 * 1024 * 1024)
        : fetcher_(std::move(fetcher))
        , cache_(max_memory_bytes) {
            active_items_.rehash(1024);
    }

    ValuePtr get(const key_type& key) {
        if (auto value = try_get_active(key)) return value;
        if (auto value = try_get_cached(key)) return value;
        return fetch_or_wait(key);
    }
    [[nodiscard]] size_t get_current_memory_usage() const noexcept {
        return cache_.memory_usage();
    }
private:
    ValuePtr try_get_active(const key_type& key) {
        typename ActiveMap::accessor active_accessor;
        if (active_items_.find(active_accessor, key)) {
            if (auto strong = active_accessor->second.lock()) {
                return strong;
            }
            // Clean up stale entry
            active_items_.erase(active_accessor);
        }
        return nullptr;
    }

    ValuePtr try_get_cached(const key_type& key) {
        // Get active lock first
        typename ActiveMap::accessor active_accessor;
        if (!active_items_.insert(active_accessor, key)) {
            return nullptr;
        }

        // Then try cache lookup
        if (auto cached_value = cache_.get(key)) {
            auto managed_value = ValuePtr(
                cached_value.release(),
                Deleter(*this, key)
            );
            active_accessor->second = managed_value;
            return managed_value;
        }

        active_items_.erase(active_accessor);
        return nullptr;
    }

    ValuePtr fetch_or_wait(const key_type& key) {
        // Get pending lock first
        typename PendingMap::accessor pending_accessor;
        if (!pending_fetches_.insert(pending_accessor, key)) {
            auto pending = pending_accessor->second;
            pending_accessor.release();
            return wait_for_pending(pending);
        }

        // Create pending entry
        auto pending = std::make_shared<PendingFetch>();
        pending_accessor->second = pending;
        pending_accessor.release();

        try {
            // Get active lock before completing fetch
            value_type fetched_value = fetcher_(key);
            auto managed_value = ValuePtr(
                new value_type(std::move(fetched_value)),
                Deleter(*this, key)
            );

            typename ActiveMap::accessor active_accessor;
            if (!active_items_.insert(active_accessor, key)) {
                throw std::runtime_error("Failed to activate fetched value");
            }
            active_accessor->second = managed_value;

            // Complete pending
            {
                std::lock_guard fetch_lock(pending->mutex);
                pending->value = managed_value;
                pending->completed = true;
            }
            pending->cv.notify_all();
            pending_fetches_.erase(key);

            return managed_value;
        }
        catch (...) {
            pending_fetches_.erase(key);
            throw;
        }
    }

    ValuePtr wait_for_pending(std::shared_ptr<PendingFetch>& pending) {
        std::unique_lock fetch_lock(pending->mutex);
        while (!pending->completed) {
            pending->cv.wait(fetch_lock);
        }
        if (pending->error) {
            std::rethrow_exception(pending->error);
        }
        return pending->value;
    }

    // Base non-template function for non-noexcept fetchers
    ValuePtr fetch_noexcept_impl(const key_type& key, std::shared_ptr<PendingFetch>& pending) {
        value_type fetched_value = fetcher_(key);

        auto managed_value = ValuePtr(
            new value_type(std::move(fetched_value)),
            Deleter(*this, key)
        );

        typename ActiveMap::accessor active_accessor;
        active_items_.insert(active_accessor, key);
        active_accessor->second = managed_value;
        active_accessor.release();

        {
            std::lock_guard fetch_lock(pending->mutex);
            pending->value = managed_value;
            pending->completed = true;
        }
        pending->cv.notify_all();
        
        pending_fetches_.erase(key);
        return managed_value;
    }
    // Template specialization for noexcept fetchers
    template<detail::NoexceptFetcher<key_type> FF = F>
    ValuePtr fetch_noexcept(const key_type& key, std::shared_ptr<PendingFetch>& pending) {
        return fetch_noexcept_impl(key, pending);
    }

    template<typename FF = F>
    requires (!detail::NoexceptFetcher<FF, key_type>)
    ValuePtr fetch_throwing(const key_type& key, std::shared_ptr<PendingFetch>& pending) {
        try {
            // Call non-template base function instead
            return fetch_noexcept_impl(key, pending);  // Remove template argument
        }
        catch (...) {
            std::lock_guard fetch_lock(pending->mutex);
            pending->completed = true;
            pending->error = std::current_exception();
            pending->cv.notify_all();
            throw;
        }
    }

    void release(const key_type& key, std::unique_ptr<value_type> ptr) {
        // 1. Get active lock - must exist
        typename ActiveMap::accessor active_accessor;
        if (!active_items_.find(active_accessor, key)) {
            return;
        }

        // 2. Move to cache while still active
        if (!cache_.insert(key, std::move(ptr), get_size(*ptr))) {
            return;  // Cache insert failed
        }

        // 3. Remove from active after cache insert complete
        active_items_.erase(active_accessor);
    }
};

} // namespace tnt::caching