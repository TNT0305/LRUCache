#include "concurrent_value_cache.h" // Include your cache header
#include <iostream>
#include <string>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <cassert>

using namespace tnt::caching::gemini2; // If you are using a namespace

struct TestValue {
    std::string data;
    TestValue(const std::string& d) : data(d + std::string(1000 - d.length(), 'x')) {}
};

size_t get_size(const TestValue& value) { // Correct signature
    // Account for actual string memory allocation, not just size
    // std::string typically uses small string optimization (SSO) around 15-16 bytes
    // For larger strings, it allocates on heap
    return sizeof(TestValue) + (value.data.capacity() + 1) * sizeof(char);
}

int main() {
    constexpr size_t num_custfunctions = 10;
    constexpr size_t elements_per_custfn = 100000;
    constexpr size_t total_unique_elements =
        static_cast<size_t>(elements_per_custfn * 1.1); // 10% unique elements (90% sharing)
    constexpr size_t max_memory = 1024 * 1024 * 30; // 10 MB cache

    std::atomic<size_t> fetches{ 0 };
    auto cache = concurrent_value_cache<std::string, TestValue>(
        [&fetches](const std::string& key) {
            ++fetches;
            return TestValue(key + "_value");
        }, max_memory
    );

    // Create 10 custFunctions, each with their own thread
    std::vector<std::thread> threads;
    std::atomic<bool> keep_running{ true };
    std::atomic<int> total_count{ 0 };

    // Simulate real access patterns:
    // 1. Each custFunction mainly accesses its own subset
    // 2. But there's 90% overlap between them
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_custfunctions; ++i) {
        threads.emplace_back([&, i]() {
            std::random_device rd;
            std::mt19937 gen(rd());

            // Each custFunction has:
            // - 90% access to shared elements (0 to 90% of total_unique_elements)
            // - 10% access to its own unique elements (90% + i*1% to 90% + (i+1)*1% of total_unique_elements)
            std::uniform_int_distribution<> shared_dist(0, static_cast<int>(total_unique_elements * 0.9));
            std::uniform_int_distribution<> unique_dist(
                static_cast<int>(total_unique_elements * 0.9 + i * 0.01 * total_unique_elements),
                static_cast<int>(total_unique_elements * 0.9 + (i + 1) * 0.01 * total_unique_elements)
            ); 
            // Simulate continuous access
            while (keep_running) {
                ++total_count;
                // 90% probability of accessing shared elements
                if (gen() % 100 < 90) {
                    std::string key = "shared_" + std::to_string(shared_dist(gen));
                    auto val = cache.get(key);
                }
                else {
                    std::string key = "unique_" + std::to_string(unique_dist(gen));
                    auto val = cache.get(key);
                }
            }
            });
    }

    // Let it run for 5 seconds
    std::this_thread::sleep_for(std::chrono::seconds(10));
    keep_running = false;

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Test Statistics:\n";
    std::cout << "Duration: " << duration.count() << "ms\n";
    std::cout << "Fetches: " << fetches.load() << "\n";
    std::cout << "Cache size: " << cache.get_lru_size() << "\n";
    std::cout << "Reactivations: " << cache.get_reactivation_count() << "\n";
    std::cout << "Second Consumers: " << cache.get_second_consumer_count() << "\n";
    std::cout << "Evictions: " << cache.get_eviction_count() << "\n";
	std::cout << "Total count: " << total_count.load() << "\n";
    double average_time_per_fetch = static_cast<double>(duration.count()) / (total_count.load());
    std::cout << "Average time per fetch: " << average_time_per_fetch << "ms" << std::endl;

    return 0;
}

int main_original() {
    constexpr size_t num_threads = 32;
    constexpr size_t num_keys = 100000;
    constexpr size_t num_iterations = 100000;
    //constexpr size_t max_memory = 1024 * 1024; 
    constexpr size_t max_memory = 1024 * 1024 * 30; // Increase cache size to 10 MB

    std::atomic<size_t> fetches{ 0 };
    auto cache = concurrent_value_cache<std::string, TestValue>(
        [&fetches](const std::string& key) {
            ++fetches;
            return TestValue(key + "_value");
        }, max_memory
    );

    std::vector<std::thread> threads;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, num_keys - 1);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (size_t j = 0; j < num_iterations; ++j) {
                int key_index = distrib(gen);
                std::string key = "key" + std::to_string(key_index);
                auto val = cache.get(key);
                auto d = key + "_value";
                assert(val->data == d + std::string(1000 - d.length(), 'x'));
            }
            });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    size_t reactivations = cache.get_reactivation_count();
    size_t second_consumers = cache.get_second_consumer_count();
    size_t evictions = cache.get_eviction_count();

    std::cout << "Reactivations: " << reactivations << std::endl;
    std::cout << "Second Consumers: " << second_consumers << std::endl;
    std::cout << "Evictions: " << evictions << std::endl;

    std::cout << "Test completed." << std::endl;
    std::cout << "Time taken: " << duration.count() << "ms" << std::endl;
    std::cout << "Actual Fetches: " << fetches.load() << std::endl;
    std::cout << "Total Fetches: " << num_iterations * num_threads << std::endl;
    // Calculate and report the average time per fetch in milliseconds
    double average_time_per_fetch = static_cast<double>(duration.count()) / (num_iterations * num_threads);
    std::cout << "Average time per fetch: " << average_time_per_fetch << "ms" << std::endl;
    assert(fetches <= num_keys);
    std::cout << "Cache size: " << cache.get_lru_size() << std::endl;

    return 0;
}


// #include "caching_factory.h"
// #include "value_cache.h"
// #include <gtest/gtest.h>
// #include <string>
// #include <vector>
// #include <thread>
// #include <chrono>
// #include <iostream>
// #include <random>
// #include <format>

// //using namespace tnt::caching;
// using namespace tnt::caching2;

// // Test value class with controlled memory usage
// class TestValue {
// public:
//     explicit TestValue(std::string key) : data_(1000, 'x') {
//         data_ = key + std::string(1000 - key.length(), 'x');
//     }
    
//     [[nodiscard]] size_t size() const noexcept { return data_.size(); }
//     [[nodiscard]] const std::string& data() const { return data_; }

// private:
//     std::string data_;
// };

// // TEST(CachingFactoryTest, BasicFunctionality) {
// //     auto factory = CachingFactory([](const std::string& key) {
// //         return TestValue(key);
// //     });

// //     auto value1 = factory.get("key1");
// //     EXPECT_EQ(value1->data().substr(0, 4), "key1");

// //     // Should get same value
// //     auto value2 = factory.get("key1");
// //     EXPECT_EQ(value1, value2);
// // }

// // TEST(CachingFactoryTest, ConcurrentAccess) {
// //     auto factory = CachingFactory([](const std::string& key) {
// //         std::this_thread::sleep_for(std::chrono::milliseconds(1));
// //         return TestValue(key);
// //     });

// //     std::vector<std::thread> threads;
// //     std::atomic<int> successful_gets = 0;

// //     for (int i = 0; i < 10; ++i) {
// //         threads.emplace_back([&factory, &successful_gets] {
// //             try {
// //                 auto value = factory.get("shared_key");
// //                 EXPECT_EQ(value->data().substr(0, 10), "shared_key");
// //                 ++successful_gets;
// //             }
// //             catch (...) {
// //                 // Count should not increment on failure
// //             }
// //         });
// //     }

// //     for (auto& thread : threads) {
// //         thread.join();
// //     }

// //     EXPECT_EQ(successful_gets, 10);
// // }

// // TEST(CachingFactoryTest, ExceptionHandling) {
// //     bool should_throw = false;
// //     auto factory = CachingFactory([&should_throw](const std::string& key) {
// //         if (should_throw) throw std::runtime_error("Simulated failure");
// //         return TestValue(key);
// //     });

// //     auto value1 = factory.get("key1");
// //     should_throw = true;
// //     EXPECT_THROW(factory.get("key2"), std::runtime_error);
// // }
// // void run_stress_test() {
// //     auto factory = CachingFactory(
// //         [](const std::string& key) { return TestValue(key); },
// //         10 * 1024 * 1024
// //     );

// //     constexpr int num_threads = 8;
// //     constexpr int ops_per_thread = 100000;
// //     std::vector<std::thread> threads;
// //     std::mutex cout_mutex;
// //     std::atomic<int> active_threads{0};

// //     auto thread_func = [&](int thread_id) {
// //         active_threads++;
// //         std::random_device rd;
// //         std::mt19937 gen(rd());
// //         std::uniform_int_distribution<> key_dist(0, 10);
        
// //         auto thread_start = std::chrono::high_resolution_clock::now();
        
// //         for (int i = 0; i < ops_per_thread; ++i) {
// //             auto key = std::format("key_{}", key_dist(gen));
// //             auto value = factory.get(key);
            
// //             if (i % 1000 == 0) {
// //                 std::lock_guard lock(cout_mutex);
// //                 std::cout << std::format("Thread {} @ {}: {} ops, {} bytes\n", 
// //                     thread_id, 
// //                     std::chrono::duration_cast<std::chrono::milliseconds>(
// //                         std::chrono::high_resolution_clock::now() - thread_start).count(),
// //                     i, 
// //                     factory.get_current_memory_usage());
// //             }
// //         }
        
// //         auto thread_end = std::chrono::high_resolution_clock::now();
// //         {
// //             std::lock_guard lock(cout_mutex);
// //             std::cout << std::format("Thread {} complete in {}ms\n",
// //                 thread_id,
// //                 std::chrono::duration_cast<std::chrono::milliseconds>(
// //                     thread_end - thread_start).count());
// //         }
// //         active_threads--;
// //     };

// //     auto start = std::chrono::high_resolution_clock::now();
    
// //     for (int i = 0; i < num_threads; ++i) {
// //         threads.emplace_back(thread_func, i);
// //     }

// //     // Monitor thread completion
// //     while (active_threads > 0) {
// //         std::this_thread::sleep_for(std::chrono::milliseconds(100));
// //         std::lock_guard lock(cout_mutex);
// //         std::cout << std::format("Active threads: {}, Memory: {} bytes\n", 
// //             active_threads.load(), 
// //             factory.get_current_memory_usage());
// //     }

// //     // Wait for completion
// //     for (auto& thread : threads) {
// //         thread.join();
// //     }

// //     auto end = std::chrono::high_resolution_clock::now();
// //     auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

// //     std::cout << std::format("\nTest completed:\n"
// //                             "Total operations: {}\n"
// //                             "Duration: {}ms\n"
// //                             "Operations/sec: {:.2f}\n"
// //                             "Final cache memory: {} bytes\n",
// //                             num_threads * ops_per_thread,
// //                             duration.count(),
// //                             (num_threads * ops_per_thread * 1000.0) / duration.count(),
// //                             factory.get_current_memory_usage());
// // }
// void run_stress_test_value_cache() {
//     auto factory = value_cache(
//         [](const std::string& key) { return TestValue(key); },
//         static_cast<size_t>(10 * 1024 * 1024)
//     );

//     constexpr int num_threads = 8;
//     constexpr int ops_per_thread = 100000;
//     std::vector<std::thread> threads;
//     std::mutex cout_mutex;
//     std::atomic<int> active_threads{0};

//     auto thread_func = [&](int thread_id) {
//         active_threads++;
//         std::random_device rd;
//         std::mt19937 gen(rd());
//         std::uniform_int_distribution<> key_dist(0, 10);
        
//         auto thread_start = std::chrono::high_resolution_clock::now();
        
//         for (int i = 0; i < ops_per_thread; ++i) {
//             auto key = std::format("key_{}", key_dist(gen));
//             auto value = factory.get(key);
            
//             if (i % 1000 == 0) {
//                 std::lock_guard lock(cout_mutex);
//                 std::cout << std::format("Thread {} @ {}: {} ops, {} bytes\n", 
//                     thread_id, 
//                     std::chrono::duration_cast<std::chrono::milliseconds>(
//                         std::chrono::high_resolution_clock::now() - thread_start).count(),
//                     i, 
//                     factory.get_current_memory_usage());
//             }
//         }
        
//         auto thread_end = std::chrono::high_resolution_clock::now();
//         {
//             std::lock_guard lock(cout_mutex);
//             std::cout << std::format("Thread {} complete in {}ms\n",
//                 thread_id,
//                 std::chrono::duration_cast<std::chrono::milliseconds>(
//                     thread_end - thread_start).count());
//         }
//         active_threads--;
//     };

//     auto start = std::chrono::high_resolution_clock::now();
    
//     for (int i = 0; i < num_threads; ++i) {
//         threads.emplace_back(thread_func, i);
//     }

//     // Monitor thread completion
//     while (active_threads > 0) {
//         std::this_thread::sleep_for(std::chrono::milliseconds(100));
//         std::lock_guard lock(cout_mutex);
//         std::cout << std::format("Active threads: {}, Memory: {} bytes\n", 
//             active_threads.load(), 
//             factory.get_current_memory_usage());
//     }

//     // Wait for completion
//     for (auto& thread : threads) {
//         thread.join();
//     }

//     auto end = std::chrono::high_resolution_clock::now();
//     auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

//     std::cout << std::format("\nTest completed:\n"
//                             "Total operations: {}\n"
//                             "Duration: {}ms\n"
//                             "Operations/sec: {:.2f}\n"
//                             "Final cache memory: {} bytes\n",
//                             num_threads * ops_per_thread,
//                             duration.count(),
//                             (num_threads * ops_per_thread * 1000.0) / duration.count(),
//                             factory.get_current_memory_usage());
// }
// int main() {
    
//     run_stress_test_value_cache();
//     return 0;
// }

// // #include <iostream>
// // #include <vector>
// // #include <random>
// // #include <chrono>
// // #include <atomic>
// // #include <numeric>
// // #include <tbb/parallel_for.h>
// // #include <tbb/blocked_range.h>
// // #include "lru_cache.h"

// // using namespace tnt;
// // // Bring tnt::get_size into the global namespace

// // class TestClass {
// // public:
// // 	static std::shared_ptr<std::vector<int>> retrieve(const int& key) noexcept {
// //         return std::make_shared<std::vector<int>>(std::initializer_list{key, key + 1, key + 2});
// //     }
// // };

// // //int main() {
// // //    using Key = int;
// // //    using Value = std::vector<int>;
// // //
// // //    auto retrieveFunc = [](const int& key) noexcept -> std::shared_ptr<std::vector<int>> {
// // //        return TestClass::retrieve(key);
// // //        };
// // //
// // //    // Set cache size to 1 MB
// // //    tnt::MemoryThresholdEvictionPolicy<Key, Value> eviction_policy(1 * 1024);
// // //
// // //    // Use in-memory secondary storage
// // //    tnt::InMemorySecondaryStorage<Key, Value> secondary_storage;
// // //	//tnt::NullSecondaryStorage<Key, Value> secondary_storage;
// // //
// // //    // Create the cache
// // //    tnt::lru_cache cache(std::move(eviction_policy), secondary_storage, retrieveFunc);
// // //
// // //    const int numElements = 1000000;
// // //    std::vector<int> keys(numElements);
// // //    std::iota(keys.begin(), keys.end(), 0);
// // //
// // //    // Test different access patterns
// // //    std::vector<std::string> patterns = { "Sequential", "Random", "Frequent", "External References" };
// // //
// // //    for (const auto& pattern : patterns) {
// // //        // Adjust keys based on pattern
// // //        if (pattern == "Random") {
// // //            std::ranges::shuffle(keys, std::mt19937{ std::random_device{}() });
// // //        }
// // //        else if (pattern == "Frequent") {
// // //            // Access a small subset frequently
// // //            keys.assign(numElements, 0);
// // //            std::ranges::generate(keys, [n = 0]() mutable {
// // //                return n++ % 200; // Keys 0 to 199
// // //                });
// // //        }
// // //        else if (pattern == "Sequential") {
// // //            // Sequential access
// // //            std::iota(keys.begin(), keys.end(), 0);
// // //        }
// // //        else if (pattern == "External References") {
// // //            // Prepare keys for external references test
// // //            std::iota(keys.begin(), keys.end(), 0);
// // //        }
// // //
// // //        auto start = std::chrono::high_resolution_clock::now();
// // //
// // //        std::atomic<int> hit_count(0), miss_count(0);
// // //        std::atomic<int> correctness_errors(0);
// // //
// // //        if (pattern == "External References") {
// // //            // Store 10% of the retrieved values externally
// // //            std::vector<std::shared_ptr<Value>> external_refs;
// // //            const int externalRefCount = numElements / 10;
// // //            for (int i = 0; i < externalRefCount; ++i) {
// // //	            if (auto t = cache.get(keys[i]))
// // //                {
// // //                    external_refs.push_back(*t);
// // //                }
// // //            }
// // //
// // //            // Access additional keys to trigger eviction
// // //            tbb::parallel_for(tbb::blocked_range<size_t>(externalRefCount, keys.size()),
// // //                [&](const tbb::blocked_range<size_t>& r) {
// // //                    for (size_t i = r.begin(); i != r.end(); ++i) {
// // //                        if (auto result = cache.get(keys[i])) {
// // //                            ++hit_count;
// // //                        }
// // //                        else {
// // //                            ++miss_count;
// // //                        }
// // //                    }
// // //                });
// // //
// // //            // Refetch the externally held keys to test promotion
// // //            tbb::parallel_for(tbb::blocked_range<size_t>(0, external_refs.size()),
// // //                [&](const tbb::blocked_range<size_t>& r) {
// // //                    for (size_t i = r.begin(); i != r.end(); ++i) {
// // //                        auto key = keys[i];
// // //                        if (auto result = cache.get(key)) {
// // //                            ++hit_count;
// // //
// // //                            // Verify correctness
// // //                            auto expected = TestClass::retrieve(key);
// // //                            if (*(*result) != *expected) {
// // //                                ++correctness_errors;
// // //                            }
// // //                        }
// // //                        else {
// // //                            ++miss_count;
// // //                        }
// // //                    }
// // //                });
// // //
// // //            // Optional: Clear external references
// // //            external_refs.clear();
// // //
// // //        }
// // //        else {
// // //            tbb::parallel_for(tbb::blocked_range<size_t>(0, keys.size()),
// // //                [&](const tbb::blocked_range<size_t>& r) {
// // //                    for (size_t i = r.begin(); i != r.end(); ++i) {
// // //                        if (auto result = cache.get(keys[i])) {
// // //                            ++hit_count;
// // //
// // //                            // Verify correctness
// // //                            auto expected = TestClass::retrieve(keys[i]);
// // //                            if (auto t = *result; *t != *expected) {
// // //                                ++correctness_errors;
// // //                            }
// // //                        }
// // //                        else {
// // //                            ++miss_count;
// // //                        }
// // //                    }
// // //                });
// // //        }
// // //
// // //        auto end = std::chrono::high_resolution_clock::now();
// // //        std::chrono::duration<double> elapsed = end - start;
// // //
// // //        std::cout << pattern << " access of " << numElements
// // //            << " elements took " << elapsed.count() << " seconds.\n"
// // //            << "Cache hits: " << hit_count.load()
// // //            << ", Cache misses: " << miss_count.load()
// // //            << ", Correctness errors: " << correctness_errors.load()
// // //            << "\n\n";
// // //    }
// // //
// // //    return 0;
// // //}
// // int main() {
// //     using Key = int;
// //     using Value = std::vector<int>;

// //     auto retrieveFunc = [](const Key& key) noexcept -> std::shared_ptr<Value> {
// //         return TestClass::retrieve(key);
// //         };


// //     // Maximum allowed memory for the cache (e.g., 100 MB)
// //     size_t max_memory = 1024 * 1024 * 1;
// // 	tnt::MemoryThresholdEvictionPolicy<Key, Value> eviction_policy(max_memory);

// //     // Create the cache instance
// // 	//auto cache = ConcurrentCache(retrieveFunc, std::move(eviction_policy));
// //     ConcurrentCache cache(retrieveFunc, eviction_policy);


// //     // Create the cache
// //     //tnt::lru_cache cache(std::move(eviction_policy), secondary_storage, retrieveFunc);

// //     const int numElements = 1000000;
// //     std::vector<int> keys(numElements);
// //     std::iota(keys.begin(), keys.end(), 0);

// //     // Test different access patterns
// //     std::vector<std::string> patterns = { "Sequential", "Random", "Frequent", "External References" };

// //     for (const auto& pattern : patterns) {
// //         // Adjust keys based on pattern
// //         if (pattern == "Random") {
// //             std::ranges::shuffle(keys, std::mt19937{ std::random_device{}() });
// //         }
// //         else if (pattern == "Frequent") {
// //             // Access a small subset frequently
// //             keys.assign(numElements, 0);
// //             std::ranges::generate(keys, [n = 0]() mutable {
// //                 return n++ % 200; // Keys 0 to 199
// //                 });
// //         }
// //         else if (pattern == "Sequential") {
// //             // Sequential access
// //             std::iota(keys.begin(), keys.end(), 0);
// //         }
// //         else if (pattern == "External References") {
// //             // Prepare keys for external references test
// //             std::iota(keys.begin(), keys.end(), 0);
// //         }

// //         auto start = std::chrono::high_resolution_clock::now();

// //         std::atomic<int> hit_count(0), miss_count(0);
// //         std::atomic<int> correctness_errors(0);

// //         if (pattern == "External References") {
// //             // Store 10% of the retrieved values externally
// //             std::vector<std::shared_ptr<Value>> external_refs;
// //             const int externalRefCount = numElements / 10;
// //             for (int i = 0; i < externalRefCount; ++i) {
// //                 if (auto t = cache.get(keys[i]))
// //                 {
// //                     external_refs.push_back(t);
// //                 }
// //             }

// //             // Access additional keys to trigger eviction
// //             tbb::parallel_for(tbb::blocked_range<size_t>(externalRefCount, keys.size()),
// //                 [&](const tbb::blocked_range<size_t>& r) {
// //                     for (size_t i = r.begin(); i != r.end(); ++i) {
// //                         if (auto result = cache.get(keys[i])) {
// //                             ++hit_count;
// //                         }
// //                         else {
// //                             ++miss_count;
// //                         }
// //                     }
// //                 });

// //             // Refetch the externally held keys to test promotion
// //             tbb::parallel_for(tbb::blocked_range<size_t>(0, external_refs.size()),
// //                 [&](const tbb::blocked_range<size_t>& r) {
// //                     for (size_t i = r.begin(); i != r.end(); ++i) {
// //                         auto key = keys[i];
// //                         if (auto result = cache.get(key)) {
// //                             ++hit_count;

// //                             // Verify correctness
// //                             auto expected = TestClass::retrieve(key);
// //                             if (*result != *expected) {
// //                                 ++correctness_errors;
// //                             }
// //                         }
// //                         else {
// //                             ++miss_count;
// //                         }
// //                     }
// //                 });

// //             // Optional: Clear external references
// //             external_refs.clear();

// //         }
// //         else {
// //             tbb::parallel_for(tbb::blocked_range<size_t>(0, keys.size()),
// //                 [&](const tbb::blocked_range<size_t>& r) {
// //                     for (size_t i = r.begin(); i != r.end(); ++i) {
// //                         if (auto result = cache.get(keys[i])) {
// //                             ++hit_count;

// //                             // Verify correctness
// //                             auto expected = TestClass::retrieve(keys[i]);
// //                             if (auto t = *result; t != *expected) {
// //                                 ++correctness_errors;
// //                             }
// //                         }
// //                         else {
// //                             ++miss_count;
// //                         }
// //                     }
// //                 });
// //         }

// //         auto end = std::chrono::high_resolution_clock::now();
// //         std::chrono::duration<double> elapsed = end - start;

// //         std::cout << pattern << " access of " << numElements
// //             << " elements took " << elapsed.count() << " seconds.\n"
// //             << "Cache hits: " << hit_count.load()
// //             << ", Cache misses: " << miss_count.load()
// //             << ", Correctness errors: " << correctness_errors.load()
// //             << "\n\n";
// //     }

// //     return 0;
// // }
