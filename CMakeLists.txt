cmake_minimum_required(VERSION 3.21)
project(LRUCacheExample)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
# Find and link TBB
find_package(TBB CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# Add test executable
add_executable(${PROJECT_NAME}_test 
    src/main.cpp
)

target_link_libraries(${PROJECT_NAME}_test 
    PRIVATE 
        TBB::tbb
        GTest::gtest 
        GTest::gtest_main
)

# Enable testing
enable_testing()
add_test(NAME ${PROJECT_NAME}_test COMMAND ${PROJECT_NAME}_test)
