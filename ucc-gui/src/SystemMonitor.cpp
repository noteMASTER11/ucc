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

#include "SystemMonitor.hpp"
#include "CommonTypes.hpp"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDBusArgument>

namespace ucc
{

SystemMonitor::SystemMonitor( QObject *parent )
  : QObject( parent )
  , m_client( std::make_unique< UccdClient >( this ) )
  , m_updateTimer( new QTimer( this ) )
{
  // Refresh visible metrics every two seconds; all bus reads are asynchronous.
  connect( m_updateTimer, &QTimer::timeout, this, &SystemMonitor::updateMetrics );
  m_updateTimer->setInterval( 2000 );

  // Load charging capabilities (these don't change at runtime)
  initializeChargingState();

  // Re-init charging capabilities if uccd wasn't running at startup
  connect( m_client.get(), &UccdClient::connectionStatusChanged,
           this, [this]( bool connected ) {
    if ( connected )
    {
      qInfo() << "[SystemMonitor] uccd reconnected — reinitialising charging state";
      initializeChargingState();
      refreshAll();
    }
  } );

  // Timer will be started when monitoring becomes active
}

SystemMonitor::~SystemMonitor() = default;

namespace {
QVariantMap decodeMap(const QVariant &value) {
  return value.canConvert<QDBusArgument>()
    ? qdbus_cast<QVariantMap>(value.value<QDBusArgument>()) : value.toMap();
}
int fanValue(const QVariant &value, const QString &key) {
  const auto inner = decodeMap(decodeMap(value).value(key));
  return inner.value("timestamp").toLongLong() > 0 ? inner.value("data", -1).toInt() : -1;
}
}

void SystemMonitor::updateMetrics()
{
  if (!m_monitoringActive || m_metricsPending) return;
  m_metricsPending = true;
  const auto generation = m_monitorGeneration;
  const bool controls = !m_controlsAge.isValid() || m_controlsAge.elapsed() >= 10000;
  if (controls) m_controlsAge.restart();
  m_client->requestMonitoringSnapshot(controls, [this, generation](const QVariantMap &snapshot) {
    m_metricsPending = false;
    if (!m_monitoringActive || generation != m_monitorGeneration || snapshot.isEmpty()) return;
    const auto json = [&snapshot](const QString &method) {
      return QJsonDocument::fromJson(snapshot.value(method).toString().toUtf8()).object();
    };
    const auto gpu = json("GetDGpuInfoValuesJSON");
    const auto igpu = json("GetIGpuInfoValuesJSON");
    const auto cpu = json("GetCpuPowerValuesJSON");
    const auto numeric = [](const QJsonObject &object, const char *key) {
      const auto value = object.value(QLatin1String(key));
      return value.isDouble() ? value.toDouble() : -1.0;
    };
    const auto fallback = [](double first, double second) { return first >= 0 ? first : second; };
    const auto formatted = [](double value, const QString &unit, int precision = 0) {
      return value < 0 ? QString("--") : QString::number(value, 'f', precision) + unit;
    };
    const auto setText = [this](QString &field, const QString &value, auto changed) {
      if (field != value) { field = value; (this->*changed)(); }
    };
    const auto setInt = [this](int &field, int value, auto changed) {
      if (field != value) { field = value; (this->*changed)(); }
    };
    const auto cpuFan = snapshot.value("GetFanDataCPU");
    const int gpu1 = fanValue(snapshot.value("GetFanDataGPU1"), "speed");
    const int gpu2 = fanValue(snapshot.value("GetFanDataGPU2"), "speed");
    const int gpuFan = gpu1 < 0 ? gpu2 : gpu2 < 0 ? gpu1 : (gpu1 + gpu2) / 2;
    setText(m_cpuTemp, formatted(fanValue(cpuFan,"temp"), "°C"), &SystemMonitor::cpuTempChanged);
    setText(m_cpuFrequency, formatted(snapshot.value("GetCpuFrequencyMHz",-1).toInt()," MHz"), &SystemMonitor::cpuFrequencyChanged);
    setText(m_cpuPower, formatted(numeric(cpu,"powerDraw")," W",1), &SystemMonitor::cpuPowerChanged);
    setText(m_gpuTemp, formatted(fallback(numeric(gpu,"temp"),numeric(igpu,"temp")),"°C"), &SystemMonitor::gpuTempChanged);
    setText(m_gpuFrequency, formatted(fallback(numeric(gpu,"coreFrequency"),numeric(gpu,"coreFreq"))," MHz"), &SystemMonitor::gpuFrequencyChanged);
    setText(m_gpuPower, formatted(fallback(numeric(gpu,"powerDraw"),numeric(igpu,"powerDraw"))," W",1), &SystemMonitor::gpuPowerChanged);
    setText(m_iGpuFrequency, formatted(numeric(igpu,"coreFrequency")," MHz"), &SystemMonitor::iGpuFrequencyChanged);
    setText(m_iGpuPower, formatted(numeric(igpu,"powerDraw")," W",1), &SystemMonitor::iGpuPowerChanged);
    setText(m_iGpuTemp, formatted(numeric(igpu,"temp"),"°C"), &SystemMonitor::iGpuTempChanged);
    setText(m_fanSpeed, formatted(fanValue(cpuFan,"speed")," %"), &SystemMonitor::fanSpeedChanged);
    setText(m_gpuFanSpeed, formatted(gpuFan," %"), &SystemMonitor::gpuFanSpeedChanged);
    setInt(m_dGpuComputeUtil, gpu.value("computeUtilPct").toInt(-1), &SystemMonitor::dGpuComputeUtilChanged);
    setInt(m_dGpuMemoryUtil, gpu.value("memoryUtilPct").toInt(-1), &SystemMonitor::dGpuMemoryUtilChanged);
    setInt(m_dGpuPstate, gpu.value("currentPstate").toInt(-1), &SystemMonitor::dGpuPstateChanged);
    setInt(m_dGpuGrClockOffset, gpu.value("grClockOffsetMHz").toInt(-999), &SystemMonitor::dGpuGrClockOffsetChanged);
    setInt(m_dGpuMemClockOffset, gpu.value("memClockOffsetMHz").toInt(-999), &SystemMonitor::dGpuMemClockOffsetChanged);
    setText(m_waterCoolerFanSpeed, formatted(snapshot.value("GetWaterCoolerFanSpeed",-1).toInt()," %"), &SystemMonitor::waterCoolerFanSpeedChanged);
    const int pump = snapshot.value("GetWaterCoolerPumpLevel",-1).toInt();
    const QStringList levels = {"High", "Max", "Low", "Med", "Off"};
    setText(m_waterCoolerPumpLevel, levels.value(pump,"--"), &SystemMonitor::waterCoolerPumpLevelChanged);
    if (snapshot.contains("GetDisplayBrightness"))
      setInt(m_displayBrightness, snapshot.value("GetDisplayBrightness").toInt(), &SystemMonitor::displayBrightnessChanged);
    if (snapshot.contains("GetWebcamSWStatus")) {
      const bool value = snapshot.value("GetWebcamSWStatus").toBool();
      if (value != m_webcamEnabled) { m_webcamEnabled = value; emit webcamEnabledChanged(); }
    }
    if (snapshot.contains("GetFnLockStatus")) {
      const bool value = snapshot.value("GetFnLockStatus").toBool();
      if (value != m_fnLock) { m_fnLock = value; emit fnLockChanged(); }
    }
    emit metricsUpdated();
  });
}

void SystemMonitor::setDisplayBrightness( int brightness )
{
  if ( m_client->setDisplayBrightness( brightness ) )
  {
    m_displayBrightness = brightness;
    emit displayBrightnessChanged();
  }
}

void SystemMonitor::setWebcamEnabled( bool enabled )
{
  if ( m_client->setWebcamEnabled( enabled ) )
  {
    m_webcamEnabled = enabled;
    emit webcamEnabledChanged();
  }
}

void SystemMonitor::setFnLock( bool enabled )
{
  if ( m_client->setFnLock( enabled ) )
  {
    m_fnLock = enabled;
    emit fnLockChanged();
  }
}

void SystemMonitor::refreshAll()
{
  updateMetrics();
}

// =====================================================================
//  Charging - one-time initialization
// =====================================================================

void SystemMonitor::initializeChargingState()
{
  // Charging profiles (firmware-level modes)
  if ( auto json = m_client->getChargingProfilesAvailable() )
  {
    if ( QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) ); doc.isArray() )
    {
      QStringList profiles;
      for ( const auto &v : doc.array() )
        profiles << v.toString();
      if ( m_chargingProfilesAvailable != profiles )
      {
        m_chargingProfilesAvailable = profiles;
        emit chargingProfilesAvailableChanged();
      }
    }
  }

  if ( auto profile = m_client->getCurrentChargingProfile() )
  {
    if ( QString p = QString::fromStdString( *profile ); m_currentChargingProfile != p )
    {
      m_currentChargingProfile = p;
      emit currentChargingProfileChanged();
    }
  }

  // Charging priority (USB-C PD)
  if ( auto json = m_client->getChargingPrioritiesAvailable() )
  {
    if ( QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) ); doc.isArray() )
    {
      QStringList priorities;
      for ( const auto &v : doc.array() )
        priorities << v.toString();
      if ( m_chargingPrioritiesAvailable != priorities )
      {
        m_chargingPrioritiesAvailable = priorities;
        emit chargingPrioritiesAvailableChanged();
      }
    }
  }

  if ( auto priority = m_client->getCurrentChargingPriority() )
  {
    if ( QString p = QString::fromStdString( *priority ); m_currentChargingPriority != p )
    {
      m_currentChargingPriority = p;
      emit currentChargingPriorityChanged();
    }
  }

  // Battery charge thresholds
  bool thresholdsAvail = false;
  if ( auto json = m_client->getChargeEndAvailableThresholds() )
  {
    if ( QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *json ) );
         doc.isArray() and not doc.array().isEmpty() )
      thresholdsAvail = true;
  }
  if ( m_chargeThresholdsAvailable != thresholdsAvail )
  {
    m_chargeThresholdsAvailable = thresholdsAvail;
    emit chargeThresholdsAvailableChanged();
  }

  if ( m_chargeThresholdsAvailable )
  {
    if ( auto val = m_client->getChargeStartThreshold() )
    {
      if ( m_chargeStartThreshold != *val )
      {
        m_chargeStartThreshold = *val;
        emit chargeStartThresholdChanged();
      }
    }

    if ( auto val = m_client->getChargeEndThreshold() )
    {
      if ( m_chargeEndThreshold != *val )
      {
        m_chargeEndThreshold = *val;
        emit chargeEndThresholdChanged();
      }
    }

    if ( auto type = m_client->getChargeType() )
    {
      if ( QString t = QString::fromStdString( *type ); m_chargeType != t )
      {
        m_chargeType = t;
        emit chargeTypeChanged();
      }
    }
  }
}

// =====================================================================
//  Charging - setters
// =====================================================================

void SystemMonitor::setCurrentChargingProfile( const QString &profile )
{
  if ( m_client->setChargingProfile( profile.toStdString() ) )
  {
    m_currentChargingProfile = profile;
    emit currentChargingProfileChanged();
  }
}

void SystemMonitor::setCurrentChargingPriority( const QString &priority )
{
  if ( m_client->setChargingPriority( priority.toStdString() ) )
  {
    m_currentChargingPriority = priority;
    emit currentChargingPriorityChanged();
  }
}

void SystemMonitor::setChargeStartThreshold( int value )
{
  if ( m_client->setChargeStartThreshold( value ) )
  {
    m_chargeStartThreshold = value;
    emit chargeStartThresholdChanged();
  }
}

void SystemMonitor::setChargeEndThreshold( int value )
{
  if ( m_client->setChargeEndThreshold( value ) )
  {
    m_chargeEndThreshold = value;
    emit chargeEndThresholdChanged();
  }
}

void SystemMonitor::setChargeType( const QString &type )
{
  if ( m_client->setChargeType( type.toStdString() ) )
  {
    m_chargeType = type;
    emit chargeTypeChanged();
  }
}

void SystemMonitor::setMonitoringActive( bool active )
{
  if ( m_monitoringActive != active )
  {
    ++m_monitorGeneration;
    m_monitoringActive = active;
    emit monitoringActiveChanged();

    if ( active )
    {
      qDebug() << "[SystemMonitor] Starting asynchronous monitoring at 2000ms";
      // Preserve the last snapshot while waiting for the next visible refresh.
      m_updateTimer->start();
      qDebug() << "[SystemMonitor] Timer started, first update in 2000ms. Timer active:" << m_updateTimer->isActive();
    }
    else
    {
      // Stop monitoring
      qDebug() << "[SystemMonitor] Stopping monitoring";
      m_updateTimer->stop();
    }
  }
}

} // namespace ucc
