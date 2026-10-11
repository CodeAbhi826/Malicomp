cmake_minimum_required(VERSION 3.22)
project(MaliCompat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Crucial for Exynos 7884 (Cortex-A53 / Cortex-A73): strictly ARMv8.0-A baseline
add_compile_options(-march=armv8-a)

add_library(malicompat SHARED
    malicompat-core/capability_analyzer.cpp
    malicompat-core/descriptor_virtualizer.cpp
    malicompat-core/descriptor_shim.cpp
)

target_include_directories(malicompat PUBLIC
    malicompat-core
)

find_library(VULKAN_LIB vulkan)
find_library(LOG_LIB log)

if(VULKAN_LIB)
    target_link_libraries(malicompat PRIVATE ${VULKAN_LIB})
endif()

if(LOG_LIB)
    target_link_libraries(malicompat PRIVATE ${LOG_LIB})
endif()
