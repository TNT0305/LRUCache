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
        std::promise<std::shared_ptr<V>> promise;
        size_t size = 0;
        K key;
    };

    struct LRUEntry {
        K key;
        size_t size;
    };

    std::function<V(const K&)> fetcher_;
    size_t max_memory_;
    std::atomic<size_t> current_memory_{0};
    std::atomic<size_t> lru_memory_{0};
    std::mutex combined_mutex_; // Single mutex for all shared data
    std::mutex eviction_mutex_;
    std::atomic<bool> is_destroying_{false};
    tbb::concurrent_hash_map<K, std::shared_ptr<CacheEntry>> value_map_;
    std::list<LRUEntry> lru_;
    std::unordered_map<K, typename std::list<LRUEntry>::iterator> lru_map_;
    std::atomic<size_t> reactivation_count_{0};
    std::atomic<size_t> second_consumer_count_{0};
    std::atomic<size_t> eviction_count_{0};

    void evict_lru() {
        std::lock_guard<std::mutex> eviction_lock(eviction_mutex_);
        std::lock_guard<std::mutex> combined_lock(combined_mutex_);

        if (lru_.empty()) return;

        auto& back = lru_.back();
        current_memory_.fetch_sub(back.size, std::memory_order_relaxed);
        lru_memory_.fetch_sub(back.size, std::memory_order_relaxed);

        value_map_.erase(back.key);

        lru_map_.erase(back.key);
        lru_.pop_back();
    }

    void remove_from_lru(const K& key, size_t value_size) {
        std::lock_guard<std::mutex> combined_lock(combined_mutex_);
        if (lru_map_.contains(key)) {
            current_memory_.fetch_sub(value_size, std::memory_order_relaxed);
            lru_memory_.fetch_sub(value_size, std::memory_order_relaxed);
            lru_.erase(lru_map_.at(key));
            lru_map_.erase(key);
        }
    }

public:
    template<typename Fetcher>
        requires fetcher<Fetcher, K, V>
    explicit concurrent_value_cache(Fetcher fetcher, size_t max_memory)
        : fetcher_(std::move(fetcher)), max_memory_(max_memory) {}

    std::shared_ptr<V> get(const K& key) {
        {
            std::lock_guard<std::mutex> combined_lock(combined_mutex_);
            typename tbb::concurrent_hash_map<K, std::shared_ptr<CacheEntry>>::const_accessor a;
            if (value_map_.find(a, key)) {
                reactivation_count_.fetch_add(1, std::memory_order_relaxed);
                return a->second->value;
            }
        }

        std::unique_lock<std::mutex> combined_lock(combined_mutex_);

        typename tbb::concurrent_hash_map<K, std::shared_ptr<CacheEntry>>::accessor a;
        bool inserted = false;

        if (value_map_.find(a, key)) { // Check if already present
            combined_lock.unlock();
            second_consumer_count_.fetch_add(1, std::memory_order_relaxed);
            return a->second->value;
        }

        auto entry = std::make_shared<CacheEntry>();
        inserted = value_map_.insert(std::make_pair(key, entry));

        if (!inserted) {
            combined_lock.unlock();
            typename tbb::concurrent_hash_map<K, std::shared_ptr<CacheEntry>>::const_accessor a2;
            if (value_map_.find(a2, key)) {
                return a2->second->value;
            }
            return nullptr; // Should not happen, but return null just in case.
        }

        combined_lock.unlock();

        std::vector<K> evicted_keys;
        V* entry_ptr = nullptr;

        try {
            V fetched_value = fetcher_(key);
            size_t value_size = sizeof(V);

            entry_ptr = new V(fetched_value);

            { // Scoped lock for LRU update
                std::lock_guard<std::mutex> combined_lock_guard(combined_mutex_);

                entry->value = std::shared_ptr<V>(entry_ptr);
                entry->key = key;
                entry->size = value_size;

                lru_.push_front({key, value_size});
                lru_map_.insert_or_assign(key, lru_.begin());
                current_memory_.fetch_add(value_size, std::memory_order_relaxed);
                lru_memory_.fetch_add(value_size, std::memory_order_relaxed);
                while (current_memory_.load(std::memory_order_relaxed) > max_memory_) {
                    K evicted_key;
                    {
                        std::lock_guard<std::mutex> eviction_lock(eviction_mutex_);
                        if (lru_.empty()) break;
                        auto& back = lru_.back();
                        evicted_key = back.key;
                        current_memory_.fetch_sub(back.size, std::memory_order_relaxed);
                        lru_memory_.fetch_sub(back.size, std::memory_order_relaxed);
                        lru_map_.erase(back.key);
                        lru_.pop_back();
                    }
                    evicted_keys.push_back(evicted_key);
                    eviction_count_.fetch_add(1, std::memory_order_relaxed);
                }
                entry->promise.set_value(entry->value);
            }

        } catch (...) {
            delete entry_ptr;
            {
                std::unique_lock<std::mutex> combined_lock_catch(combined_mutex_, std::adopt_lock);
                typename tbb::concurrent_hash_map<K, std::shared_ptr<CacheEntry>>::accessor a;
                if (this->value_map_.find(a, key)) {
                    this->value_map_.erase(a);
                }
            }
            entry->promise.set_exception(std::current_exception());
            throw;
        }
        {
            std::lock_guard<std::mutex> combined_lock(combined_mutex_);
            for(const auto& k : evicted_keys)
            {
                value_map_.erase(k);
            }
        }

        return entry->value;
    }

    void clear() {
        std::lock_guard<std::mutex> combined_lock(combined_mutex_);
        value_map_.clear();
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
        is_destroying_.store(true, std::memory_order_release);
        clear();
    }
};

} // namespace tnt::caching::gemini2