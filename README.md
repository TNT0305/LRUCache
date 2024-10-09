# LRUCache

An implementation of an efficient LRU (Least Recently Used) cache in C++, leveraging Intel TBB for concurrency. This cache is designed to handle high-performance use cases with flexible memory management and robust thread safety.

## Features
- **Memory-Limited Cache**: Limits the cache size based on memory usage.
- **Thrashing Metrics**: Tracks evictions over the last N requests to monitor cache efficiency.
- **Secondary Storage**: Retains in-use items that are evicted, ensuring they can be retrieved without reloading.
- **Intel TBB Integration**: Uses `tbb::concurrent_unordered_map` for high-performance concurrency.
- **Flexible Size Calculation**: Supports custom size calculation functions for different data types.
- **Condition Variable Guard**: Ensures `notify_all` is always called, maintaining thread synchronization.

## Usage

### Example
Here's how to use `LRUCache` with a custom retrieval function:

```cpp
#include "LRUCache.h"
#include <vector>
#include <memory>

class MyClass {
public:
    std::shared_ptr<std::vector<int>> retrieve(const int& key) {
        return std::make_shared<std::vector<int>>(std::initializer_list<int>{key, key + 1, key + 2});
    }
};

int main() {
    MyClass myClass;

    auto retrieveFunc = [&myClass](const int& key) -> std::shared_ptr<std::vector<int>> {
        return myClass.retrieve(key);
    };

    LRUCache<int, std::vector<int>> cache(8 * 1024 * 1024 * 1024, retrieveFunc); // 8 GB max RAM usage

    auto data1 = cache.get(1);
    auto data2 = cache.get(2);
    auto data3 = cache.get(3);

    std::cout << "Data1: " << data1->at(0) << std::endl;

    auto data4 = cache.get(4);
    auto data_existing = cache.get(1); // Should pull from secondary storage if still in use

    std::cout << "Thrashing metrics (last 100 requests): " << cache.getThrashingMetrics() << std::endl;

    return 0;
}
## Requirements
- **C++20** or later
- **Intel TBB** library (can be installed via vcpkg)

## Building
Make sure to have [vcpkg](https://github.com/microsoft/vcpkg) installed and TBB available. Use the following CMake setup:

### CMakeLists.txt
```cmake
cmake_minimum_required(VERSION 3.21)
project(LRUCacheExample)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

find_package(TBB REQUIRED)

add_executable(LRUCacheExample src/main.cpp)

target_link_libraries(LRUCacheExample PRIVATE TBB::tbb)

###CMakePresets.json
{
  "version": 3,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 21,
    "patch": 0
  },
  "presets": [
    {
      "name": "default",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "YES"
      }
    },
    {
      "name": "windows",
      "inherits": "default",
      "description": "Configure for Windows with Clang and vcpkg",
      "toolchainFile": "C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "architecture": {
        "value": "x64"
      },
      "generator": "Ninja",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang-cl",
        "CMAKE_CXX_COMPILER": "clang-cl"
      }
    },
    {
      "name": "linux",
      "inherits": "default",
      "description": "Configure for Linux with Clang and vcpkg",
      "toolchainFile": "/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "generator": "Ninja",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_CXX_COMPILER": "clang++"
      }
    }
  ]
}
###License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
