# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required( VERSION 3.20 )

project( ucc
  VERSION 0.2.0
  DESCRIPTION "Uniwill Control Center - Qt/KDE application suite"
  LANGUAGES CXX
)

# ---------- Version / release tag -------------------------------------------
# Detect short git commit hash; used as the RELEASE component in version.h.
find_package( Git QUIET )
if( GIT_FOUND )
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE UCC_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_result
  )
  if( NOT _git_result EQUAL 0 )
    set( UCC_GIT_HASH "" )
  endif()
else()
  set( UCC_GIT_HASH "" )
endif()

if( UCC_GIT_HASH )
  set( UCC_VERSION_FULL "${PROJECT_VERSION}+${UCC_GIT_HASH}" )
else()
  set( UCC_VERSION_FULL "${PROJECT_VERSION}" )
endif()

configure_file(
  "${CMAKE_SOURCE_DIR}/include/version.h.in"
  "${CMAKE_BINARY_DIR}/include/version.h"
  @ONLY
)

# C++ Standard
set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

# Build type
if( NOT CMAKE_BUILD_TYPE )
  set( CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE )
endif()

# Output directories (avoid absolute paths like /plasma/... during build)
set( CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" )
set( CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" )
set( CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" )

# Compiler flags
add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
)

if(CMAKE_BUILD_TYPE STREQUAL "DEBUG")
  add_compile_options(-O0 -g3)
elseif(CMAKE_BUILD_TYPE STREQUAL "RELEASE")
  add_compile_options(-O3)
elseif(CMAKE_BUILD_TYPE STREQUAL "RELWITHDEBINFO")
  add_compile_options(-O2 -g)
endif()

# Find dependencies
find_package( Qt6 REQUIRED COMPONENTS
  Core
  DBus
  Gui
  Widgets
  Qml
  Quick
  QuickControls2
)

include( GNUInstallDirs )
set( CMAKE_AUTOMOC ON )

# Options
option( BUILD_GUI "Build the main GUI application" ON )
option( BUILD_CLI "Build the command-line interface" ON )
option( BUILD_TRAY "Build the system tray applet" ON )
option( BUILD_GNOME "Install the GNOME Shell extension" ON )
option( BUILD_TESTS "Build the unit test suite" OFF )

if( BUILD_TRAY )
  find_package( ECM REQUIRED NO_MODULE )
  set( CMAKE_MODULE_PATH ${ECM_MODULE_PATH} )
  include( KDEInstallDirs )
  include( KDECMakeSettings )
  include( ECMQmlModule )
  find_package( KF6Config REQUIRED )
  find_package( KF6CoreAddons REQUIRED )
  find_package( Plasma REQUIRED )
endif()

# Conditionally find Qt6::Test if tests are enabled
if( BUILD_TESTS )
  find_package( Qt6 REQUIRED COMPONENTS Test )
endif()

# Include directories (source headers + generated version.h)
include_directories( ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/include )


# Subdirectories
add_subdirectory( uccd )
add_subdirectory( libucc-dbus )

if( BUILD_GUI )
  add_subdirectory( ucc-gui )
endif()

if( BUILD_CLI )
  add_subdirectory( ucc-cli )
endif()

if( BUILD_TRAY )
  add_subdirectory( ucc-tray )
endif()

if( BUILD_TESTS )
  enable_testing()
  add_subdirectory( tests )
endif()

# Install GNOME Shell extension
if( BUILD_GNOME )
  set( _gnome_ext_dir "share/gnome-shell/extensions/ucc@uniwill.com" )
  install( FILES
    ucc-gnome/metadata.json
    ucc-gnome/extension.js
    ucc-gnome/uccdClient.js
    ucc-gnome/stylesheet.css
    DESTINATION ${_gnome_ext_dir}
  )
endif()

# Icon install (all sizes)
install(FILES
  icons/ucc-gui.svg
  icons/ucc-gui_16.svg
  icons/ucc-gui_24.svg
  icons/ucc-gui_32.svg
  icons/ucc-gui_48.svg
  icons/ucc-gui_64.svg
  icons/ucc-gui_128.svg
  icons/ucc-tray.svg
  icons/ucc-tray_16.svg
  icons/ucc-tray_24.svg
  icons/ucc-tray_32.svg
  icons/ucc-tray_48.svg
  icons/ucc-tray_64.svg
  icons/ucc-tray_128.svg
  DESTINATION share/icons/hicolor/scalable/apps
)

# Install PNGs to correct hicolor subdirectories
install(FILES icons/ucc-gui_16.png   DESTINATION share/icons/hicolor/16x16/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_24.png   DESTINATION share/icons/hicolor/24x24/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_32.png   DESTINATION share/icons/hicolor/32x32/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_48.png   DESTINATION share/icons/hicolor/48x48/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_64.png   DESTINATION share/icons/hicolor/64x64/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_128.png  DESTINATION share/icons/hicolor/128x128/apps RENAME ucc-gui.png)
install(FILES icons/ucc-tray_16.png  DESTINATION share/icons/hicolor/16x16/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_24.png  DESTINATION share/icons/hicolor/24x24/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_32.png  DESTINATION share/icons/hicolor/32x32/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_48.png  DESTINATION share/icons/hicolor/48x48/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_64.png  DESTINATION share/icons/hicolor/64x64/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_128.png DESTINATION share/icons/hicolor/128x128/apps RENAME ucc-tray.png)

# Install common headers
# install(FILES include/CommonTypes.hpp DESTINATION include/ucc)

# Summary
message(STATUS "")
message(STATUS "=== Uniwill Control Center Build Configuration ===")
message(STATUS "  Version:          ${UCC_VERSION_FULL}")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Qt Version:       ${Qt6_VERSION}")
message(STATUS "")
message(STATUS "  Components:")
message(STATUS "    DBus Library:   ON")
message(STATUS "    GUI App:        ${BUILD_GUI}")
message(STATUS "    CLI Tool:       ${BUILD_CLI}")
message(STATUS "    Tray Applet:    ${BUILD_TRAY}")
message(STATUS "    GNOME Ext:      ${BUILD_GNOME}")
message(STATUS "    Unit Tests:     ${BUILD_TESTS}")
message(STATUS "")
message(STATUS "  Install prefix:   ${CMAKE_INSTALL_PREFIX}")
message(STATUS "===================================================")
message(STATUS "")
