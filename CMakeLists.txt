cmake_minimum_required(VERSION 3.22)
project(MaliCompat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(malicompat SHARED
    malicompat-core/capability_analyzer.cpp
    malicompat-core/descriptor_virtualizer.cpp
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
