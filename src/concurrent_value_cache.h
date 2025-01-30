#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <concepts>
#include <iostream>
#include <tbb/concurrent_hash_map.h>
#include <vector>

namespace tnt::caching::gemini2 {

template<typename F, typename K, typename V>
concept fetcher = requires(F f, const K& k) {
    { f(k) } -> std::same_as<V>;
};

template<typename K, typename V>
class concurrent_value_cache {
private:
    struct CacheEntry {
        std::shared_ptr<V> value;
        std::shared_ptr<std::promise<std::shared_ptr<V>>> promise;
        std::atomic<bool> is_fetching{false};
        size_t size = 0;
        K key;
        mutable std::mutex mutex;
        std::shared_future<std::shared_ptr<V>> shared_future; // Correctly added here
    };
    struct LRUEntry {
        K key;
        size_t size;
    };

    using lru_iterator = typename std::list<LRUEntry>::iterator;
    using cache_type = tbb::concurrent_hash_map<K, CacheEntry>;

    std::function<V(const K&)> fetcher_;
    size_t max_memory_;
    std::atomic<size_t> current_memory_{0};
    std::atomic<size_t> lru_memory_{0};
    std::atomic<size_t> reactivation_count_{0};
    std::atomic<size_t> second_consumer_count_{0};
    std::atomic<size_t> eviction_count_{0};

    cache_type cache_;
    std::list<LRUEntry> lru_;
    std::unordered_map<K, lru_iterator> lru_map_;
    mutable std::mutex combined_mutex_;

    void evict_lru() {
        std::lock_guard<std::mutex> lock(combined_mutex_);
        if (lru_.empty()) return;

        auto last = lru_.back();
        typename cache_type::accessor accessor;
        if (cache_.find(accessor, last.key)) {
            current_memory_.fetch_sub(last.size, std::memory_order_relaxed);
            lru_memory_.fetch_sub(last.size, std::memory_order_relaxed);
            cache_.erase(accessor);
            eviction_count_.fetch_add(1, std::memory_order_relaxed);
        }
        lru_map_.erase(last.key);
        lru_.pop_back();
    }

public:
    concurrent_value_cache(std::function<V(const K&)> fetcher, size_t max_memory)
        : fetcher_(std::move(fetcher))
        , max_memory_(max_memory) {}

    // std::shared_ptr<V> get(const K& key) {
    //     typename cache_type::accessor accessor;
    //     cache_.insert(accessor, key);
    //     auto& entry = accessor->second;

    //     if (entry.value) {
    //         accessor.release();
    //         return entry.value;
    //     }

    //     bool expected = false;
    //     if (!entry.is_fetching.compare_exchange_strong(expected, true)) {
    //         second_consumer_count_.fetch_add(1, std::memory_order_relaxed);
    //         auto future = entry.promise->get_future();
    //         accessor.release();
    //         return future.get();
    //     }

    //     entry.promise = std::make_shared<std::promise<std::shared_ptr<V>>>();
    //     accessor.release();

    //     V* entry_ptr = nullptr;
    //     try {
    //         V fetched_value = fetcher_(key);
    //         size_t value_size = sizeof(V);
    //         entry_ptr = new V(fetched_value);

    //         {
    //             std::lock_guard<std::mutex> lock(combined_mutex_);
    //             typename cache_type::accessor write_accessor;
    //             cache_.find(write_accessor, key);
    //             auto& entry = write_accessor->second;

    //             entry.value = std::shared_ptr<V>(entry_ptr);
    //             entry.key = key;
    //             entry.size = value_size;

    //             lru_.push_front({key, value_size});
    //             lru_map_.insert_or_assign(key, lru_.begin());
    //             current_memory_.fetch_add(value_size, std::memory_order_relaxed);
    //             lru_memory_.fetch_add(value_size, std::memory_order_relaxed);

    //             while (current_memory_.load(std::memory_order_relaxed) > max_memory_) {
    //                 evict_lru();
    //             }

    //             entry.promise->set_value(entry.value);
    //             entry.is_fetching = false;
    //             return entry.value;
    //         }
    //     } catch (...) {
    //         delete entry_ptr;
    //         typename cache_type::accessor write_accessor;
    //         cache_.find(write_accessor, key);
    //         auto& entry = write_accessor->second;
    //         entry.promise->set_exception(std::current_exception());
    //         entry.is_fetching = false;
    //         throw;
    //     }
    // } 
    std::shared_ptr<V> get(const K& key) {
        typename cache_type::accessor accessor;

        if (cache_.find(accessor, key)) {
            auto& entry = accessor->second;
            if (entry.value) {
                accessor.release();
                return entry.value;
            }
        } else {
            cache_.insert(accessor, key);
        }

        auto& entry = accessor->second;
        std::shared_ptr<std::promise<std::shared_ptr<V>>> local_promise;
        std::shared_future<std::shared_ptr<V>> shared_future;

        {
            std::unique_lock<std::mutex> lock(entry.mutex);
            if (entry.value) {
                accessor.release();
                return entry.value;
            }

            if (!entry.is_fetching) {
                entry.is_fetching = true;
                local_promise = entry.promise = std::make_shared<std::promise<std::shared_ptr<V>>>();
                entry.shared_future = local_promise->get_future().share();
            } else {
                local_promise = entry.promise;
                shared_future = entry.shared_future;
            }
        } // Release entry.mutex lock

        accessor.release();

        if (!local_promise) { // Another thread is fetching
            return shared_future.get();
        }

        std::shared_ptr<V> value_ptr;
        try {
            value_ptr = fetch_and_insert(key); // Fetch and insert

            { // Lock combined_mutex_ for setting the promise value
                std::lock_guard<std::mutex> lock(combined_mutex_);
                local_promise->set_value(value_ptr); // Set the value *inside* combined_mutex_
            }
            entry.is_fetching = false;

        } catch (...) {
            try {
                local_promise->set_exception(std::current_exception()); // Set exception *inside* combined_mutex_
            } catch (const std::future_error& e) {
                if (e.code() != std::future_errc::promise_already_satisfied) {
                    std::cerr << "Unexpected future_error setting exception: " << e.what() << std::endl;
                }
            }
            entry.is_fetching = false;
            throw;
        }
        return shared_future.get();
    }

    // New helper function to perform the fetch and insertion
    std::shared_ptr<V> fetch_and_insert(const K& key) {
        V fetched_value = fetcher_(key); // Fetch value
        size_t value_size = sizeof(V);
        std::shared_ptr<V> value_ptr = std::make_shared<V>(fetched_value); // Create shared_ptr

        {
            std::lock_guard<std::mutex> lock(combined_mutex_); // Lock for LRU and memory
            typename cache_type::accessor write_accessor;
            cache_.find(write_accessor, key);
            auto& entry = write_accessor->second;

            entry.value = value_ptr;
            entry.key = key;
            entry.size = value_size;

            lru_.push_front({key, value_size});
            lru_map_.insert_or_assign(key, lru_.begin());
            current_memory_.fetch_add(value_size, std::memory_order_relaxed);
            lru_memory_.fetch_add(value_size, std::memory_order_relaxed);

            while (current_memory_.load(std::memory_order_relaxed) > max_memory_) {
                evict_lru();
            }
        } // Release combined_mutex_ lock
        return value_ptr;
    }    
    void clear() {
        std::lock_guard<std::mutex> lock(combined_mutex_);
        cache_.clear();
        current_memory_.store(0, std::memory_order_relaxed);
        lru_memory_.store(0, std::memory_order_relaxed);
        lru_.clear();
        lru_map_.clear();
    }

    size_t get_lru_size() const {
        return lru_memory_.load(std::memory_order_relaxed);
    }

    size_t get_reactivation_count() const { return reactivation_count_.load(std::memory_order_relaxed); }
    size_t get_second_consumer_count() const { return second_consumer_count_.load(std::memory_order_relaxed); }
    size_t get_eviction_count() const { return eviction_count_.load(std::memory_order_relaxed); }

    ~concurrent_value_cache() {
        clear();
    }
};

} // namespace tnt::caching::gemini2