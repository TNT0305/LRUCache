#pragma once

#include <iostream>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_queue.h>
#include <list>
#include <future>
#include "concurrentqueue.h" // Include the moodycamel concurrent queue

namespace tnt::caching::gemini2 {
    // Helper function to calculate the size of a value

    // Define the HasCapacity concept with noexcept requirement
    template <typename T>
    concept HasCapacity = requires(const T & value) {
        { value.capacity() } noexcept -> std::convertible_to<size_t>;
    };

    // Define the HasSize concept with noexcept requirement
    template <typename T>
    concept HasSize = requires(const T & value) {
        { value.size() } noexcept -> std::convertible_to<size_t>;
    };

    // Helper function to calculate the size of a value

    // Primary template: for types with capacity()
    template <typename T>
        requires HasCapacity<T>
    constexpr size_t get_size(const T& value) noexcept {
        return sizeof(T) + value.capacity() * sizeof(typename T::value_type);
    }

    // For std::shared_ptr<T>, when T has capacity()
    template <typename T>
        requires HasCapacity<T>
    constexpr size_t get_size(const std::shared_ptr<T>& value) noexcept {
        return get_size(*value);
    }

    // For std::shared_ptr<T>, when T has capacity()
    template <typename T>
        requires HasCapacity<T>
    constexpr size_t get_size(const std::unique_ptr<T>& value) noexcept {
        return get_size(*value);
    }

    // Next, for types that have size(), but not capacity()
    template <typename T>
        requires (!HasCapacity<T>) && HasSize<T>
    constexpr size_t get_size(const T& value) noexcept {
        return sizeof(T) + value.size() * sizeof(typename T::value_type);
    }

    // For std::shared_ptr<T>, when T has size(), but not capacity()
    template <typename T>
        requires (!HasCapacity<T>) && HasSize<T>
    constexpr size_t get_size(const std::shared_ptr<T>& value) noexcept {
        return get_size(*value);
    }

    // Finally, fallback for other types
    template <typename T>
        requires (!HasCapacity<T>) && (!HasSize<T>)
    constexpr size_t get_size(const T& /*value*/) noexcept {
        return sizeof(T);
    }

    // For std::shared_ptr<T>, fallback
    template <typename T>
        requires (!HasCapacity<T>) && (!HasSize<T>)
    constexpr size_t get_size(const std::shared_ptr<T>& value) noexcept {
        return get_size(*value);
    }

    // HasGetSize concept
    template <typename T>
    concept HasGetSize = requires(const T & value) {
        { get_size(value) } noexcept -> std::convertible_to<size_t>;
    };

    // Forward declaration
    template<typename K, typename V>
    class concurrent_value_cache;

    // Custom Deleter to move entries to inactive cache
    template<typename K, typename V>
    class CacheEntryDeleter {
    public:
        CacheEntryDeleter(concurrent_value_cache<K, V>* cache, const K& key)
            : cache_(cache), key_(key) {
        }

        void operator()(V* ptr);

    private:
        concurrent_value_cache<K, V>* cache_;
        K key_;
    };

    // The main cache class
    template<typename K, typename V>
    class concurrent_value_cache {
    private:
        using CacheMap = tbb::concurrent_hash_map<K, std::weak_ptr<V>>;
        using InactiveCacheMap = tbb::concurrent_hash_map<K, std::unique_ptr<V>>;
        using PendingFetchMap = tbb::concurrent_hash_map<K, std::shared_future<std::shared_ptr<V>>>;

        CacheMap active_cache_;
        InactiveCacheMap inactive_cache_;
        PendingFetchMap pending_fetches_;

        // LRU list to track usage
        moodycamel::ConcurrentQueue<K> lru_queue_;

        // Memory management for inactive items
        size_t max_memory_;
        std::atomic<size_t> current_memory_{ 0 };

        // Statistics
        std::atomic<size_t> reactivation_count_{ 0 };
        std::atomic<size_t> second_consumer_count_{ 0 };
        std::atomic<size_t> eviction_count_{ 0 };

        // Fetcher function to retrieve values
        std::function<V(const K&)> fetcher_;

    public:
        concurrent_value_cache(std::function<V(const K&)> fetcher, size_t max_memory)
            : fetcher_(std::move(fetcher)), max_memory_(max_memory) {
        }

        // Disable copy and move semantics
        concurrent_value_cache(const concurrent_value_cache&) = delete;
        concurrent_value_cache& operator=(const concurrent_value_cache&) = delete;

        // Get function to retrieve or fetch values
        std::shared_ptr<V> get(const K& key) {
            // Attempt to find in active cache
            {
                CacheMap::const_accessor accessor;
                if (active_cache_.find(accessor, key)) {
                    if (auto sp = accessor->second.lock()) {
                        second_consumer_count_.fetch_add(1);
                        return sp;
                    }
                }
            }

            // Attempt to find in inactive cache
            {
                auto sp = try_promote_to_active(key);
                if (sp) return sp;
            }

            std::shared_future<std::shared_ptr<V>> shared_fut;
            std::shared_ptr<std::promise<std::shared_ptr<V>>> fetch_promise;
            bool need_to_fetch = false;

            // Lock pending_mutex_ only while accessing pending_fetches_
            {
                PendingFetchMap::accessor accessor;
                if (pending_fetches_.find(accessor, key)) {
                    // Fetch is already in progress; use the existing shared_future
                    shared_fut = accessor->second;
                }
                else {
                    // No fetch in progress; initiate one
                    std::promise<std::shared_ptr<V>> promise;
                    shared_fut = promise.get_future().share();
                    pending_fetches_.insert(accessor, key);
                    accessor->second = shared_fut;

                    // Prepare to perform the fetch outside the mutex
                    fetch_promise = std::make_shared<std::promise<std::shared_ptr<V>>>(std::move(promise));
                    need_to_fetch = true;
                }
            }

            if (need_to_fetch) {
                // Perform the fetch outside the mutex
                std::shared_ptr<V> sp;
                try {
                    V fetched_value = fetcher_(key);
                    sp = std::shared_ptr<V>(new V(std::move(fetched_value)),
                        CacheEntryDeleter<K, V>(this, key));

                    // Insert into active cache without affecting current_memory_
                    {
                        CacheMap::accessor accessor;
                        active_cache_.insert(accessor, key);
                        accessor->second = sp;
                        // No change to current_memory_ since active items are not tracked
                    }

                    // Fulfill the promise with the fetched value
                    fetch_promise->set_value(sp);
                }
                catch (...) {
                    // Set the exception to notify all waiting threads
                    fetch_promise->set_exception(std::current_exception());
                }

                // Remove the key from pending_fetches_ after successful fetch
                {
                    PendingFetchMap::accessor accessor;
                    if (pending_fetches_.find(accessor, key)) {
                        pending_fetches_.erase(accessor);
                    }
                }

                // Return the fetched value directly
                return sp;
            }
            else {
                // Wait for the fetch to complete and return the result
                return shared_fut.get();
            }
        }

        // Statistics getters
        size_t get_reactivation_count() const { return reactivation_count_.load(); }
        size_t get_second_consumer_count() const { return second_consumer_count_.load(); }
        size_t get_eviction_count() const { return eviction_count_.load(); }

        // Get current cache size
        size_t get_lru_size() const {
            return current_memory_.load();
        }

    private:
        // Promote an entry from inactive to active cache
        std::shared_ptr<V> try_promote_to_active(const K& key) {
            // Remove from inactive cache and decrement current_memory_
            std::unique_ptr<V> up;
            {
                InactiveCacheMap::accessor accessor;
                if (inactive_cache_.find(accessor, key)) {
                    up = std::move(accessor->second);
                    inactive_cache_.erase(accessor);
                    size_t value_size = get_size(*up);
                    current_memory_.fetch_sub(value_size, std::memory_order_relaxed);
                    //std::cout << "Promoted to active: " << key << ", size: " << value_size << ", current_memory_: " << current_memory_.load() << std::endl;
                    reactivation_count_.fetch_add(1);
                }
            }

            std::shared_ptr<V> sp;
            if (!up) return sp;

            // Insert into active cache
            {
                CacheMap::accessor accessor;
                active_cache_.insert(accessor, key);
                sp = std::shared_ptr<V>(up.release(),
                    CacheEntryDeleter<K, V>(this, key));
                accessor->second = sp;
                // No change to current_memory_ since active items are not tracked
            }
            return sp;
        }
        std::mutex active_transition;
        // Move an entry to the inactive cache
        void move_to_inactive(const K& key, std::unique_ptr<V> sp) {
            size_t value_size = get_size(*sp);
            // Insert into inactive cache and increment current_memory_
            {
                InactiveCacheMap::accessor accessor;
                inactive_cache_.insert(accessor, key);
                accessor->second = std::move(sp);
                current_memory_.fetch_add(value_size, std::memory_order_relaxed);
                //std::cout << "Moved to inactive: " << key << ", size: " << value_size << ", current_memory_: " << current_memory_.load() << std::endl;
            }

            // Add to LRU queue
            lru_queue_.enqueue(key);

            std::lock_guard<std::mutex> lock(active_transition);
            // Remove from active cache
            {
                CacheMap::accessor accessor;
                if (active_cache_.find(accessor, key)) {
                    active_cache_.erase(accessor);
                }
            }
            enforce_memory_limit();
        }

        // Enforce memory limit by evicting least recently used items from inactive cache
        void enforce_memory_limit() {
            while (current_memory_.load(std::memory_order_relaxed) > max_memory_) {
                K least_used_key;
                if (!lru_queue_.try_dequeue(least_used_key)) {
                    break;
                }
                {
                    InactiveCacheMap::accessor accessor;
                    if (inactive_cache_.find(accessor, least_used_key)) {
                        size_t value_size = get_size(*accessor->second);
                        eviction_count_.fetch_add(1);
                        current_memory_.fetch_sub(value_size, std::memory_order_relaxed);
                        inactive_cache_.erase(accessor);
                        //std::cout << "Evicted: " << least_used_key << ", size: " << value_size << ", current_memory_: " << current_memory_.load() << std::endl;
                    }
                }
            }
        }

        // Calculate the size of the value
        friend class CacheEntryDeleter<K, V>;
    };

    // Definition of the custom deleter
    template<typename K, typename V>
    void CacheEntryDeleter<K, V>::operator()(V* ptr) {
        // Move the entry to inactive cache
        cache_->move_to_inactive(key_, std::unique_ptr<V>(ptr));
    }

} // namespace tnt::caching::gemini2


