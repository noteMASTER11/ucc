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
#include <QTextStream>
#include <QThread>
#include <cstdlib>
#include "MainWindow.hpp"
#include "SystemMonitor.hpp"
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

    for ( int i = 0; i < displayCount; ++i )
    {
      monitor.refreshAll();
      out << "cpu_temp=" << monitor.cpuTemp()
          << " cpu_freq=" << monitor.cpuFrequency()
          << " cpu_power=" << monitor.cpuPower()
          << " cpu_fan=" << monitor.cpuFanSpeed()
          << " gpu_temp=" << monitor.gpuTemp()
          << " gpu_freq=" << monitor.gpuFrequency()
          << " gpu_power=" << monitor.gpuPower()
          << " gpu_fan=" << monitor.gpuFanSpeed()
          << Qt::endl;

      QCoreApplication::processEvents();

      if ( i < displayCount - 1 )
      {
        QThread::sleep( 1 );
      }
    }

    return 0;
  }

  QApplication app( argc, argv );
  app.setOrganizationName( "UniwillControlCenter" );
  app.setOrganizationDomain( "uniwill.local" );
  app.setApplicationName( "ucc-gui" );
  app.setApplicationVersion( UCC_VERSION_FULL );

  // ensure window decorations and the application use the theme icon we installed
  app.setWindowIcon( QIcon::fromTheme( "ucc-gui" ) );

  ucc::MainWindow window;
  window.show();

  return app.exec();
}
