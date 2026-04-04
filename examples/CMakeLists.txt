add_executable(frame-mode-demo ${CMAKE_CURRENT_SOURCE_DIR}/frame_mode_demo.cpp)
target_link_libraries(frame-mode-demo PRIVATE libera-core)
libera_apply_warnings(frame-mode-demo)
libera_apply_sanitizers(frame-mode-demo)

add_executable(stream-callback-demo ${CMAKE_CURRENT_SOURCE_DIR}/stream_callback_demo.cpp)
target_link_libraries(stream-callback-demo PRIVATE libera-core)
libera_apply_warnings(stream-callback-demo)
libera_apply_sanitizers(stream-callback-demo)

add_executable(simple-frame-demo ${CMAKE_CURRENT_SOURCE_DIR}/simple_frame_demo.cpp)
target_link_libraries(simple-frame-demo PRIVATE libera-core)
libera_apply_warnings(simple-frame-demo)
libera_apply_sanitizers(simple-frame-demo)

add_executable(simple-frame-avb-demo ${CMAKE_CURRENT_SOURCE_DIR}/simple_frame_avb_demo.cpp)
target_link_libraries(simple-frame-avb-demo PRIVATE libera-core)
libera_apply_warnings(simple-frame-avb-demo)
libera_apply_sanitizers(simple-frame-avb-demo)

# Example plugin — built as a shared library to demonstrate the plugin API.
add_library(example-plugin SHARED ${CMAKE_CURRENT_SOURCE_DIR}/example_plugin.cpp)
target_include_directories(example-plugin PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>)
set_target_properties(example-plugin PROPERTIES PREFIX "" OUTPUT_NAME "example-plugin")
libera_apply_warnings(example-plugin)

add_subdirectory(laser_tool)
