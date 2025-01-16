#include <tbb/concurrent_unordered_map.h>
#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/locks.hpp>
#include <memory>
#include <functional>
#include <atomic>
#include <future>
#include <list>
#include <mutex>
#include <optional>

namespace tnt {
    // C++20 Concepts for policy interfaces and type requirements
    // EvictionPolicy concept
    template <typename Policy, typename Key, typename Value>
    concept EvictionPolicy = requires(Policy policy, const Key & key, const std::shared_ptr<Value>& value, const std::atomic<size_t>&current_ro, std::atomic<size_t>&current) {
        { policy.add(current, key, value) } -> std::same_as<void>;
        { policy.evict_required(current_ro) } -> std::convertible_to<bool>;
        { policy.evict(current, key, value) } -> std::same_as<void>;
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
    // MemoryThresholdEvictionPolicy
    template <typename Key, typename Value>
        requires HasGetSize<Value>
    class MemoryThresholdEvictionPolicy {
    public:
        using key_type = Key;
        using value_type = Value;

        explicit MemoryThresholdEvictionPolicy(size_t max_ram_usage_bytes)
            : max_ram_usage_bytes_(max_ram_usage_bytes) {
        }

    	void add(std::atomic<size_t>& current, const Key& key, const std::shared_ptr<Value>& value) {
            current.fetch_add(get_size(value), std::memory_order_relaxed);
        }

		bool evict_required(const std::atomic<size_t>& current) const {
			return current.load() > max_ram_usage_bytes_;
		}
        void evict(std::atomic<size_t>& current, const Key& key, const std::shared_ptr<Value>& value) {
            current.fetch_sub(get_size(value), std::memory_order_relaxed);
        }

    private:
        size_t max_ram_usage_bytes_;
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

        void add(std::atomic<size_t>& current, const Key& key, const std::shared_ptr<Value>& value) {
            current.fetch_add(1, std::memory_order_relaxed);
        }

        bool evict_required(const std::atomic<size_t>& current) const {
            return current.load() > max_item_count_;
        }
        void evict(std::atomic<size_t>& current, const Key& key, const std::shared_ptr<Value>& value) {
            current.fetch_add(1, std::memory_order_relaxed);
        }

    private:
        size_t max_item_count_;
    };

    template <
        typename Key,
        typename Value,
        typename RetrieveFuncType,
        typename EvictPolicy
    >
	requires RetrieveFunc<RetrieveFuncType, Key, Value>&&
		EvictionPolicy<EvictPolicy, Key, Value>
        

    class ConcurrentCache {
    public:
        using value_type = std::shared_ptr<Value>;

        ConcurrentCache(RetrieveFuncType generator, EvictPolicy eviction_policy)
            : generator_(std::move(generator)),
            eviction_policy_(std::move(eviction_policy)) {
        }

        // Retrieves or generates the value associated with the key
        value_type get(const Key& key) {
            // First, try to find the key in the cache
            auto it = cache_.find(key);
            if (it != cache_.end()) {
	            if (auto value_ptr = it->second.value.lock()) {
                    update_lru(key);
                    return value_ptr;
                }
                // Value expired; proceed to regenerate
            }

            std::shared_ptr<std::promise<value_type>> promise = std::make_shared<std::promise<value_type>>();
            std::shared_future<value_type> shared_fut = promise->get_future().share();
            CacheEntry entry;
            entry.promise = promise;

            // Try to insert the new entry
            auto result = cache_.insert({ key, entry });
            bool inserted = result.second;

            if (!inserted) {
                // Another thread is generating the value
                if (auto p = result.first->second.promise) {
                    // Wait for the other thread to finish
                    return p->get_future().share().get();
                }
                if (auto rv = result.first->second.value.lock()) {
					// Another thread has already generated the value
					return rv;
				}
            }

            // We are responsible for generating the value
            value_type value_ptr;
            try {
                // Generate the value outside of any locks
                value_ptr = generator_(key);
            }
            catch (...) {
                // Remove the placeholder and rethrow
                cache_.unsafe_erase(key);
                promise->set_exception(std::current_exception());
                throw;
            }

            // Update the cache entry
            entry.value = value_ptr;
            entry.promise = nullptr; // Generation complete
            cache_[key] = entry;

            // Update LRU and memory usage
            update_lru(key);
            eviction_policy_.add(current_size, key, value_ptr);

            // Fulfill the promise
            promise->set_value(value_ptr);

            // Evict if necessary
            evict_if_needed();

            return shared_fut.get();
        }

    private:
        // Cache entry containing the value or a promise for synchronization
        struct CacheEntry {
            std::weak_ptr<Value> value;
            std::shared_ptr<std::promise<value_type>> promise;
        };

        tbb::concurrent_unordered_map<Key, CacheEntry> cache_;
        RetrieveFuncType generator_;
        EvictPolicy eviction_policy_;
        std::atomic<size_t> current_size;

        // LRU tracking
        std::list<Key> lru_list_;
        boost::shared_mutex lru_mutex_; // Upgradeable lock
        std::unordered_map<Key, typename std::list<Key>::iterator> lru_positions_;

        // Eviction handling
        void update_lru(const Key& key) {
            boost::upgrade_lock<boost::shared_mutex> lock(lru_mutex_);
            auto it = lru_positions_.find(key);
            if (it != lru_positions_.end()) {
                // Move the key to the front
                boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);
                lru_list_.erase(it->second);
                lru_list_.push_front(key);
                lru_positions_[key] = lru_list_.begin();
            }
            else {
                // Insert the key
                boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(lock);
                lru_list_.push_front(key);
                lru_positions_[key] = lru_list_.begin();
            }
        }
        void evict_if_needed() {
            boost::unique_lock<boost::shared_mutex> lock(lru_mutex_);
            while (eviction_policy_.evict_required(current_size)) {
                if (lru_list_.empty()) {
                    break;
                }

                // Get the least recently used key
                Key key_to_evict = lru_list_.back();
                lru_list_.pop_back();
                lru_positions_.erase(key_to_evict);

                // Remove from cache if not in use externally
                auto it = cache_.find(key_to_evict);
                if (it != cache_.end()) {
                    auto value_ptr = it->second.value.lock();
                    if (value_ptr && value_ptr.use_count() == 1) {
                        // Not in use externally
						eviction_policy_.evict(current_size, key_to_evict, value_ptr);
                        cache_.unsafe_erase(it);
                    }
                    else {
                        // Still in use externally; skip eviction
                        break;
                    }
                }
            }
        }
    };
    template <
        typename RetrieveFuncType,
        typename EvictPolicy
    >
    ConcurrentCache(RetrieveFuncType, EvictPolicy)
        -> ConcurrentCache<
        typename EvictPolicy::key_type,
        typename EvictPolicy::value_type,
		RetrieveFuncType,
		EvictPolicy
	>;
} // namespace tnt