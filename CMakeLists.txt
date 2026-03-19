# Minimum CMake version. 3.15 is a good baseline for modern features like CONFIGURE_DEPENDS.
cmake_minimum_required(VERSION 3.15)

# Project name and languages.
project(libera-core LANGUAGES CXX)

# Enable CTest and define BUILD_TESTING.
include(CTest)

# ---------------------------
# Compiler and language setup
# ---------------------------
# Use a fixed C++ standard and avoid compiler extensions for portability.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
# Toggle with -DSANITIZE=none|address|thread (presets above set this)
set(SANITIZE "none" CACHE STRING "Sanitizer to use (none|address|thread)")
set_property(CACHE SANITIZE PROPERTY STRINGS none address thread)

option(LIBERA_BUILD_TESTS "Build libera-core tests" ${BUILD_TESTING})
option(LIBERA_BUILD_INTEGRATION_TESTS "Build integration tests (loopback/network)" OFF)
option(LIBERA_BUILD_HARDWARE_TESTS "Build hardware tests (requires controller hardware)" OFF)
option(LIBERA_BUILD_EXAMPLES "Build example applications" OFF)

# Optional: a few sensible warnings. Comment out if you prefer quiet builds.
function(libera_apply_warnings target)
  if (MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /FS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Sanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Libusb.cmake)

# --------------------------------
# Helios SDK libusb selection
# --------------------------------
option(LIBERA_USE_BUNDLED_LIBUSB "Use bundled libusb from libs/helios_dac" OFF)
set(LIBERA_BUNDLED_LIBUSB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp"
    CACHE PATH "Path to bundled libusb headers/binaries"
)
libera_configure_libusb()

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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp>
)
libera_apply_warnings(libera-core)
libera_apply_sanitizers(libera-core)

# Helios SDK sources (USB + IDN).
set(HELIOS_SDK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/HeliosDac.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/idn/idn.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/idn/idnServerList.cpp"
)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/HeliosLog.cpp")
    list(APPEND HELIOS_SDK_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/HeliosLog.cpp"
    )
endif()
if (WIN32)
    list(APPEND HELIOS_SDK_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/idn/plt-windows.cpp"
    )
else()
    list(APPEND HELIOS_SDK_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/idn/plt-posix.cpp"
    )
endif()
add_library(helios-sdk OBJECT ${HELIOS_SDK_SOURCES})
target_include_directories(helios-sdk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp")
target_compile_definitions(helios-sdk PRIVATE HELIOS_ENABLE_IDN_DISCOVERY=1)
if (MSVC)
  target_compile_options(helios-sdk PRIVATE /W0)
else()
  target_compile_options(helios-sdk PRIVATE -w)
endif()
target_sources(libera-core PRIVATE $<TARGET_OBJECTS:helios-sdk>)

target_link_libraries(libera-core PUBLIC libusb::libusb)

# Windows needs Winsock2 for TCP and UDP symbols. Linux and macOS do not.
# _WIN32_WINNT=0x0A00 targets Windows 10+, suppressing ASIO's version warnings
# and enabling modern socket APIs.
if (WIN32)
    target_link_libraries(libera-core PUBLIC ws2_32)
    target_compile_definitions(libera-core PUBLIC _WIN32_WINNT=0x0A00)
endif()

if (WIN32)
    set(LIBERA_OFX_RTAUDIO_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../libs/rtAudio")
    find_path(LIBERA_RTAUDIO_INCLUDE_DIR
        NAMES RtAudio.h
        HINTS
            "${LIBERA_OFX_RTAUDIO_ROOT}/include")
    find_library(LIBERA_RTAUDIO_LIBRARY
        NAMES rtaudio RtAudio
        HINTS
            "${LIBERA_OFX_RTAUDIO_ROOT}/lib"
        PATH_SUFFIXES
            vs/x64
            vs/Win32
            win64
            win
            x64
            x86)

    if (LIBERA_RTAUDIO_INCLUDE_DIR AND LIBERA_RTAUDIO_LIBRARY)
        target_include_directories(libera-core PRIVATE "${LIBERA_RTAUDIO_INCLUDE_DIR}")
        target_link_libraries(libera-core PRIVATE "${LIBERA_RTAUDIO_LIBRARY}")
    endif()
endif()

if (APPLE)
    find_library(CORE_AUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(AUDIO_TOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    target_link_libraries(libera-core
        PUBLIC
            ${CORE_AUDIO_FRAMEWORK}
            ${CORE_FOUNDATION_FRAMEWORK}
            ${AUDIO_TOOLBOX_FRAMEWORK})
endif()

# --------------------------------
# Tests
# --------------------------------
if (BUILD_TESTING AND LIBERA_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# --------------------------------
# Examples
# --------------------------------
if (LIBERA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
