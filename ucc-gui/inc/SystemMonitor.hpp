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

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include "UccdClient.hpp"

namespace ucc
{

/**
 * @brief System monitoring for QML interface
 *
 * Provides real-time system metrics
 */
class SystemMonitor : public QObject
{
  Q_OBJECT
  Q_PROPERTY( QString cpuUsage READ cpuUsage NOTIFY cpuUsageChanged )
  Q_PROPERTY( QString cpuTemp READ cpuTemp NOTIFY cpuTempChanged )
  Q_PROPERTY( QString cpuFrequency READ cpuFrequency NOTIFY cpuFrequencyChanged )
  Q_PROPERTY( QString cpuPower READ cpuPower NOTIFY cpuPowerChanged )
  Q_PROPERTY( QString gpuTemp READ gpuTemp NOTIFY gpuTempChanged )
  Q_PROPERTY( QString gpuFrequency READ gpuFrequency NOTIFY gpuFrequencyChanged )
  Q_PROPERTY( QString gpuPower READ gpuPower NOTIFY gpuPowerChanged )
  Q_PROPERTY( QString iGpuFrequency READ iGpuFrequency NOTIFY iGpuFrequencyChanged )
  Q_PROPERTY( QString iGpuPower READ iGpuPower NOTIFY iGpuPowerChanged )
  Q_PROPERTY( QString iGpuTemp READ iGpuTemp NOTIFY iGpuTempChanged )
  Q_PROPERTY( QString cpuFanSpeed READ cpuFanSpeed NOTIFY fanSpeedChanged )
  Q_PROPERTY( QString gpuFanSpeed READ gpuFanSpeed NOTIFY gpuFanSpeedChanged )
  Q_PROPERTY( int dGpuComputeUtil   READ dGpuComputeUtil   NOTIFY dGpuComputeUtilChanged )
  Q_PROPERTY( int dGpuMemoryUtil    READ dGpuMemoryUtil    NOTIFY dGpuMemoryUtilChanged )
  Q_PROPERTY( int dGpuPstate        READ dGpuPstate        NOTIFY dGpuPstateChanged )
  Q_PROPERTY( int dGpuGrClockOffset  READ dGpuGrClockOffset  NOTIFY dGpuGrClockOffsetChanged )
  Q_PROPERTY( int dGpuMemClockOffset READ dGpuMemClockOffset NOTIFY dGpuMemClockOffsetChanged )
  Q_PROPERTY( QString waterCoolerFanSpeed READ waterCoolerFanSpeed NOTIFY waterCoolerFanSpeedChanged )
  Q_PROPERTY( QString waterCoolerPumpLevel READ waterCoolerPumpLevel NOTIFY waterCoolerPumpLevelChanged )
  Q_PROPERTY( int displayBrightness READ displayBrightness WRITE setDisplayBrightness NOTIFY displayBrightnessChanged )
  Q_PROPERTY( bool webcamEnabled READ webcamEnabled WRITE setWebcamEnabled NOTIFY webcamEnabledChanged )
  Q_PROPERTY( bool fnLock READ fnLock WRITE setFnLock NOTIFY fnLockChanged )
  Q_PROPERTY( bool monitoringActive READ monitoringActive WRITE setMonitoringActive NOTIFY monitoringActiveChanged )
  Q_PROPERTY( bool isACPower READ isACPower NOTIFY isACPowerChanged )

  // Charging properties
  Q_PROPERTY( QStringList chargingProfilesAvailable READ chargingProfilesAvailable NOTIFY chargingProfilesAvailableChanged )
  Q_PROPERTY( QString currentChargingProfile READ currentChargingProfile WRITE setCurrentChargingProfile NOTIFY currentChargingProfileChanged )
  Q_PROPERTY( QStringList chargingPrioritiesAvailable READ chargingPrioritiesAvailable NOTIFY chargingPrioritiesAvailableChanged )
  Q_PROPERTY( QString currentChargingPriority READ currentChargingPriority WRITE setCurrentChargingPriority NOTIFY currentChargingPriorityChanged )
  Q_PROPERTY( bool chargeThresholdsAvailable READ chargeThresholdsAvailable NOTIFY chargeThresholdsAvailableChanged )
  Q_PROPERTY( int chargeStartThreshold READ chargeStartThreshold WRITE setChargeStartThreshold NOTIFY chargeStartThresholdChanged )
  Q_PROPERTY( int chargeEndThreshold READ chargeEndThreshold WRITE setChargeEndThreshold NOTIFY chargeEndThresholdChanged )
  Q_PROPERTY( QString chargeType READ chargeType WRITE setChargeType NOTIFY chargeTypeChanged )

public:
  explicit SystemMonitor( QObject *parent = nullptr );
  ~SystemMonitor() override;

  QString cpuUsage() const { return m_cpuUsage; }
  QString cpuTemp() const { return m_cpuTemp; }
  QString cpuFrequency() const { return m_cpuFrequency; }
  QString cpuPower() const { return m_cpuPower; }
  QString gpuTemp() const { return m_gpuTemp; }
  QString gpuFrequency() const { return m_gpuFrequency; }
  QString gpuPower() const { return m_gpuPower; }
  QString iGpuFrequency() const { return m_iGpuFrequency; }
  QString iGpuPower() const { return m_iGpuPower; }
  QString iGpuTemp() const { return m_iGpuTemp; }
  QString cpuFanSpeed() const { return m_fanSpeed; }
  QString gpuFanSpeed() const { return m_gpuFanSpeed; }
  int dGpuComputeUtil()    const { return m_dGpuComputeUtil; }
  int dGpuMemoryUtil()     const { return m_dGpuMemoryUtil; }
  int dGpuPstate()         const { return m_dGpuPstate; }
  int dGpuGrClockOffset()  const { return m_dGpuGrClockOffset; }
  int dGpuMemClockOffset() const { return m_dGpuMemClockOffset; }
  QString waterCoolerFanSpeed() const { return m_waterCoolerFanSpeed; }
  QString waterCoolerPumpLevel() const { return m_waterCoolerPumpLevel; }
  int displayBrightness() const { return m_displayBrightness; }
  bool webcamEnabled() const { return m_webcamEnabled; }
  bool fnLock() const { return m_fnLock; }
  bool monitoringActive() const { return m_monitoringActive; }
  bool isACPower() const { return m_isACPower; }

  // Charging getters
  QStringList chargingProfilesAvailable() const { return m_chargingProfilesAvailable; }
  QString currentChargingProfile() const { return m_currentChargingProfile; }
  QStringList chargingPrioritiesAvailable() const { return m_chargingPrioritiesAvailable; }
  QString currentChargingPriority() const { return m_currentChargingPriority; }
  bool chargeThresholdsAvailable() const { return m_chargeThresholdsAvailable; }
  int chargeStartThreshold() const { return m_chargeStartThreshold; }
  int chargeEndThreshold() const { return m_chargeEndThreshold; }
  QString chargeType() const { return m_chargeType; }

public slots:
  void setDisplayBrightness( int brightness );
  void setWebcamEnabled( bool enabled );
  void setFnLock( bool enabled );
  void setMonitoringActive( bool active );
  void refreshAll();

  // Charging setters
  void setCurrentChargingProfile( const QString &profile );
  void setCurrentChargingPriority( const QString &priority );
  void setChargeStartThreshold( int value );
  void setChargeEndThreshold( int value );
  void setChargeType( const QString &type );

signals:
  void metricsUpdated();
  void cpuUsageChanged();
  void cpuTempChanged();
  void cpuFrequencyChanged();
  void cpuPowerChanged();
  void gpuTempChanged();
  void gpuFrequencyChanged();
  void gpuPowerChanged();
  void iGpuFrequencyChanged();
  void iGpuPowerChanged();
  void iGpuTempChanged();
  void fanSpeedChanged();
  void gpuFanSpeedChanged();
  void dGpuComputeUtilChanged();
  void dGpuMemoryUtilChanged();
  void dGpuPstateChanged();
  void dGpuGrClockOffsetChanged();
  void dGpuMemClockOffsetChanged();
  void waterCoolerFanSpeedChanged();
  void waterCoolerPumpLevelChanged();
  void displayBrightnessChanged();
  void webcamEnabledChanged();
  void fnLockChanged();
  void monitoringActiveChanged();
  void isACPowerChanged();

  // Charging signals
  void chargingProfilesAvailableChanged();
  void currentChargingProfileChanged();
  void chargingPrioritiesAvailableChanged();
  void currentChargingPriorityChanged();
  void chargeThresholdsAvailableChanged();
  void chargeStartThresholdChanged();
  void chargeEndThresholdChanged();
  void chargeTypeChanged();

private slots:
  void updateMetrics();

private:
  void initializeChargingState();

  std::unique_ptr< UccdClient > m_client;
  QTimer *m_updateTimer;
  QElapsedTimer m_controlsAge;
  bool m_metricsPending = false;
  unsigned m_monitorGeneration = 0;

  QString m_cpuUsage = "0%";
  QString m_cpuTemp = "0°C";
  QString m_cpuFrequency = "0 MHz";
  QString m_cpuPower = "0 W";
  QString m_gpuTemp = "0°C";
  QString m_gpuFrequency = "0 MHz";
  QString m_gpuPower = "0 W";
  QString m_iGpuFrequency = "0 MHz";
  QString m_iGpuPower = "0 W";
  QString m_iGpuTemp = "0\u00b0C";
  QString m_fanSpeed = "0 RPM";
  QString m_gpuFanSpeed = "0 RPM";
  int m_dGpuComputeUtil    = -1;
  int m_dGpuMemoryUtil     = -1;
  int m_dGpuPstate         = -1;
  int m_dGpuGrClockOffset  = -999;
  int m_dGpuMemClockOffset = -999;
  QString m_waterCoolerFanSpeed = "--";
  QString m_waterCoolerPumpLevel = "--";
  int m_displayBrightness = 50;
  bool m_webcamEnabled = true;
  bool m_fnLock = false;
  bool m_monitoringActive = false;
  bool m_isACPower = false;

  // Charging state
  QStringList m_chargingProfilesAvailable;
  QString m_currentChargingProfile;
  QStringList m_chargingPrioritiesAvailable;
  QString m_currentChargingPriority;
  bool m_chargeThresholdsAvailable = false;
  int m_chargeStartThreshold = 0;
  int m_chargeEndThreshold = 100;
  QString m_chargeType;
};

} // namespace ucc
