if (NOT TARGET imgui_lib)
    include(FetchContent)

    # ---- GLFW (windowing) ----
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)

    # ---- Dear ImGui (UI) ----
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.8
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)

    # Build ImGui as a library with GLFW + OpenGL3 backends
    add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui_lib PUBLIC glfw)

    # OpenGL
    if (APPLE)
        find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
        target_link_libraries(imgui_lib PUBLIC ${OPENGL_FRAMEWORK})
    elseif (WIN32)
        target_link_libraries(imgui_lib PUBLIC opengl32)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(imgui_lib PUBLIC OpenGL::GL)
    endif()
endif()

# ---- Laser Tool executable ----
add_executable(laser-tool
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ILDAParser.cpp
)

# Copy patterns folder next to the executable
file(GLOB ILDA_PATTERNS "${CMAKE_CURRENT_SOURCE_DIR}/patterns/*.ild")
foreach(PATTERN_FILE ${ILDA_PATTERNS})
    get_filename_component(PATTERN_NAME ${PATTERN_FILE} NAME)
    configure_file(${PATTERN_FILE}
        ${CMAKE_CURRENT_BINARY_DIR}/patterns/${PATTERN_NAME} COPYONLY)
endforeach()
target_include_directories(laser-tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(laser-tool PRIVATE libera-core imgui_lib)
libera_apply_warnings(laser-tool)
libera_apply_sanitizers(laser-tool)
