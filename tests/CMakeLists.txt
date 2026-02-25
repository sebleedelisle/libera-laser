set(LIBERA_UNIT_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_status.cpp
)

set(LIBERA_INTEGRATION_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_reconnect.cpp
)

set(LIBERA_HARDWARE_TESTS
  ${CMAKE_CURRENT_SOURCE_DIR}/test_etherdream_packet_latency.cpp
)

foreach(test_source IN LISTS LIBERA_UNIT_TESTS)
  get_filename_component(test_name ${test_source} NAME_WE)
  add_executable(${test_name} ${test_source})
  target_link_libraries(${test_name} PRIVATE libera-core)
  libera_apply_warnings(${test_name})
  libera_apply_sanitizers(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
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
