cmake_minimum_required(VERSION 3.21)
project(LRUCacheExample)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
# Set MSVC compatibility flags for C++20 (Crucial!)
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++20 /permissive- /Zc:preprocessor /Zc:twoPhase-")
endif()

# Clang-cl specific flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wmost -fdiagnostics-show-template-tree")
    # You could also try setting a specific MSVC compatibility version:
    # set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fms-compatibility-version=19.34") # Replace 19.34 with your VS version if needed
endif()

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
# Add custom target for preprocessor output
add_custom_target(
    preprocess_minimal
    COMMAND ${CMAKE_CXX_COMPILER} -E ${CMAKE_CURRENT_SOURCE_DIR}/minimal_test.cpp > ${CMAKE_CURRENT_BINARY_DIR}/preprocessed.cpp
    DEPENDS minimal_test # Optional: Make it depend on the executable
    COMMENT "Generating preprocessed minimal_test.cpp"
)
# Enable testing
enable_testing()
add_test(NAME ${PROJECT_NAME}_test COMMAND ${PROJECT_NAME}_test)
