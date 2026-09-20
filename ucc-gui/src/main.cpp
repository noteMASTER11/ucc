/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QTextStream>
#include <QTimer>
#include <cstdlib>
#include "MainWindow.hpp"
#include "GuiInstance.hpp"
#include "SystemMonitor.hpp"
#include "UccdClient.hpp"
#include "version.h"

int main( int argc, char *argv[] )
{
  int displayCount = 0;
  for ( int i = 1; i < argc; ++i )
  {

    if ( std::string( argv[i] ) == "--display" && i + 1 < argc )
    {
      displayCount = static_cast< int >( std::strtol( argv[i + 1], nullptr, 10 ) );
      break;
    }
  }

  if ( displayCount > 0 )
  {
    QCoreApplication app( argc, argv );
    app.setOrganizationName( "UniwillControlCenter" );
    app.setOrganizationDomain( "uniwill.local" );
    app.setApplicationName( "ucc-gui" );
    app.setApplicationVersion( UCC_VERSION_FULL );

    ucc::SystemMonitor monitor;
    QTextStream out( stdout );

    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(4000);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&app] {
      QTextStream(stderr) << "Timed out waiting for UCC metrics" << Qt::endl;
      app.exit(2);
    });
    int received = 0;
    QObject::connect(&monitor, &ucc::SystemMonitor::metricsUpdated, &app, [&] {
      out << "cpu_temp=" << monitor.cpuTemp()
          << " cpu_freq=" << monitor.cpuFrequency()
          << " cpu_power=" << monitor.cpuPower()
          << " cpu_fan=" << monitor.cpuFanSpeed()
          << " gpu_temp=" << monitor.gpuTemp()
          << " gpu_freq=" << monitor.gpuFrequency()
          << " gpu_power=" << monitor.gpuPower()
          << " gpu_fan=" << monitor.gpuFanSpeed() << Qt::endl;
      if (++received >= displayCount) app.quit();
      else watchdog.start();
    });
    monitor.setMonitoringActive(true);
    watchdog.start();
    monitor.refreshAll();
    return app.exec();
  }

  QApplication app( argc, argv );
  app.setDesktopFileName("ucc-gui");
  app.setOrganizationName( "UniwillControlCenter" );
  app.setOrganizationDomain( "uniwill.local" );
  app.setApplicationName( "ucc-gui" );
  app.setApplicationVersion( UCC_VERSION_FULL );

  ucc::GuiInstance instance;
  const auto instanceResult = instance.startOrActivate();
  if (instanceResult != ucc::GuiInstance::Result::Primary)
    return instanceResult == ucc::GuiInstance::Result::ActivatedExisting ? 0 : 1;

  // ensure window decorations and the application use the theme icon we installed
  app.setWindowIcon( QIcon::fromTheme( "ucc-gui" ) );

  {
    ucc::UccdClient client;
    auto supported = client.isDeviceSupported();
    if ( !supported.has_value() || !supported.value() )
    {
      QMessageBox::critical(
        nullptr, "Unsupported Device",
        "This laptop model is not supported by UCC.\n\n"
        "Running UCC on untested hardware may cause\n"
        "unexpected behavior, even damage." );
      return 1;
    }
  }

  ucc::MainWindow window;
  instance.setWindow(&window);
  window.show();

  return app.exec();
}
