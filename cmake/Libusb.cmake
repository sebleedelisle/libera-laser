function(libera_configure_libusb)
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
      set(LIBUSB_BUNDLED_LIB_DEBUG   "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/Windows/x64/Debug/dll/libusb-1.0.lib")
      set(LIBUSB_BUNDLED_LIB_RELEASE "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/Windows/x64/Release/dll/libusb-1.0.lib")
      set(LIBUSB_BUNDLED_LIB "${LIBUSB_BUNDLED_LIB_RELEASE}")
    endif()

    if (NOT EXISTS "${LIBUSB_BUNDLED_LIB}")
      message(FATAL_ERROR "Bundled libusb binary not found at ${LIBUSB_BUNDLED_LIB}")
    endif()

    add_library(libusb::libusb UNKNOWN IMPORTED)
    if (WIN32)
      set_target_properties(libusb::libusb PROPERTIES
        IMPORTED_LOCATION         "${LIBUSB_BUNDLED_LIB_RELEASE}"
        IMPORTED_LOCATION_DEBUG   "${LIBUSB_BUNDLED_LIB_DEBUG}"
        IMPORTED_LOCATION_RELEASE "${LIBUSB_BUNDLED_LIB_RELEASE}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBERA_BUNDLED_LIBUSB_DIR}"
      )
    else()
      set_target_properties(libusb::libusb PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_BUNDLED_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBERA_BUNDLED_LIBUSB_DIR}"
      )
    endif()

    if (APPLE)
      set(CMAKE_BUILD_RPATH "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/macOS")
    elseif (WIN32)
      set(LIBERA_BUNDLED_LIBUSB_DLL_DIR_DEBUG   "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/Windows/x64/Debug/dll"   CACHE INTERNAL "")
      set(LIBERA_BUNDLED_LIBUSB_DLL_DIR_RELEASE "${LIBERA_BUNDLED_LIBUSB_DIR}/libusb_bin/Windows/x64/Release/dll" CACHE INTERNAL "")
    endif()
  endif()

  set(LIBERA_LIBUSB_FOUND ${LIBERA_LIBUSB_FOUND} PARENT_SCOPE)
endfunction()
