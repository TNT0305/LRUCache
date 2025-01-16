#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <optional>
#include <shared_mutex>

// Namespace declaration
namespace tnt::legacy2 {

    // C++20 Concepts for policy interfaces and type requirements
    // EvictionPolicy concept
    template <typename Policy, typename Key, typename Value>
    concept EvictionPolicy = requires(Policy policy, const Key & key, const std::shared_ptr<Value>&value) {
        { policy.onInsert(key, value) } -> std::same_as<void>;
        { policy.onTouch(key) } -> std::same_as<void>;
        { policy.evict() } -> std::convertible_to<std::optional<Key>>;
    };

    // SecondaryStoragePolicy concept
    template <typename Policy, typename Key, typename Value>
    concept SecondaryStoragePolicy = requires(Policy policy, const Policy & const_policy, const Key & key, const std::shared_ptr<Value>&value) {
        { policy.store(key, value) } -> std::same_as<void>;
        { const_policy.retrieve(key) } -> std::convertible_to<std::optional<std::shared_ptr<Value>>>;
        { policy.remove(key) } -> std::same_as<void>;
    };

    // RetrieveFunc concept
    template <typename Func, typename Key, typename Value>
    concept RetrieveFunc = requires(Func func, const Key & key) {
        { func(key) } -> std::convertible_to<std::shared_ptr<Value>>;
    };

    // NoexceptRetrieveFunc concept
    template <typename Func, typename Key, typename Value>
    concept NoexceptRetrieveFunc = requires(Func func, const Key & key) {
        { func(key) } noexcept -> std::convertible_to<std::shared_ptr<Value>>;
    };

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

    // Eviction Policies

    // MemoryThresholdEvictionPolicy
    template <typename Key, typename Value>
        requires HasGetSize<Value>
    class MemoryThresholdEvictionPolicy {
    public:
        using key_type = Key;
        using value_type = Value;

        explicit MemoryThresholdEvictionPolicy(size_t max_ram_usage_bytes)
            : max_ram_usage_bytes_(max_ram_usage_bytes), current_ram_usage_bytes_(0) {
        }

        void onInsert(const Key& key, const std::shared_ptr<Value>& value) {
            size_t size = get_size(value);
            current_ram_usage_bytes_ += size;
            key_list_.push_front(key);
            key_iterator_map_[key] = key_list_.begin();
            key_size_map_[key] = size;
        }

        void onTouch(const Key& key) {
            auto it = key_iterator_map_.find(key);
            if (it != key_iterator_map_.end()) {
                key_list_.splice(key_list_.begin(), key_list_, it->second);
            }
        }

        std::optional<Key> evict() {
            while (current_ram_usage_bytes_ > max_ram_usage_bytes_ && !key_list_.empty()) {
                Key evicted_key = key_list_.back();
                key_list_.pop_back();
                key_iterator_map_.erase(evicted_key);
                current_ram_usage_bytes_ -= key_size_map_.at(evicted_key);
                key_size_map_.erase(evicted_key);
                return evicted_key;
            }
            return std::nullopt;
        }

    private:
        size_t max_ram_usage_bytes_;
        size_t current_ram_usage_bytes_;
        std::list<Key> key_list_;
        std::unordered_map<Key, typename std::list<Key>::iterator> key_iterator_map_;
        std::unordered_map<Key, size_t> key_size_map_;
    };

    // ItemCountEvictionPolicy
    template <typename Key, typename Value>
    class ItemCountEvictionPolicy {
    public:
        using key_type = Key;
        using value_type = Value;

        explicit ItemCountEvictionPolicy(size_t max_item_count)
            : max_item_count_(max_item_count) {
        }

        void onInsert(const Key& key, const std::shared_ptr<Value>& /*value*/) {
            key_list_.push_front(key);
            key_iterator_map_[key] = key_list_.begin();
        }

        void onTouch(const Key& key) {
            auto it = key_iterator_map_.find(key);
            if (it != key_iterator_map_.end()) {
                key_list_.splice(key_list_.begin(), key_list_, it->second);
            }
        }

        std::optional<Key> evict() {
            while (key_list_.size() > max_item_count_ && !key_list_.empty()) {
                Key evicted_key = key_list_.back();
                key_list_.pop_back();
                key_iterator_map_.erase(evicted_key);
                return evicted_key;
            }
            return std::nullopt;
        }

    private:
        size_t max_item_count_;
        std::list<Key> key_list_;
        std::unordered_map<Key, typename std::list<Key>::iterator> key_iterator_map_;
    };

    // Secondary Storage Policies

    // InMemorySecondaryStorage
	// Assumes caller synchronizes access to the secondary storage
    template <typename Key, typename Value>
    class InMemorySecondaryStorage {
    public:
        using key_type = Key;
        using value_type = Value;

        void store(const Key& key, const std::shared_ptr<Value>& value) {
            storage_[key] = value;
        }

        std::optional<std::shared_ptr<Value>> retrieve(const Key& key) const {
            auto it = storage_.find(key);
            if (it != storage_.end()) {
                // Attempt to lock the weak_ptr to get a shared_ptr
                auto val = it->second.lock();

                //// Remove the entry from storage_ regardless of whether val is valid
                //storage_.erase(it);

                // If the object is still alive, return it
                if (val) {
                    return val;
                }
            }
            return std::nullopt;
        }

        void remove(const Key& key) {
            storage_.erase(key);
        }

    private:
        std::unordered_map<Key, std::weak_ptr<Value>> storage_;
    };

    // NullSecondaryStorage (No Secondary Storage)
    template <typename Key, typename Value>
    class NullSecondaryStorage {
    public:
        using key_type = Key;
        using value_type = Value;

        void store(const Key&, const std::shared_ptr<Value>&) noexcept {
            // Do nothing
        }

        std::optional<std::shared_ptr<Value>> retrieve(const Key&) const noexcept {
            return std::nullopt;
        }

        void remove(const Key&) noexcept {
            // Do nothing
        }
    };

    // lru_cache Class
    template<
        typename Key,
        typename Value,
        typename EvictPolicy,
        typename SecondaryPolicy,
        typename RetrieveFuncType>
        requires EvictionPolicy<EvictPolicy, Key, Value>&&
            SecondaryStoragePolicy<SecondaryPolicy, Key, Value>&&
            RetrieveFunc<RetrieveFuncType, Key, Value>
        class lru_cache {
        public:
            lru_cache(const lru_cache&) = delete;
            lru_cache& operator=(const lru_cache&) = delete;

            lru_cache(lru_cache&&) = delete;
            lru_cache& operator=(lru_cache&&) = delete;

            lru_cache(EvictPolicy eviction_policy,
                SecondaryPolicy secondary_storage,
                RetrieveFuncType retrieve_func)
                : eviction_policy_(std::move(eviction_policy)),
                secondary_storage_(std::move(secondary_storage)),
                retrieve_func_(std::move(retrieve_func)) {
            }

            ~lru_cache() {
                // Indicate that the cache is shutting down
                shutting_down_.store(true);

                // Notify all threads waiting on the condition variable
                {
                    std::unique_lock<std::mutex> retrieve_lock(retrieve_mutex_);
                    retrieve_cond_.notify_all();
                }
            }

            std::optional<std::shared_ptr<Value>> get(const Key& key) {
                // Acquire shared lock for cache access
                {
                    std::shared_lock<std::shared_mutex> shared_lock(cache_mutex_);

                    // Check primary cache
                    auto it = cache_.find(key);
                    if (it != cache_.end()) {
                        eviction_policy_.onTouch(key);
                        return it->second;
                    }

                    // Check secondary storage
                    {
                        std::lock_guard<std::mutex> secondary_lock(secondary_storage_mutex_);
                        auto sVal = secondary_storage_.retrieve(key);
                        if (sVal) {
                            // Remove from secondary storage
                            secondary_storage_.remove(key);

                            // Upgrade to exclusive lock (must release shared lock first)
                            shared_lock.unlock();
                            std::unique_lock<std::shared_mutex> unique_lock(cache_mutex_);

                            // Double-check cache after acquiring exclusive lock
                            it = cache_.find(key);
                            if (it == cache_.end()) {
                                // Insert into cache
                                cache_[key] = *sVal;
                                eviction_policy_.onInsert(key, *sVal);
                            }
                            else {
                                *sVal = it->second;
                            }

                            return *sVal;
                        }
                    } // Release secondary_storage_mutex_ lock

                    // Release shared lock before proceeding
                    shared_lock.unlock();
                } // Ensure no locks are held before calling retrieve_and_insert()

                // Proceed to retrieve the value
                return retrieve_and_insert(key);
            }

        private:
            std::optional<std::shared_ptr<Value>> retrieve_and_insert(const Key& key) {
                // Synchronize retrieval
                {
                    std::unique_lock<std::mutex> retrieve_lock(retrieve_mutex_);

                    // Wait if another thread is retrieving the same key
                    retrieve_cond_.wait(retrieve_lock, [&]() {
                        return !retrieving_keys_.contains(key) || shutting_down_.load();
                        });

                    if (shutting_down_.load()) {
                        return std::nullopt;
                    }

                    // Indicate that this thread is retrieving the key
                    retrieving_keys_.insert(key);
                } // Release retrieve_lock

                // Retrieve the value outside of locks
                std::shared_ptr<Value> new_value;
                try {
                    new_value = retrieve_func_(key);
                }
                catch (...) {
                    // Handle exceptions and cleanup
                    {
                        std::unique_lock<std::mutex> retrieve_lock(retrieve_mutex_);
                        retrieving_keys_.erase(key);
                        retrieve_cond_.notify_all();
                    }
                    return std::nullopt;
                }

                // Insert into cache
                {
                    std::unique_lock<std::shared_mutex> unique_lock(cache_mutex_);

                    // Double-check the cache
                    auto it = cache_.find(key);
                    if (it == cache_.end()) {
                        cache_[key] = new_value;
                        eviction_policy_.onInsert(key, new_value);
                    }
                    else {
                        // Another thread inserted the item
                        new_value = it->second;
                    }
                }

                // Remove from retrieving_keys_ and notify others
                {
                    std::unique_lock<std::mutex> retrieve_lock(retrieve_mutex_);
                    retrieving_keys_.erase(key);
                    retrieve_cond_.notify_all();
                }

                return new_value;
            }

            void put_internal(const Key& key, const std::shared_ptr<Value>& value) {
                // This function should be called with unique_lock on cache_mutex_

                cache_[key] = value;
                eviction_policy_.onInsert(key, value);

                // Evict items if necessary
                while (true) {
                    std::optional<Key> evicted_key = eviction_policy_.evict();
                    if (!evicted_key) break;

                    auto it = cache_.find(*evicted_key);
                    if (it != cache_.end()) {
                        if (it->second.use_count() > 1) {
                            // Store in secondary storage
                            {
                                std::lock_guard<std::mutex> secondary_lock(secondary_storage_mutex_);
                                secondary_storage_.store(*evicted_key, it->second);
                            }
                        }
                        cache_.erase(it);
                    }
                }
            }

            // Member variables
            EvictPolicy eviction_policy_;
            SecondaryPolicy secondary_storage_;
            RetrieveFuncType retrieve_func_;

            std::unordered_map<Key, std::shared_ptr<Value>> cache_;
            mutable std::shared_mutex cache_mutex_; // Protects cache_ and eviction_policy_
            std::mutex secondary_storage_mutex_;    // Protects secondary_storage_
            std::unordered_set<Key> retrieving_keys_;
            std::mutex retrieve_mutex_;             // For retrieving_keys_ and retrieve_cond_
            std::condition_variable retrieve_cond_;
            std::atomic<bool> shutting_down_{ false };
    };

    // Deduction guide for lru_cache
    template<
        typename EvictPolicy,
        typename SecondaryPolicy,
        typename RetrieveFuncType>
    lru_cache(EvictPolicy,
        SecondaryPolicy,
        RetrieveFuncType)
        -> lru_cache<typename EvictPolicy::key_type,
        typename EvictPolicy::value_type,
        EvictPolicy,
        SecondaryPolicy,
        RetrieveFuncType>;

} // namespace tnt
