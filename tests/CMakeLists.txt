set(LIBERA_UNIT_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_buffer_estimator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_controller_cache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_controller_status.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_content_source_switch.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_request.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_advance.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_projected_render_time.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_single_controller_manager_base.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_avb_backend_state.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_helios_timing.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lasercubenet_buffer_target.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lasercubenet_point_rate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_status.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_encoding.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_byte_buffer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_command.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lasercubenet_status.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_transition.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_scanner_sync.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_point_stream_framer.cpp
)

set(LIBERA_INTEGRATION_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_reconnect.cpp
)

set(LIBERA_HARDWARE_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_packet_latency.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_helios_usb_stream.cpp
)

foreach(test_source IN LISTS LIBERA_UNIT_TESTS)
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE libera-core)
  libera_apply_warnings(${test_name})
  libera_apply_sanitizers(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
  if (WIN32 AND LIBERA_BUNDLED_LIBUSB_DLL_DIR_RELEASE)
    add_custom_command(TARGET ${test_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${LIBERA_BUNDLED_LIBUSB_DLL_DIR_DEBUG},${LIBERA_BUNDLED_LIBUSB_DLL_DIR_RELEASE}>/libusb-1.0.dll"
        $<TARGET_FILE_DIR:${test_name}>
    )
  endif()
endforeach()

if (LIBERA_BUILD_INTEGRATION_TESTS)
  foreach(test_source IN LISTS LIBERA_INTEGRATION_TESTS)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE libera-core)
    libera_apply_warnings(${test_name})
    libera_apply_sanitizers(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "integration")
  endforeach()
endif()

if (LIBERA_BUILD_HARDWARE_TESTS)
  foreach(test_source IN LISTS LIBERA_HARDWARE_TESTS)
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE libera-core)
    libera_apply_warnings(${test_name})
    libera_apply_sanitizers(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "hardware")
  endforeach()
endif()
