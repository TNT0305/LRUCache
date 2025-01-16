#pragma once
#include <tbb/concurrent_hash_map.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <concepts>
#include <atomic>

namespace tnt::caching2 {
template<typename T>
concept has_size_method = requires(const T& t) {
    { t.size() } noexcept -> std::same_as<size_t>;
};

template<typename T>
size_t get_size(const T& value) requires has_size_method<T> {
    return value.size();
}

// Core concepts
template<typename T>
concept has_size = requires(T t) {
    { get_size(t) } -> std::convertible_to<size_t>;
};

template<typename F, typename K, typename V>
concept fetcher = requires(F f, const K& k) {
    { f(k) } -> std::same_as<V>;
};

template<typename P, typename K, typename V>
concept eviction_policy = requires(P p, const K& k, const V& v) {
    { p.should_evict(k, v) } -> std::convertible_to<bool>;
    { p.on_access(k) } -> std::same_as<void>;
    { p.on_insert(k, v) } -> std::same_as<void>;
    { p.on_evict(k) } -> std::same_as<void>;
};

template<typename K, typename V>
class lock_free_lru_policy {
private:
    struct Node {
        K key;
        size_t memory_size;
        std::atomic<Node*> next{nullptr};
        std::atomic<Node*> prev{nullptr};
        std::atomic<bool> valid{true};
        std::atomic<uint32_t> ref_count{0};

        Node(K k, size_t size) 
            : key(std::move(k))
            , memory_size(size) {}
    };

    struct NodeGuard {
        Node* ptr_;
        void acquire(Node* p) {
            ptr_ = p;
            if (ptr_) ptr_->ref_count.fetch_add(1, std::memory_order_acquire);
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
        Node* operator->() const { return ptr_; }
    };

    tbb::concurrent_hash_map<K, Node*> positions_;
    std::atomic<Node*> head_{nullptr};
    std::atomic<Node*> tail_{nullptr};
    std::atomic<size_t> current_memory_{0};
    const size_t max_memory_;

public:
    explicit lock_free_lru_policy(size_t max_memory) : max_memory_(max_memory) {}

    bool should_evict(const K& key, const V& value) {
        return current_memory_.load(std::memory_order_relaxed) + 
               get_size(value) > max_memory_;
    }

    void on_access(const K& key) {
        typename tbb::concurrent_hash_map<K, Node*>::accessor acc;
        if (positions_.find(acc, key)) {
            move_to_front(acc->second);
        }
    }

    void on_insert(const K& key, const V& value) {
        auto* node = new Node(key, get_size(value));
        typename tbb::concurrent_hash_map<K, Node*>::accessor acc;
        if (positions_.insert(acc, key)) {
            acc->second = node;
            push_front(node);
            current_memory_.fetch_add(node->memory_size, std::memory_order_release);
        }
    }

    void on_evict(const K& key) {
        typename tbb::concurrent_hash_map<K, Node*>::accessor acc;
        if (positions_.find(acc, key)) {
            auto size = acc->second->memory_size;
            remove_node(acc->second);
            positions_.erase(acc);
            current_memory_.fetch_sub(size, std::memory_order_release);
        }
    }

    [[nodiscard]] size_t current_memory() const noexcept {
        return current_memory_.load(std::memory_order_relaxed);
    }

private:
    void push_front(Node* node) {
        while (true) {
            auto old_head = head_.load(std::memory_order_acquire);
            node->next.store(old_head, std::memory_order_release);
            if (head_.compare_exchange_weak(old_head, node,
                std::memory_order_release,
                std::memory_order_relaxed)) {
                if (!old_head) {
                    tail_.store(node, std::memory_order_release);
                } else {
                    old_head->prev.store(node, std::memory_order_release);
                }
                break;
            }
        }
    }

    void move_to_front(Node* node) {
        auto prev = node->prev.load(std::memory_order_acquire);
        auto next = node->next.load(std::memory_order_acquire);
        
        if (!prev) return; // Already at front

        if (prev) prev->next.store(next, std::memory_order_release);
        if (next) next->prev.store(prev, std::memory_order_release);
        
        push_front(node);
    }

    void remove_node(Node* node) {
        auto prev = node->prev.load(std::memory_order_acquire);
        auto next = node->next.load(std::memory_order_acquire);

        if (prev) prev->next.store(next, std::memory_order_release);
        if (next) next->prev.store(prev, std::memory_order_release);

        if (head_.load(std::memory_order_relaxed) == node) {
            head_.store(next, std::memory_order_release);
        }
        if (tail_.load(std::memory_order_relaxed) == node) {
            tail_.store(prev, std::memory_order_release);
        }

        node->valid.store(false, std::memory_order_release);
    }
};

// Default LRU policy
template<typename K, typename V>
class lru_policy {
    mutable std::shared_mutex mutex_;
    std::list<K> lru_order_;
    std::unordered_map<K, typename std::list<K>::iterator> positions_;
    size_t current_memory_{0};
    const size_t max_memory_;

public:
    explicit lru_policy(size_t max_memory) : max_memory_(max_memory) {}

    bool should_evict(const K& key, const V& value) {
        std::shared_lock lock(mutex_);
        return current_memory_ + get_size(value) > max_memory_;
    }

    void on_access(const K& key) {
        std::unique_lock lock(mutex_);
        if (auto it = positions_.find(key); it != positions_.end()) {
            lru_order_.erase(it->second);
            auto new_pos = lru_order_.insert(lru_order_.begin(), key);
            it->second = new_pos;
        }
    }

    void on_insert(const K& key) {
        std::unique_lock lock(mutex_);
        auto it = lru_order_.insert(lru_order_.begin(), key);
        positions_[key] = it;
    }

    void on_evict(const K& key) {
        std::unique_lock lock(mutex_);
        if (auto it = positions_.find(key); it != positions_.end()) {
            lru_order_.erase(it->second);
            positions_.erase(it);
        }
    }
    [[nodiscard]] size_t current_memory() const noexcept {
        return current_memory_;
    }
};

template<typename K, typename V>
class count_lru_policy {
    std::mutex mutex_;
    std::list<K> lru_order_;
    std::unordered_map<K, typename std::list<K>::iterator> positions_;
    const size_t max_items_;

public:
    explicit count_lru_policy(size_t max_items) 
        : max_items_(max_items) {}

    bool should_evict(const K& key, const V& value) {
        std::lock_guard lock(mutex_);
        return lru_order_.size() >= max_items_;
    }

    void on_access(const K& key) {
        std::lock_guard lock(mutex_);
        if (auto it = positions_.find(key); it != positions_.end()) {
            lru_order_.erase(it->second);
            auto new_pos = lru_order_.insert(lru_order_.begin(), key);
            it->second = new_pos;
        }
    }

    void on_insert(const K& key) {
        std::lock_guard lock(mutex_);
        auto it = lru_order_.insert(lru_order_.begin(), key);
        positions_[key] = it;
    }

    void on_evict(const K& key) {
        std::lock_guard lock(mutex_);
        if (auto it = positions_.find(key); it != positions_.end()) {
            lru_order_.erase(it->second);
            positions_.erase(it);
        }
    }
};

namespace detail {
    // Primary template
    template<typename T>
    struct function_traits;

    // Function pointer
    template<typename R, typename Arg>
    struct function_traits<R(*)(Arg)> {
        using key_type = std::remove_cvref_t<Arg>;
        using value_type = R;
    };

    // Member function (const)
    template<typename C, typename R, typename Arg>
    struct function_traits<R(C::*)(Arg) const> {
        using key_type = std::remove_cvref_t<Arg>;
        using value_type = R;
    };

    // Member function (non-const)
    template<typename C, typename R, typename Arg>
    struct function_traits<R(C::*)(Arg)> {
        using key_type = std::remove_cvref_t<Arg>;
        using value_type = R;
    };

    // Callable object/lambda traits
    template<typename F>
    struct fetcher_traits {
    private:
        using call_type = decltype(&std::remove_cvref_t<F>::operator());
    public:
        using traits = function_traits<call_type>;
        using key_type = typename traits::key_type;
        using value_type = typename traits::value_type;
    };

    // Helper aliases
    template<typename F>
    using fetcher_key_t = typename fetcher_traits<std::remove_cvref_t<F>>::key_type;

    template<typename F>
    using fetcher_value_t = typename fetcher_traits<std::remove_cvref_t<F>>::value_type;
}

// Main cache container
template<typename K, 
         typename V, 
         typename F, 
         typename P = lock_free_lru_policy<K,V>>
    requires has_size<V> && 
             fetcher<F, K, V> && 
             eviction_policy<P, K, V>
class value_cache {
private:
    struct pending_fetch {
        std::mutex mutex;
        std::condition_variable cv;
        std::shared_ptr<V> value;
        bool completed = false;
        std::exception_ptr error;
    };

    struct hash_compare {
        static size_t hash(const K& key) {
            return std::hash<K>{}(key);
        }
        static bool equal(const K& k1, const K& k2) {
            return k1 == k2;
        }
    };

    using value_ptr = std::shared_ptr<V>;
    using weak_value_ptr = std::weak_ptr<V>;
    using pending_ptr = std::shared_ptr<pending_fetch>;
    using active_map = tbb::concurrent_hash_map<K, weak_value_ptr, hash_compare>;
    using cache_map = tbb::concurrent_hash_map<K, std::unique_ptr<V>, hash_compare>;
    using pending_map = tbb::concurrent_hash_map<K, pending_ptr, hash_compare>;

    F fetcher_;
    P policy_;
    active_map active_values_;
    cache_map cached_values_;
    pending_map pending_fetches_;

    struct value_deleter {
        value_cache& cache;
        K key;

        value_deleter(value_cache& c, K k) 
            : cache(c), key(std::move(k)) {}

        void operator()(V* ptr) {
            cache.release(key, std::unique_ptr<V>(ptr));
        }
    };

public:
    value_cache(F fetcher, size_t max_memory = 1024*1024*1024)
        : fetcher_(std::move(fetcher))
        , policy_(max_memory) {
        active_values_.rehash(1024);
        cached_values_.rehash(1024);
    }

    value_cache(F&& fetcher, P&& policy)
        : fetcher_(std::forward(fetcher))
        , policy_(std::forward(policy)) {
        active_values_.rehash(1024);
        cached_values_.rehash(1024);
    }

    value_ptr get(const K& key) {
        if (auto value = try_get_active(key)) return value;
        if (auto value = try_get_cached(key)) return value;
        return fetch_or_wait(key);
    }
    [[nodiscard]] size_t get_current_memory_usage() const noexcept {
        return policy_.current_memory();
    }
private:
    value_ptr try_get_active(const K& key) {
        typename active_map::accessor accessor;
        if (active_values_.find(accessor, key)) {
            if (auto value = accessor->second.lock()) {
                policy_.on_access(key);
                return value;
            }
            active_values_.erase(accessor);
        }
        return nullptr;
    }

    value_ptr try_get_cached(const K& key) {
        typename cache_map::accessor cache_accessor;
        if (cached_values_.find(cache_accessor, key)) {
            typename active_map::accessor active_accessor;
            if (active_values_.insert(active_accessor, key)) {
                auto value = std::shared_ptr<V>(
                    cache_accessor->second.release(),
                    value_deleter(*this, key)
                );
                active_accessor->second = value;
                cached_values_.erase(cache_accessor);
                policy_.on_access(key);
                return value;
            }
        }
        return nullptr;
    }

    value_ptr fetch_or_wait(const K& key) {
        typename pending_map::accessor pending_accessor;
        if (!pending_fetches_.insert(pending_accessor, key)) {
            auto pending = pending_accessor->second;
            pending_accessor.release();
            return wait_for_pending(pending);
        }

        auto pending = std::make_shared<pending_fetch>();
        pending_accessor->second = pending;
        pending_accessor.release();

        try {
            auto fetched = fetcher_(key);
            auto value = std::shared_ptr<V>(
                new V(std::move(fetched)),
                value_deleter(*this, key)
            );

            typename active_map::accessor active_accessor;
            active_values_.insert(active_accessor, key);
            active_accessor->second = value;

            {
                std::lock_guard lock(pending->mutex);
                pending->value = value;
                pending->completed = true;
            }
            pending->cv.notify_all();
            pending_fetches_.erase(key);

            return value;
        }
        catch (...) {
            pending_fetches_.erase(key);
            throw;
        }
    }

    value_ptr wait_for_pending(pending_ptr pending) {
        std::unique_lock lock(pending->mutex);
        pending->cv.wait(lock, [&] { return pending->completed; });
        if (pending->error) {
            std::rethrow_exception(pending->error);
        }
        return pending->value;
    }

    void release(const K& key, std::unique_ptr<V> ptr) {
        // 1. Get active lock
        typename active_map::accessor active_accessor;
        if (!active_values_.find(active_accessor, key)) {
            return;
        }

        // 2. Try to acquire cache lock before state change
        typename cache_map::accessor cache_accessor;
        bool cached = false;

        if (!policy_.should_evict(key, *ptr) && 
            cached_values_.insert(cache_accessor, key)) {
            // 3. Update policy first while holding both locks
            policy_.on_insert(key, *ptr);
            cache_accessor->second = std::move(ptr);
            cached = true;
        }

        // 4. Remove from active
        active_values_.erase(active_accessor);

        // 5. Handle eviction if caching failed
        if (!cached) {
            policy_.on_evict(key, *ptr);
        }
    }
};


// Deduction guides
template<typename F>
value_cache(F) -> value_cache<
    typename detail::fetcher_traits<std::remove_cvref_t<F>>::key_type,
    typename detail::fetcher_traits<std::remove_cvref_t<F>>::value_type,
    std::remove_cvref_t<F>
>;

template<typename F>
value_cache(F, size_t) -> value_cache<
    typename detail::fetcher_traits<std::remove_cvref_t<F>>::key_type,
    typename detail::fetcher_traits<std::remove_cvref_t<F>>::value_type,
    std::remove_cvref_t<F>
>;

template<typename F, typename P>
value_cache(F, P) -> value_cache<
    typename detail::fetcher_traits<std::remove_cvref_t<F>>::key_type,
    typename detail::fetcher_traits<std::remove_cvref_t<F>>::value_type,
    std::remove_cvref_t<F>,
    std::remove_cvref_t<P>
>;

} // namespace tnt::caching