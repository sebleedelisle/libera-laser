set(LIBERA_UNIT_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_buffer_estimator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_controller_cache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_controller_status.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_content_source_switch.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_request.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_scheduler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_advance.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_projected_render_time.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_queue_budget_floor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_single_controller_manager_base.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_controller_driver_selection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_avb_backend_state.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_helios_timing.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lasercubenet_buffer_target.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lasercubenet_point_rate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_status.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_buffer_target.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_buffer_projection.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_encoding.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_byte_buffer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_command.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lightspacenet_packet.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lasercubenet_status.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_frame_transition.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_core_scanner_sync.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_point_stream_framer.cpp
)

set(LIBERA_INTEGRATION_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_begin_timeout_reconnect.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_firmware_probe.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_reconnect.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_stream_order.cpp
)

set(LIBERA_HARDWARE_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_packet_latency.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_protocol_probe.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_underflow_probe.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_helios_usb_stream.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/test_lightspacenet_stream.cpp
)

if (LIBERA_ENABLE_PLUGINS)
  list(APPEND LIBERA_UNIT_TESTS
    ${CMAKE_CURRENT_SOURCE_DIR}/test_core_plugin_directory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_management.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_package.cpp)
endif()

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

if (LIBERA_ENABLE_PLUGINS)
  target_link_libraries(test_plugin_package PRIVATE libera-miniz)
  target_link_libraries(test_plugin_management PRIVATE libera-miniz)

  add_library(test_valid_libera_plugin SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin_fixtures/valid_plugin.cpp)
  add_library(test_missing_transport_libera_plugin SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin_fixtures/missing_transport_plugin.cpp)

  foreach(plugin_fixture IN ITEMS test_valid_libera_plugin test_missing_transport_libera_plugin)
    target_include_directories(${plugin_fixture} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    libera_apply_warnings(${plugin_fixture})
    libera_apply_sanitizers(${plugin_fixture})
    set_target_properties(${plugin_fixture} PROPERTIES
      PREFIX ""
      WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endforeach()

  set_target_properties(test_valid_libera_plugin PROPERTIES
    OUTPUT_NAME "test-valid-libera-plugin")
  set_target_properties(test_missing_transport_libera_plugin PROPERTIES
    OUTPUT_NAME "test-missing-transport-libera-plugin")

  add_dependencies(test_plugin_management
    test_valid_libera_plugin
    test_missing_transport_libera_plugin)
  target_compile_definitions(test_plugin_management PRIVATE
    TEST_VALID_PLUGIN_PATH="$<TARGET_FILE:test_valid_libera_plugin>"
    TEST_MISSING_TRANSPORT_PLUGIN_PATH="$<TARGET_FILE:test_missing_transport_libera_plugin>")
endif()

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
