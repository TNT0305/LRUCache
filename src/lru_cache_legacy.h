#pragma once

#include <iostream>
#include <tbb/concurrent_unordered_map.h>
#include <memory>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <unordered_set>

namespace tnt::legacy {
    // Define a concept to check if a callable is noexcept
    template <typename Callable, typename... Args>
    concept NoexceptInvocable = std::invocable<Callable, Args...> && noexcept(std::invoke(std::declval<Callable>(), std::declval<Args>()...));

    template <std::invocable Callable>
    class finally {
    public:
        explicit finally(Callable && callable) noexcept
            : callable_(std::move(callable)), active_(true) {
        }

        finally(finally && other) noexcept
            : callable_(std::move(other.callable_)), active_(other.active_) {
            other.active_ = false;
        }

        finally& operator=(finally && other) noexcept {
            if (this != &other) {
                if (active_) {
                    call();
                }
                callable_ = std::move(other.callable_);
                active_ = other.active_;
                other.active_ = false;
            }
            return *this;
        }

        ~finally() noexcept {
            if (active_) {
                call();
            }
        }

        // Delete copy constructor and copy assignment operator
        finally(const finally&) = delete;
        finally& operator=(const finally&) = delete;

    private:
        Callable callable_;
        bool active_;

        // Call the callable, with or without exception handling based on noexcept
        void call() noexcept {
            if constexpr (NoexceptInvocable<Callable>) {
                callable_();
            }
            else {
                try {
                    callable_();
                }
                catch (const std::exception& e) {
                    std::cerr << "Exception in finally destructor: " << e.what() << '\n';
                }
                catch (...) {
                    std::cerr << "Unknown exception in finally destructor\n";
                }
            }
        }
    };

    // Define the HasCapacity concept with noexcept requirement
    template <typename T>
    concept HasCapacity = requires (const T & value) {
        { value.capacity() } noexcept -> std::convertible_to<size_t>;
    };

    // Default getSize implementation for types with capacity()
    template <typename T>
        requires HasCapacity<T>
    size_t get_size(const T& value) noexcept {
        return sizeof(T) + value.capacity() * sizeof(typename T::value_type);
    }
    // Default getSize implementation for types with capacity()
    template <typename T>
        requires HasCapacity<T>
    size_t get_size(const std::shared_ptr<T>& value) noexcept {
        return sizeof(T) + value->capacity() * sizeof(typename T::value_type);
    }

    // Fallback getSize implementation for types without capacity()
    template <typename T>
    size_t get_size(const std::shared_ptr<T>& value) noexcept {
        return sizeof(*value);
    }

    // Define the getSize concept
    template <typename T>
    concept HasGetSize = requires (const T & value) {
        { get_size(value) } noexcept -> std::convertible_to<size_t>;
    };

    template<typename Key>
    bool isValidIterator(const typename std::list<Key>::iterator& it, const std::list<Key>& lst) {
        for (auto listIt = lst.begin(); listIt != lst.end(); ++listIt) {
            if (listIt == it) {
                return true;
            }
        }
        return false;
    }

    template <typename Func, typename Key, typename Value>
    concept RetrieveFunc = requires(Func func, const Key & key) {
        { func(key) } -> std::convertible_to<std::shared_ptr<Value>>;
    };
    template <typename Func, typename Key, typename Value>
    concept NoexceptRetrieveFunc = requires(Func func, const Key & key) {
        { func(key) } noexcept -> std::convertible_to<std::shared_ptr<Value>>;
    };

    template<typename Key, typename Value>
        requires HasGetSize<Value>&& RetrieveFunc<std::function<std::shared_ptr<Value>(const Key&)>, Key, Value>
    class lru_cache {
    public:
        using RetrieveFuncType = std::function<std::shared_ptr<Value>(const Key&)>;

        lru_cache(const size_t max_ram_usage_bytes, RetrieveFuncType retrieve_func, const size_t thrashing_window = 100, const size_t clean_up_threshold = 100)
            : max_ram_usage_bytes_(max_ram_usage_bytes), thrashing_window_(thrashing_window), clean_up_threshold_(clean_up_threshold), retrieve_func_(std::move(retrieve_func)) {
            thrashing_metrics_.resize(thrashing_window, false);
        }

        std::optional<std::shared_ptr<Value>> get(const Key& key) {
            std::shared_ptr<Value> value;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                // Check primary cache
                auto it = cache_.find(key);
                if (it != cache_.end()) {
                    // Move the accessed item to the front of the list
                    if (isValidIterator(it->second.second, items_)) {
                        items_.splice(items_.begin(), items_, it->second.second);
                    }
                    else {
                        // Reinsert item if iterator is invalid
                        items_.push_front(key);
                        it->second.second = items_.begin();
                    }
                    return it->second.first;
                }

                // Check secondary storage
                auto it_sec = secondary_storage_.find(key);
                if (it_sec != secondary_storage_.end()) {
                    if ((value = it_sec->second.lock())) {
                        // Move back to primary cache
                        put_internal(key, value, true);
                        return value;
                    }
                }

                // If a retrieval is already in progress for this key, wait
                while (retrieving_keys_.contains(key)) {
                    retrieve_cond_.wait(lock);
                }

                // Begin retrieving the value
                retrieving_keys_.insert(key);
                lock.unlock();

                // Scope guard to ensure cleanup (act as a "finally" block)
                finally cleanup([&] {
                    // need the lock held, here, which it will be by dtor time.
                    // dtors are called in reverse order of construction
                    retrieving_keys_.erase(key); // erase is noexcept.  Safe to call notify_all afterward
                    retrieve_cond_.notify_all();
                    });

                // Retrieve the value outside the mutex
                if constexpr (NoexceptRetrieveFunc<RetrieveFuncType, Key, Value>) {
                    value = retrieve_func_(key);
                }
                else {
                    try {
                        value = retrieve_func_(key);
                    }
                    catch (const std::exception& e) {
                        // Handle the exception, e.g., log an error and return a default value
                        std::cerr << "Error retrieving value: " << e.what() << '\n';
                        return std::nullopt;
                    }
                }

                // lock here and let the dtor unlock.
                lock.lock();
                if (value != nullptr) {
                    put_internal(key, value, false);
                    return value;
                }
                // Handle cache miss or error
                return std::nullopt;
            }
        }

        size_t get_thrashing_metrics() {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t evictions = 0;
            for (const auto evicted : thrashing_metrics_) {
                if (evicted) {
                    evictions++;
                }
            }
            return evictions;
        }

    private:
        size_t max_ram_usage_bytes_;
        size_t current_ram_usage_bytes_ = 0;
        size_t thrashing_window_;
        size_t clean_up_threshold_;
        size_t eviction_count_ = 0;
        size_t request_count_ = 0;
        std::vector<bool> thrashing_metrics_;
        std::list<Key> items_;
        tbb::concurrent_unordered_map<Key, std::pair<std::shared_ptr<Value>, typename std::list<Key>::iterator>> cache_;
        tbb::concurrent_unordered_map<Key, std::weak_ptr<Value>> secondary_storage_;
        std::unordered_set<Key> retrieving_keys_;
        std::mutex mutex_;
        std::condition_variable retrieve_cond_;
        RetrieveFuncType retrieve_func_;

        void put_internal(const Key key, std::shared_ptr<Value> value, const bool from_secondary) {
            auto it = cache_.find(key);
            bool eviction_occurred = false;

            if (it != cache_.end() && isValidIterator(it->second.second, items_)) {
                // Update the value and move to front
                items_.splice(items_.begin(), items_, it->second.second);
                current_ram_usage_bytes_ -= get_size(it->second.first);
                it->second.first = value;
                current_ram_usage_bytes_ += get_size(value);
            }
            else {
                // Handle new insertion or invalid iterator
                if (it == cache_.end()) {
                    current_ram_usage_bytes_ += get_size(value);
                }
                else {
                    current_ram_usage_bytes_ -= get_size(it->second.first);
                    current_ram_usage_bytes_ += get_size(value);
                }
                while (current_ram_usage_bytes_ > max_ram_usage_bytes_ && !items_.empty()) {
                    // Evict the least recently used item
                    const Key evicted_key = items_.back();
                    items_.pop_back();
                    auto evicted_item = cache_.find(evicted_key);

                    current_ram_usage_bytes_ -= get_size(evicted_item->second.first);

                    // Move to secondary storage
                    if (evicted_item->second.first.use_count() > 1) {
                        secondary_storage_[evicted_key] = evicted_item->second.first;
                    }
                    cache_.unsafe_erase(evicted_key);

                    eviction_count_++;
                    eviction_occurred = true;
                }

                if (eviction_count_ >= clean_up_threshold_) {
                    clean_up_secondary();
                    eviction_count_ = 0;
                }

                if (!from_secondary) {
                    items_.push_front(key);
                }
                else {
                    items_.insert(items_.begin(), key);
                }

                cache_[key] = { value, items_.begin() };
            }
            thrashing_metrics_[request_count_ % thrashing_window_] = eviction_occurred;
            request_count_++;
        }

        void clean_up_secondary() {
            for (auto it = secondary_storage_.begin(); it != secondary_storage_.end();) {
                if (it->second.expired()) {
                    it = secondary_storage_.unsafe_erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        class notify_guard {
        public:
            notify_guard(std::condition_variable& cv) : cv_(cv) {}
            ~notify_guard() {
                cv_.notify_all();
            }
        private:
            std::condition_variable& cv_;
        };
    };
} // namespace tnt::legacy