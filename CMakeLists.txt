# Minimum CMake version. 3.15 is a good baseline for modern features like CONFIGURE_DEPENDS.
cmake_minimum_required(VERSION 3.15)

# Project name and languages.
project(libera-core LANGUAGES CXX)

# ---------------------------
# Compiler and language setup
# ---------------------------
# Use a fixed C++ standard and avoid compiler extensions for portability.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Toggle with -DSANITIZE=none|address|thread (presets above set this)
set(SANITIZE "none" CACHE STRING "Sanitizer to use (none|address|thread)")
set_property(CACHE SANITIZE PROPERTY STRINGS none address thread)

# Apply only to Debug builds
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (SANITIZE STREQUAL "address")
    set(SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  elseif (SANITIZE STREQUAL "thread")
    set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  endif()

  if (SAN_FLAGS)
    add_compile_options(${SAN_FLAGS})
    add_link_options(${SAN_FLAGS})
  endif()
endif()


# Optional: a few sensible warnings. Comment out if you prefer quiet builds.
if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --------------------------------
# Collect sources for the library
# --------------------------------
# Finds all .cpp files under src/ at configure time. When you add new files,
# CMake will reconfigure because of CONFIGURE_DEPENDS.
file(GLOB_RECURSE LIBERA_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# ----------------------------
# Define the library target
# ----------------------------
# By default this is a static library. You can change to SHARED if you prefer.
add_library(libera-core ${LIBERA_CORE_SOURCES})

# Public include directories:
# - BUILD_INTERFACE is used when building this project directly.
# - INSTALL_INTERFACE is used after installation.
# Add the vendored Asio include path so headers like <asio.hpp> resolve.
target_include_directories(libera-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/asio/include>
)

# Windows needs Winsock2 for TCP and UDP symbols. Linux and macOS do not.
if (WIN32)
    target_link_libraries(libera-core PUBLIC ws2_32)
endif()

# --------------------------------
# Tests
# --------------------------------
enable_testing()

# Gather all test sources under tests/.
file(GLOB_RECURSE LIBERA_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
)

# Create one test executable per source file and link to the library.
foreach(test_source ${LIBERA_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE libera-core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# --------------------------------
# Example or main executable
# --------------------------------
add_executable(libera-main apps/main.cpp)
target_link_libraries(libera-main PRIVATE libera-core)

add_executable(laser-stream-test apps/LaserStreamTest.cpp)
target_link_libraries(laser-stream-test PRIVATE libera-core)
