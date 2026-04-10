# Maintainer: nanomatters <nanomatters@github.com>

pkgname=ucc
pkgver=0.2.0
pkgrel=1
pkgdesc='Uniwill Control Center - System daemon, GUI, CLI tool and Plasma applet for Uniwill laptops'
arch=('x86_64')
url='https://github.com/nanomatters/ucc'
license=('GPL-3.0-or-later')

# --- Runtime dependencies ---
# qt6-base:          QtCore, QtGui, QtWidgets, QtDBus (GUI, shared lib, daemon)
# qt6-connectivity:  QtBluetooth (daemon BT scanning, GUI BT display)
# qt6-declarative:   QtQml, QtQuick (required for Plasma applet QML module)
# plasma-workspace:  Plasma shell (hosts the applet)
# systemd-libs:      libsystemd / libudev (uccd links udev)
# hicolor-icon-theme: icon theme spec directory structure
# dbus:              system bus for uccd D-Bus service
depends=(
  'qt6-base'
  'qt6-charts'
  'qt6-connectivity'
  'qt6-declarative'
  'plasma-workspace'
  'systemd-libs'
  'hicolor-icon-theme'
  'dbus'
  'polkit'
)

# --- Build-time dependencies ---
# cmake:               build system (>= 3.20 required by CMakeLists)
# extra-cmake-modules: ECM for KDEInstallDirs / KDECMakeSettings / ECMQmlModule
# ninja:               faster parallel builds
# pkgconf:             pkg-config used by uccd/CMakeLists (find_package(PkgConfig))
# plasma-workspace:    PlasmaMacros for plasma_add_applet
# kconfig:             KF6Config (Plasma link dependency)
# kcoreaddons:         KF6CoreAddons (Plasma link dependency)
makedepends=(
  'cmake'
  'extra-cmake-modules'
  'ninja'
  'nlohmann-json'
  'pkgconf'
  'plasma-workspace'
  'kconfig'
  'kcoreaddons'
  'kpackage'
  'kwindowsystem'
)

# tuxedo-drivers provides the /dev/tuxedo_io kernel interface for hardware control
optdepends=(
  'tuxedo-drivers: kernel module providing /dev/tuxedo_io hardware interface for hardware control'
)

# Preserve user-modified configuration across upgrades
backup=('etc/ucc/settings')

install=ucc.install

source=("${pkgname}-${pkgver}.tar.gz::${url}/archive/v${pkgver}.tar.gz")
sha256sums=('SKIP')

build() {
  local cmake_options=(
    -B build
    -S "${pkgname}-${pkgver}"
    -G Ninja
    -Wno-dev
    -DCMAKE_BUILD_TYPE=None
    -DCMAKE_INSTALL_PREFIX=/usr
    -DCMAKE_INSTALL_LIBDIR=lib
    -DBUILD_GUI=ON
    -DBUILD_TRAY=ON
    -DBUILD_GNOME=ON
    -DBUILD_CLI=ON
  )
  cmake "${cmake_options[@]}"
  cmake --build build
}

package() {
  DESTDIR="${pkgdir}" cmake --install build

  # Ensure the configuration directory exists
  install -dm755 "${pkgdir}/etc/ucc"
}
