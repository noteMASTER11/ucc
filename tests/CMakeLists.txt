# SPDX-License-Identifier: GPL-3.0-or-later
# Unit test suite — uses Qt Test (Qt6::Test)

set( CMAKE_AUTOMOC ON )

# uccd headers needed by most tests
set( UCCD_INC_DIR "${CMAKE_SOURCE_DIR}/uccd/inc" )

# nlohmann/json lives in uccd/3rdparty
set( UCCD_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/uccd/3rdparty" )

# ---------- helper function -------------------------------------------------
# ucc_add_test( <name> <source> [LINK_LIBS lib1 lib2 ...] )
#
# Creates an executable, links Qt6::Test + any extra libs, registers it with
# CTest, and adds the uccd include paths automatically.
function( ucc_add_test TEST_NAME TEST_SOURCE )
  cmake_parse_arguments( ARG "" "" "LINK_LIBS" ${ARGN} )

  add_executable( ${TEST_NAME} ${TEST_SOURCE} )

  target_include_directories( ${TEST_NAME} PRIVATE
    ${UCCD_INC_DIR}
    ${UCCD_3RDPARTY_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
  )

  target_link_libraries( ${TEST_NAME} PRIVATE
    Qt6::Core
    Qt6::Test
    ${ARG_LINK_LIBS}
  )

  add_test( NAME ${TEST_NAME} COMMAND ${TEST_NAME} )
endfunction()

# ---------- tests -----------------------------------------------------------

ucc_add_test( test_state_utils      test_state_utils.cpp )
ucc_add_test( test_fan_profile      test_fan_profile.cpp )
ucc_add_test( test_profile_manager  test_profile_manager.cpp )
ucc_add_test( test_settings_manager test_settings_manager.cpp )
ucc_add_test( test_metrics_history  test_metrics_history.cpp )
ucc_add_test( test_aquaris_selection test_aquaris_selection.cpp )
target_compile_options(test_aquaris_selection PRIVATE -UNDEBUG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_device_identification COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_device_identification.py)
add_test(NAME test_bluetooth_retry COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_bluetooth_retry.py)
add_test(NAME test_bluetooth_startup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_bluetooth_startup.py)
add_test(NAME test_display_identity COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_display_identity.py)
find_program(GJS_EXECUTABLE gjs)
find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
find_program(NODE_EXECUTABLE node)
if(GJS_EXECUTABLE AND DBUS_RUN_SESSION_EXECUTABLE)
  add_test(NAME test_gnome_async COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- bash ${CMAKE_CURRENT_SOURCE_DIR}/run_gnome_async_test.sh ${GJS_EXECUTABLE})
endif()
if(NODE_EXECUTABLE)
  add_test(NAME test_gnome_launch COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gnome_launch.cjs)
  add_test(NAME test_gnome_refresh COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gnome_refresh.cjs)
endif()
if(DBUS_RUN_SESSION_EXECUTABLE AND BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Charts Bluetooth)
  add_executable(test_gui_audit test_gui_audit.cpp
    ../ucc-gui/src/FanControlTab.cpp ../ucc-gui/inc/FanControlTab.hpp
    ../ucc-gui/src/FanCurveEditorWidget.cpp ../ucc-gui/inc/FanCurveEditorWidget.hpp
    ../ucc-gui/src/PumpCurveEditorWidget.cpp ../ucc-gui/inc/PumpCurveEditorWidget.hpp
    ../ucc-gui/src/ProfileManager.cpp ../ucc-gui/inc/ProfileManager.hpp
    ../ucc-gui/src/SystemMonitor.cpp ../ucc-gui/inc/SystemMonitor.hpp
    ../ucc-gui/src/DashboardTab.cpp ../ucc-gui/inc/DashboardTab.hpp
    ../ucc-gui/src/MonitorTab.cpp ../ucc-gui/inc/MonitorTab.hpp)
  target_include_directories(test_gui_audit PRIVATE ../ucc-gui/inc ../include)
  target_link_libraries(test_gui_audit PRIVATE ucc-dbus Qt6::Widgets Qt6::Charts Qt6::Bluetooth Qt6::Test)
  add_test(NAME test_gui_audit COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- bash ${CMAKE_CURRENT_SOURCE_DIR}/run_gui_audit_test.sh $<TARGET_FILE:test_gui_audit>)
  add_executable(test_gui_instance test_gui_instance.cpp
    ../ucc-gui/src/GuiInstance.cpp ../ucc-gui/inc/GuiInstance.hpp)
  target_include_directories(test_gui_instance PRIVATE ../ucc-gui/inc)
  target_link_libraries(test_gui_instance PRIVATE Qt6::Widgets Qt6::DBus Qt6::Test)
  add_test(NAME test_gui_instance COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:test_gui_instance>)
  set_tests_properties(test_gui_instance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  add_executable(test_system_monitor test_system_monitor.cpp
    ../ucc-gui/src/SystemMonitor.cpp ../ucc-gui/inc/SystemMonitor.hpp)
  add_dependencies(test_system_monitor ucc-gui)
  target_include_directories(test_system_monitor PRIVATE ../ucc-gui/inc ../include)
  target_link_libraries(test_system_monitor PRIVATE ucc-dbus Qt6::Core Qt6::Test)
  add_test(NAME test_system_monitor COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- bash ${CMAKE_CURRENT_SOURCE_DIR}/run_private_system_bus.sh $<TARGET_FILE:test_system_monitor>)
endif()
