#pragma once

#include <iostream>
#include <tbb/concurrent_unordered_map.h>
#include <memory>
#include <mutex>
#include <functional>
#include <condition_variable>
#include <unordered_set>

// Define the HasCapacity concept
template <typename T>
concept HasCapacity = requires (const T & value) {
    { value.capacity() } -> std::convertible_to<size_t>;
};

// Default getSize implementation for types with capacity()
template <typename T>
    requires HasCapacity<T>
size_t getSize(const T& value) {
    return sizeof(T) + value.capacity() * sizeof(typename T::value_type);
}
// Default getSize implementation for types with capacity()
template <typename T>
    requires HasCapacity<T>
size_t getSize(const std::shared_ptr<T>& value) {
    return sizeof(T) + value->capacity() * sizeof(typename T::value_type);
}

// Fallback getSize implementation for types without capacity()
template <typename T>
size_t getSize(const std::shared_ptr<T>& value) {
    return sizeof(*value);
}

// Define the getSize concept
template <typename T>
concept HasGetSize = requires (const T & value) {
    { getSize(value) } -> std::convertible_to<size_t>;
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

template<typename Key, typename Value>
    requires HasGetSize<Value>
class lru_cache {
public:
    using RetrieveFunc = std::function<std::shared_ptr<Value>(const Key&)>;

    lru_cache(size_t maxRamUsageBytes, RetrieveFunc retrieveFunc, size_t thrashingWindow = 100, size_t cleanUpThreshold = 100)
        : maxRamUsageBytes(maxRamUsageBytes), thrashingWindow(thrashingWindow), cleanUpThreshold(cleanUpThreshold), retrieveFunc(retrieveFunc) {
        thrashingMetrics.resize(thrashingWindow, false);
    }

    std::shared_ptr<Value> get(const Key& key) {
        std::shared_ptr<Value> value;

        {
            std::unique_lock<std::mutex> lock(mutex);

            // Check primary cache
            auto it = cache.find(key);
            if (it != cache.end()) {
                // Move the accessed item to the front of the list
                if (isValidIterator(it->second.second, items)) {
                    items.splice(items.begin(), items, it->second.second);
                }
                else {
                    // Reinsert item if iterator is invalid
                    items.push_front(key);
                    it->second.second = items.begin();
                }
                return it->second.first;
            }

            // Check secondary storage
            auto it_sec = secondary_storage.find(key);
            if (it_sec != secondary_storage.end()) {
                if ((value = it_sec->second.lock())) {
                    // Move back to primary cache
                    putInternal(key, value, true);
                    return value;
                }
            }

            // If a retrieval is already in progress for this key, wait
            while (retrieving_keys.count(key) > 0) {
                retrieve_cond.wait(lock);
            }

            // Begin retrieving the value
            retrieving_keys.insert(key);
            lock.unlock();

            // Ensure notify_all is always called
            NotifyGuard notifyGuard(retrieve_cond);

            // Retrieve the value outside the mutex
            value = retrieveFunc(key);
            lock.lock();
            putInternal(key, value, false);
            retrieving_keys.erase(key);

            return value;
        }
    }

    size_t getThrashingMetrics() {
        std::lock_guard<std::mutex> lock(mutex);
        size_t evictions = 0;
        for (const auto& evicted : thrashingMetrics) {
            if (evicted) {
                evictions++;
            }
        }
        return evictions;
    }

private:
    size_t maxRamUsageBytes;
    size_t currentRamUsageBytes = 0;
    size_t thrashingWindow;
    size_t cleanUpThreshold;
    size_t evictionCount = 0;
    size_t requestCount = 0;
    std::vector<bool> thrashingMetrics;
    std::list<Key> items;
    tbb::concurrent_unordered_map<Key, std::pair<std::shared_ptr<Value>, typename std::list<Key>::iterator>> cache;
    tbb::concurrent_unordered_map<Key, std::weak_ptr<Value>> secondary_storage;
    std::unordered_set<Key> retrieving_keys;
    std::mutex mutex;
    std::condition_variable retrieve_cond;
    RetrieveFunc retrieveFunc;

    void putInternal(const Key key, std::shared_ptr<Value> value, bool fromSecondary) {
        auto it = cache.find(key);
        if (it != cache.end() && isValidIterator(it->second.second, items)) {
            // Update the value and move to front
            items.splice(items.begin(), items, it->second.second);
            currentRamUsageBytes -= getSize(it->second.first);
            it->second.first = value;
            currentRamUsageBytes += getSize(value);
        }
        else {
            // Handle new insertion or invalid iterator
            if (it == cache.end()) {
                currentRamUsageBytes += getSize(value);
            }
            else {
                currentRamUsageBytes -= getSize(it->second.first);
                currentRamUsageBytes += getSize(value);
            }

            while (currentRamUsageBytes > maxRamUsageBytes && !items.empty()) {
                // Evict the least recently used item
                const Key evicted_key = items.back();
                items.pop_back();
                auto evicted_item = cache.find(evicted_key);

                currentRamUsageBytes -= getSize(evicted_item->second.first);

                // Move to secondary storage
                if (evicted_item->second.first.use_count() > 1) {
                    secondary_storage[evicted_key] = evicted_item->second.first;
                }
                cache.unsafe_erase(evicted_key);

                evictionCount++;
                thrashingMetrics[requestCount % thrashingWindow] = true;
            }
            thrashingMetrics[requestCount % thrashingWindow] = false;
            requestCount++;

            if (evictionCount >= cleanUpThreshold) {
                cleanUpSecondary();
                evictionCount = 0;
            }

            if (!fromSecondary) {
                items.push_front(key);
            }
            else {
                items.insert(items.begin(), key);
            }

            cache[key] = { value, items.begin() };
        }
    }

    bool isValidIterator(const typename std::list<Key>::iterator& it, const std::list<Key>& lst) {
        return it != lst.end();
    }

    void cleanUpSecondary() {
        for (auto it = secondary_storage.begin(); it != secondary_storage.end();) {
            if (it->second.expired()) {
                it = secondary_storage.unsafe_erase(it);
            }
            else {
                ++it;
            }
        }
    }

    class NotifyGuard {
    public:
        NotifyGuard(std::condition_variable& cv) : cv(cv) {}
        ~NotifyGuard() {
            cv.notify_all();
        }
    private:
        std::condition_variable& cv;
    };
};

