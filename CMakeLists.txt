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
# Helios SDK libusb selection
# --------------------------------
option(LIBERA_USE_BUNDLED_LIBUSB "Use bundled libusb from libs/helios_dac" OFF)
set(LIBERA_BUNDLED_LIBUSB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp"
    CACHE PATH "Path to bundled libusb headers/binaries"
)

set(LIBERA_LIBUSB_FOUND FALSE)

if (NOT LIBERA_USE_BUNDLED_LIBUSB)
  find_package(PkgConfig QUIET)
  if (PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUSB QUIET libusb-1.0)
  endif()

  if (LIBUSB_FOUND)
    add_library(libusb::libusb UNKNOWN IMPORTED)
    set_target_properties(libusb::libusb PROPERTIES
      IMPORTED_LOCATION "${LIBUSB_LINK_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIRS}"
    )
    set(LIBERA_LIBUSB_FOUND TRUE)
  else()
    find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
    if (LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
      add_library(libusb::libusb UNKNOWN IMPORTED)
      set_target_properties(libusb::libusb PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}"
      )
      set(LIBERA_LIBUSB_FOUND TRUE)
    endif()
  endif()
endif()

if (NOT LIBERA_LIBUSB_FOUND)
  if (NOT EXISTS "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb.h")
    message(FATAL_ERROR "Bundled libusb not found. Did you init the helios_dac submodule?")
  endif()

  if (APPLE)
    set(LIBUSB_BUNDLED_LIB "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/macOS/libusb-1.0.0.dylib")
  elseif (UNIX)
    set(LIBUSB_BUNDLED_LIB "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/Linux x64/libusb-1.0.so")
  elseif (WIN32)
    set(LIBUSB_BUNDLED_LIB "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/Windows/libusb-1.0.lib")
  endif()

  if (NOT EXISTS "${LIBUSB_BUNDLED_LIB}")
    message(FATAL_ERROR "Bundled libusb binary not found at ${LIBUSB_BUNDLED_LIB}")
  endif()

  add_library(libusb::libusb UNKNOWN IMPORTED)
  set_target_properties(libusb::libusb PROPERTIES
    IMPORTED_LOCATION "${LIBUSB_BUNDLED_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBERA_BUNDLED_LIBUSB_DIR}"
  )

  if (APPLE)
    set(CMAKE_BUILD_RPATH "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/macOS")
  endif()
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp>
)

# Helios SDK sources (USB + IDN).
set(HELIOS_SDK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/HeliosDac.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/idn/idn.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/helios_dac/sdk/cpp/idn/idnServerList.cpp"
)
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
if (MSVC)
  target_compile_options(helios-sdk PRIVATE /W0)
else()
  target_compile_options(helios-sdk PRIVATE -w)
endif()
target_sources(libera-core PRIVATE $<TARGET_OBJECTS:helios-sdk>)

target_link_libraries(libera-core PUBLIC libusb::libusb)

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
