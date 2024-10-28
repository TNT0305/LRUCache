//#include "lru_cache.h"
//
//#include <iostream>
//
//class MyClass {
//public:
//    std::shared_ptr<std::vector<int>> retrieve(const int& key) {
//        auto rv = std::make_shared<std::vector<int>>(std::initializer_list<int>{key, key + 1, key + 2});
//        rv->reserve(static_cast<size_t>(1) * 1024 * 1024 * 128);
//        return rv;
//    }
//};
//int main() {
//    MyClass myClass;
//
//    auto retrieveFunc = [&myClass](const int& key) -> std::shared_ptr<std::vector<int>> {
//        return myClass.retrieve(key);
//    };
//
//    size_t max_size = static_cast<size_t>(8) * 1024 * 1024 * 1024;
//    tnt::lru_cache<int, std::vector<int>> cache(max_size, retrieveFunc); // 8 GB max RAM usage
//
//    auto data1 = cache.get(1);
//    auto data2 = cache.get(2);
//    auto data3 = cache.get(3);
//    for (int i = 0; i < 100; ++i) cache.get(i);
//
//    std::cout << "Data1: " << data1->at(0) << std::endl;
//
//    auto data4 = cache.get(4);
//    auto data_existing = cache.get(1); // Should pull from secondary storage if still in use
//
//    std::cout << "Thrashing metrics (last 100 requests): " << cache.getThrashingMetrics() << std::endl;
//
//    return 0;
//}

#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <atomic>
#include <numeric>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include "lru_cache.h"

using namespace tnt;

class TestClass {
public:
    std::shared_ptr<std::vector<int>> retrieve(const int& key) {
        return std::make_shared<std::vector<int>>(std::initializer_list<int>{key, key + 1, key + 2});
    }
};

int main() {
    TestClass myTestClass;

    auto retrieveFunc = [&myTestClass](const int& key) -> std::shared_ptr<std::vector<int>> {
        return myTestClass.retrieve(key);
        };
    int thashing_window_size = 1000;
    lru_cache<int, std::vector<int>> cache(1 * 1024 * 1024, retrieveFunc, thashing_window_size); // 1 GB max RAM usage

    const int numElements = 1000000;
    std::vector<int> keys(numElements);
    std::iota(keys.begin(), keys.end(), 0);
    std::shuffle(keys.begin(), keys.end(), std::mt19937{ std::random_device{}() });

    auto start = std::chrono::high_resolution_clock::now();

    std::atomic<int> success_cnt(0), fail_cnt(0);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, keys.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
            if (cache.get(keys[i])) ++success_cnt;
            else ++fail_cnt;
        }
        });

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Insertion and initial access of " << numElements << " elements took " << elapsed.count() << " seconds. Successes: " << success_cnt.load() << ", failures: " << fail_cnt.load() << "\n";

    // Random Access Pattern
    start = std::chrono::high_resolution_clock::now();

    std::shuffle(keys.begin(), keys.end(), std::mt19937{ std::random_device{}() });
    success_cnt = 0;
    fail_cnt = 0;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, keys.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i) {
            if (cache.get(keys[i])) ++success_cnt;
            else ++fail_cnt;
        }
        });

    end = std::chrono::high_resolution_clock::now();
    elapsed = end - start;

    std::cout << "Random access of " << numElements << " elements took " << elapsed.count() << " seconds.\n";

    std::cout << "Thrashing metrics (last " << thashing_window_size << " requests): " << cache.get_thrashing_metrics() << std::endl;

    return 0;
}
