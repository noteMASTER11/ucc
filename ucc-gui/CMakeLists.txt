# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required( VERSION 3.20 )

project( ucc-gui
  VERSION ${CMAKE_PROJECT_VERSION}
  DESCRIPTION "Uniwill Control Center GUI application"
  LANGUAGES CXX
)

set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

set( CMAKE_AUTOMOC ON )
set( CMAKE_AUTORCC ON )
set( CMAKE_AUTOUIC ON )

# Find Qt6
find_package( Qt6 REQUIRED COMPONENTS
  Core
  Gui
  Widgets
  DBus
  Bluetooth
  Charts
)

# Application sources
set( UCC_GUI_SOURCES
  src/main.cpp
  src/GuiInstance.cpp
  inc/GuiInstance.hpp
  src/MainWindow.cpp
  inc/MainWindow.hpp
  src/FanControlTab.cpp
  inc/FanControlTab.hpp
  src/DashboardTab.cpp
  inc/DashboardTab.hpp
  src/HardwareTab.cpp
  inc/HardwareTab.hpp
  src/LCTWaterCoolerController.cpp
  inc/LCTWaterCoolerController.hpp
  src/KeyboardTab.cpp
  src/ProfileManager.cpp
  inc/ProfileManager.hpp
  src/SystemMonitor.cpp
  inc/SystemMonitor.hpp
  src/FanCurveEditorWidget.cpp
  inc/FanCurveEditorWidget.hpp
  src/PumpCurveEditorWidget.cpp
  inc/PumpCurveEditorWidget.hpp
  src/KeyboardVisualizerWidget.cpp
  inc/KeyboardVisualizerWidget.hpp
  src/DetectLayout.cpp
  inc/DetectLayout.hpp
  src/AssignKeyZones.cpp
  inc/AssignKeyZones.hpp
  src/MonitorTab.cpp
  inc/MonitorTab.hpp
)

set( UCC_GUI_LIBRARIES
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::DBus
  Qt6::Bluetooth
  Qt6::Charts
  ucc-dbus
)

add_executable( ucc-gui
  ${UCC_GUI_SOURCES}
)

target_include_directories( ucc-gui PRIVATE
  inc/
  ${CMAKE_SOURCE_DIR}/../libucc-dbus
)

target_link_libraries( ucc-gui PRIVATE
  ${UCC_GUI_LIBRARIES}
)

target_compile_options( ucc-gui PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

# Installation
install( TARGETS ucc-gui
  RUNTIME DESTINATION bin
)

# Install desktop file so packaging systems and desktop environments pick it up
install( FILES ucc-gui.desktop DESTINATION share/applications )

# Custom run target
add_custom_target( run
  COMMAND ${CMAKE_CURRENT_BINARY_DIR}/ucc-gui
  DEPENDS ucc-gui
  COMMENT "Running UCC GUI application..."
)
