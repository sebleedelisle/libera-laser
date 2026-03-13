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
