cmake_minimum_required(VERSION 3.21)
project(LRUCacheExample)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find and link TBB
find_package(TBB REQUIRED)

# Specify the source files
add_executable(LRUCacheExample src/main.cpp)

# Link TBB
target_link_libraries(LRUCacheExample PRIVATE TBB::tbb)
