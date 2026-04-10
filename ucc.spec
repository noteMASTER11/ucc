%global _hardened_build 1

Name:           ucc
Version:        0.2.0
Release:        1%{?dist}
Summary:        Uniwill Control Center - System control application suite
License:        GPL-3.0-or-later

URL:            https://github.com/tuxedocomputers/uniwill-control-center
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  extra-cmake-modules
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel >= 6.0
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtconnectivity-devel
BuildRequires:  qt6-qtcharts-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kpackage-devel
BuildRequires:  kf6-kwindowsystem-devel
BuildRequires:  libplasma-devel >= 6.0
BuildRequires:  systemd-devel
BuildRequires:  libXrandr-devel
BuildRequires:  libgudev-devel

Requires:       systemd
Requires:       qt6-qtbase >= 6.0
Requires:       qt6-qtdeclarative
Requires:       qt6-qtconnectivity
Requires:       qt6-qtcharts
Requires:       plasma-workspace >= 6.0
Requires:       polkit
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
Recommends:     tuxedo-drivers

%description
Uniwill Control Center (UCC) is a comprehensive system control application 
suite for Uniwill computers. It provides:

- Daemon (uccd): Background service for system control and monitoring
- GUI (ucc-gui): Main graphical user interface
- CLI (ucc-cli): Command-line interface for scripting and headless use
- Plasma Applet: KDE system tray applet for quick access

Features include fan control, keyboard backlight management, display settings,
CPU power management, and water cooler control for supported systems.

%prep
%autosetup

%build
%cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=%{_libdir} \
  -DCMAKE_INSTALL_RPATH="" \
  -DBUILD_GUI=ON \
  -DBUILD_TRAY=ON \
  -DBUILD_GNOME=ON \
  -DBUILD_CLI=ON
%cmake_build -j

%install
%cmake_install

# Create configuration directory
mkdir -p %{buildroot}%{_sysconfdir}/ucc

# Set permissions
chmod 755 %{buildroot}%{_sysconfdir}/ucc

%post
%systemd_post uccd.service
systemctl daemon-reload > /dev/null 2>&1 || true

%preun
%systemd_preun uccd.service

%postun
%systemd_postun_with_restart uccd.service

%files
%license LICENSE
%doc README.md
%{_bindir}/uccd
%{_bindir}/ucc-gui
%{_bindir}/ucc-cli
%{_libdir}/libucc-dbus.so*
# libucc-declarative.so is installed twice by cmake (KDE_INSTALL_TARGETS_DEFAULT_ARGS
# and ecm_finalize_qml_module). Only the QML module copy is needed.
%exclude %{_libdir}/libucc-declarative.so
%{_libdir}/qt6/plugins/plasma/applets/com.uniwill.ucc.so
%{_libdir}/qt6/qml/com/uniwill/ucc/private/libucc-declarative.so
%{_libdir}/qt6/qml/com/uniwill/ucc/private/qmldir
%{_libdir}/qt6/qml/com/uniwill/ucc/private/ucc-declarative.qmltypes
%{_libdir}/qt6/qml/com/uniwill/ucc/private/kde-qmlmodule.version
%{_unitdir}/uccd.service
%{_unitdir}/uccd-sleep.service
%{_datadir}/dbus-1/system-services/com.uniwill.uccd.service
%{_datadir}/dbus-1/system.d/com.uniwill.uccd.conf
%{_datadir}/polkit-1/actions/com.uniwill.uccd.policy
%dir %{_sysconfdir}/ucc
%{_datadir}/applications/ucc-gui.desktop
# Icons (SVG and PNG, all sizes)
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_16.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_24.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_32.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_48.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_64.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_128.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_16.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_24.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_32.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_48.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_64.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_128.svg
%{_datadir}/icons/hicolor/16x16/apps/ucc-gui.png
%{_datadir}/icons/hicolor/24x24/apps/ucc-gui.png
%{_datadir}/icons/hicolor/32x32/apps/ucc-gui.png
%{_datadir}/icons/hicolor/48x48/apps/ucc-gui.png
%{_datadir}/icons/hicolor/64x64/apps/ucc-gui.png
%{_datadir}/icons/hicolor/128x128/apps/ucc-gui.png
%{_datadir}/icons/hicolor/16x16/apps/ucc-tray.png
%{_datadir}/icons/hicolor/24x24/apps/ucc-tray.png
%{_datadir}/icons/hicolor/32x32/apps/ucc-tray.png
%{_datadir}/icons/hicolor/48x48/apps/ucc-tray.png
%{_datadir}/icons/hicolor/64x64/apps/ucc-tray.png
%{_datadir}/icons/hicolor/128x128/apps/ucc-tray.png
%{_mandir}/man1/ucc-cli.1*
%{_datadir}/bash-completion/completions/ucc-cli
# GNOME Shell extension
%{_datadir}/gnome-shell/extensions/ucc@uniwill.com/metadata.json
%{_datadir}/gnome-shell/extensions/ucc@uniwill.com/extension.js
%{_datadir}/gnome-shell/extensions/ucc@uniwill.com/uccdClient.js
%{_datadir}/gnome-shell/extensions/ucc@uniwill.com/stylesheet.css
