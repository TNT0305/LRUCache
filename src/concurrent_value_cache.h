#include <atomic>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <tbb/concurrent_hash_map.h>
#include <concepts>

// gemini2 variant
namespace tnt::caching::gemini2 {

template<typename F, typename K, typename V>
concept fetcher = requires(F f, const K& k) {
    { f(k) } -> std::same_as<V>;
};

template<typename K, typename V>
class concurrent_value_cache; // Forward declaration

template<typename K, typename V>
struct ValueDeleter {
    concurrent_value_cache<K, V>* cache;
    K key;

    void operator()(V* ptr) const {
        if (cache) {
            {
                std::unique_lock<std::shared_mutex> write_lock(cache->map_mutex_);
                auto it = cache->value_map_.find(key);
                if (it != cache->value_map_.end() && it->second.value.get() == ptr) {
                    std::lock_guard<std::mutex> lru_lock(cache->lru_mutex_);
                    cache->lru_.push_front({key, 0});
                    cache->lru_map_[key] = cache->lru_.begin();
                    cache->current_memory_ += 0;
                    while (cache->current_memory_ > cache->max_memory_) {
                        cache->evict_lru();
                    }
                    cache->value_map_.erase(it);
                }
            }
        }
    }
};

template<typename K, typename V>
class concurrent_value_cache {
private:
    struct CacheEntry {
        std::shared_ptr<V> value;
        std::promise<std::shared_ptr<V>> promise;
        size_t size = 0;
    };

    struct LRUEntry {
        K key;
        size_t size;
    };

    void evict_lru() {
        std::unique_lock<std::mutex> lru_lock(lru_mutex_);
        if (lru_.empty()) return;

        auto& back = lru_.back();
        current_memory_ -= back.size;

        {
            std::unique_lock<std::shared_mutex> write_lock(map_mutex_);
            value_map_.erase(back.key);
        }
        lru_map_.erase(back.key);
        lru_.pop_back();
    }

    std::function<V(const K&)> fetcher_;
    size_t max_memory_;
    size_t current_memory_ = 0;
    std::shared_mutex map_mutex_;
    std::mutex lru_mutex_;
    std::unordered_map<K, CacheEntry> value_map_;
    std::list<LRUEntry> lru_;
    std::unordered_map<K, typename std::list<LRUEntry>::iterator> lru_map_;

public:
    template<typename Fetcher>
        requires fetcher<Fetcher, K, V>
    explicit concurrent_value_cache(Fetcher fetcher, size_t max_memory)
        : fetcher_(std::move(fetcher)), max_memory_(max_memory) {}

    template<typename Fetcher>
    concurrent_value_cache(Fetcher fetcher, size_t max_memory) -> concurrent_value_cache<decltype(std::declval<Fetcher>()(std::declval<K>())), K>;

    std::shared_ptr<V> get(const K& key) {
        {
            std::shared_lock<std::shared_mutex> read_lock(map_mutex_);
            auto it = value_map_.find(key);
            if (it != value_map_.end()) {
                return it->second.value;
            }
        }

        std::unique_lock<std::shared_mutex> write_lock(map_mutex_);
        auto [it, inserted] = value_map_.try_emplace(key, CacheEntry{});
        if (!inserted) {
            return it->second.value;
        }

        std::promise<std::shared_ptr<V>> promise;
        auto future = promise.get_future();
        it->second.promise = std::move(promise);

        write_lock.unlock();

        std::shared_ptr<V> value;
        try {
            V fetched_value = fetcher_(key);
            value = std::shared_ptr<V>(new V(fetched_value), ValueDeleter<K, V>{this, key});
            {
                std::unique_lock<std::shared_mutex> write_lock2(map_mutex_);
                it->second.value = value;
                std::lock_guard<std::mutex> lru_lock(lru_mutex_);
                lru_.push_front({key, 0});
                lru_map_[key] = lru_.begin();
                while (current_memory_ > max_memory_) {
                    evict_lru();
                }
            }
            it->second.promise.set_value(value);

        } catch (...) {
            {
                std::unique_lock<std::shared_mutex> write_lock2(map_mutex_);
                value_map_.erase(key);
            }
            it->second.promise.set_exception(std::current_exception());
            throw;
        }

        return future.get();
    }
};
}; // tnt::caching::gemini2