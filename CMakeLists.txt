cmake_minimum_required(VERSION 3.20)
project(QuestLHSync VERSION 1.28 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENXR openxr)

# Common IPC library (Ticket 02)
add_library(qlhs_common STATIC
  src/common/qlhs_shm.cpp
)
target_include_directories(qlhs_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common
)
target_link_libraries(qlhs_common PUBLIC Threads::Threads)

# Core solver and network client (Ticket 01)
add_library(qlhs_core STATIC
  src/driver/sync.cpp
  src/driver/net.cpp
  src/driver/relations.cpp
)
target_include_directories(qlhs_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/driver
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common
)
target_link_libraries(qlhs_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Optional preload library to hook driver_lighthouse.so inside wivrn-server / vrserver
add_library(qlhs_relations SHARED
  src/driver/relations_preload.cpp
  src/driver/relations.cpp
)
target_include_directories(qlhs_relations PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/driver
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common
)
target_link_libraries(qlhs_relations PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# Monado / OpenXR adapter library (Ticket 03)
add_library(qlhs_driver STATIC
  src/driver/monado_adapter.cpp
  src/driver/openxr_locator.cpp
)
target_include_directories(qlhs_driver PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OPENXR_INCLUDE_DIRS}
)
target_link_libraries(qlhs_driver PUBLIC
  Threads::Threads
  ${CMAKE_DL_LIBS}
  ${OPENXR_LIBRARIES}
  m
)

# Daemon coordinator library and daemon executable (Ticket 04)
add_library(qlhs_daemon STATIC
  src/daemon/daemon_coordinator.cpp
)
target_include_directories(qlhs_daemon PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OPENXR_INCLUDE_DIRS}
)
target_link_libraries(qlhs_daemon PUBLIC
  qlhs_common
  qlhs_core
  qlhs_driver
)

add_executable(questlhsyncd
  src/daemon/daemon_main.cpp
)
target_link_libraries(questlhsyncd PRIVATE qlhs_daemon)

# Dear ImGui & GLFW & OpenGL3 (Ticket 05)
find_package(OpenGL REQUIRED)
find_package(glfw3 QUIET)
if(NOT TARGET glfw)
  pkg_check_modules(GLFW REQUIRED glfw3)
endif()

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.8
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  if(TARGET glfw)
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
  else()
    target_include_directories(imgui PUBLIC ${GLFW_INCLUDE_DIRS})
    target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES} OpenGL::GL)
  endif()
endif()

# GUI and Unified Launcher (Ticket 05)
add_library(qlhs_gui STATIC
  src/gui/dashboard.cpp
  src/gui/launcher.cpp
)
target_include_directories(qlhs_gui PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common
)
target_link_libraries(qlhs_gui PUBLIC
  imgui
  qlhs_common
  qlhs_daemon
)

add_executable(questlhsync
  src/gui/gui_main.cpp
)
target_link_libraries(questlhsync PRIVATE qlhs_gui)

# Testing
enable_testing()

add_executable(test_gui tests/test_gui.cpp)
target_link_libraries(test_gui PRIVATE qlhs_gui)
add_test(NAME test_gui COMMAND test_gui)

add_executable(test_daemon tests/test_daemon.cpp)
target_link_libraries(test_daemon PRIVATE qlhs_daemon)
add_test(NAME test_daemon COMMAND test_daemon)

add_executable(test_solver tests/test_solver.cpp)
target_link_libraries(test_solver PRIVATE qlhs_core)
add_test(NAME test_solver COMMAND test_solver)

add_executable(test_layout_correction tests/test_layout_correction.cpp)
target_link_libraries(test_layout_correction PRIVATE qlhs_core)
add_test(NAME test_layout_correction COMMAND test_layout_correction)

add_executable(test_net tests/test_net.cpp)
target_link_libraries(test_net PRIVATE qlhs_core)
add_test(NAME test_net COMMAND test_net)

add_executable(test_shm tests/test_shm.cpp)
target_link_libraries(test_shm PRIVATE qlhs_common)
add_test(NAME test_shm COMMAND test_shm)

add_executable(test_monado_adapter tests/test_monado_adapter.cpp)
target_link_libraries(test_monado_adapter PRIVATE qlhs_driver)
add_test(NAME test_monado_adapter COMMAND test_monado_adapter)

set_tests_properties(test_gui test_daemon test_solver test_net test_shm test_monado_adapter PROPERTIES TIMEOUT 10)

# Offline replay tool
add_executable(qlhs_replay src/tools/replay.cpp)
target_link_libraries(qlhs_replay PRIVATE qlhs_core)
