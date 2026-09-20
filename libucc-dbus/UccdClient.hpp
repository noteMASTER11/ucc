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
#include <QDBusInterface>
#include <QDBusConnection>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <string>
#include <memory>
#include <optional>
#include <functional>
#include <vector>
#include <map>

namespace ucc
{

/**
 * @brief DBus client for communicating with uccd daemon
 *
 * Provides a Qt6-native interface to all uccd DBus methods.
 */
class UccdClient : public QObject
{
  Q_OBJECT

public:
  /**
   * @brief Construct client connected to system bus
   */
  explicit UccdClient( QObject *parent = nullptr );

  ~UccdClient() override = default;

  // System Information
  std::optional< std::string > getSystemInfoJSON();
  std::optional< bool > isDeviceSupported();
  // One nonblocking read per source, shared by every metric in the GUI refresh.
  void requestMonitoringSnapshot(bool includeControls, std::function<void(QVariantMap)> ready);

  // Profile Management
  std::optional< std::string > getDefaultProfilesJSON();
  std::optional< std::string > getCpuFrequencyLimitsJSON();
  std::optional< std::string > getDefaultValuesProfileJSON();
  std::optional< std::string > getCustomProfilesJSON();
  std::optional< std::string > getActiveProfileJSON();
  std::optional< std::string > getSettingsJSON();
  std::optional< std::string > getPowerState();
  bool setStateMap( const std::string &state, const std::string &profileId );
  bool setBatchStateMap( const std::map< std::string, std::string > &entries );
  bool setActiveProfile( const std::string &profileId );
  bool applyProfile( const std::string &profileJSON );
  bool saveCustomProfile( const std::string &profileJSON );
  bool deleteCustomProfile( const std::string &profileId );
  std::optional< std::string > getFanProfile( const std::string &fanProfileId );
  std::optional< std::string > getFanProfilesJSON();
  std::optional< bool > setFanProfile( const std::string &fanProfileId, const std::string &json );

  // Display Control
  bool setDisplayBrightness( int brightness );
  std::optional< int > getDisplayBrightness();
  bool setYCbCr420Workaround( bool enabled );
  std::optional< bool > getYCbCr420Workaround();
  bool setDisplayRefreshRate( const std::string &display, int refreshRate );

  // CPU Control
  bool setCpuScalingGovernor( const std::string &governor );
  std::optional< std::string > getCpuScalingGovernor();
  std::optional< std::vector< std::string > > getAvailableCpuGovernors();
  bool setCpuFrequency( int minFreq, int maxFreq );
  bool setEnergyPerformancePreference( const std::string &preference );
  std::optional< std::vector< std::string > > getAvailableEPPs();
  std::optional< int > getCpuCoreCount();

  // Fan Control
  bool setFanProfile( const std::string &profileJSON );
  bool setFanProfileCPU( const std::string &pointsJSON );
  bool setFanProfileDGPU( const std::string &pointsJSON );
  bool applyFanProfiles( const std::string &fanProfilesJSON );
  bool revertFanProfiles();
  std::optional< std::string > getCurrentFanSpeed();
  std::optional< std::string > getFanTemperatures();

  // Power Management
  bool setODMPowerLimits( const std::vector< int > &limits );
  std::optional< std::vector< int > > getODMPowerLimits();

  // Charging Profile (firmware-level charging modes)
  std::optional< std::string > getChargingProfilesAvailable();
  std::optional< std::string > getCurrentChargingProfile();
  bool setChargingProfile( const std::string &profileDescriptor );

  // Charging Priority (USB-C PD priority)
  std::optional< std::string > getChargingPrioritiesAvailable();
  std::optional< std::string > getCurrentChargingPriority();
  bool setChargingPriority( const std::string &priorityDescriptor );

  // Battery Charge Thresholds
  std::optional< std::string > getChargeStartAvailableThresholds();
  std::optional< std::string > getChargeEndAvailableThresholds();
  std::optional< int > getChargeStartThreshold();
  std::optional< int > getChargeEndThreshold();
  bool setChargeStartThreshold( int value );
  bool setChargeEndThreshold( int value );

  // Charge Type
  std::optional< std::string > getChargeType();
  bool setChargeType( const std::string &type );

  // GPU Control
  bool setNVIDIAPowerOffset( int offset );
  std::optional< int > getNVIDIAPowerOffset();
  std::optional< int > getNVIDIAPowerCTRLMaxPowerLimit();
  std::optional< int > getNVIDIAPowerCTRLDefaultPowerLimit();
  std::optional< bool > getNVIDIAPowerCTRLAvailable();
  bool setPrimeProfile( const std::string &profile );
  std::optional< std::string > getPrimeProfile();
  std::optional< std::string > getGpuInfo();

  // Device Capability Queries
  std::optional< bool > getWaterCoolerSupported();
  std::optional< bool > getCTGPAdjustmentSupported();
  std::optional< bool > getKeyboardBacklightControlSupported();
  std::optional< bool > getHwpDynamicBoostSupported();

  // Keyboard Control
  bool setKeyboardBacklight( const std::string &config );
  std::optional< std::string > getKeyboardBacklightInfo();
  std::optional< std::string > getKeyboardBacklightStates();
  
  // Custom Keyboard Profiles
  std::optional< std::string > getCustomKeyboardProfiles();
  bool saveCustomKeyboardProfile( const std::string &id, const std::string &name, const std::string &json );
  bool deleteCustomKeyboardProfile( const std::string &id );

  // Custom Fan Profiles
  std::optional< std::string > getCustomFanProfiles();
  bool saveCustomFanProfile( const std::string &id, const std::string &name, const std::string &json );
  bool deleteCustomFanProfile( const std::string &id );

  bool setFnLock( bool enabled );
  std::optional< bool > getFnLock();

  // Webcam Control
  bool setWebcamEnabled( bool enabled );
  std::optional< bool > getWebcamEnabled();

  // ODM Profile
  bool setODMPerformanceProfile( const std::string &profile );
  std::optional< std::string > getODMPerformanceProfile();
  std::optional< std::vector< std::string > > getAvailableODMProfiles();

  // System Monitoring
  std::optional< int > getCpuTemperature();
  std::optional< int > getGpuTemperature();
  std::optional< int > getIGpuTemperature();
  std::optional< int > getCpuFrequency();
  std::optional< int > getGpuFrequency();
  std::optional< int > getIGpuFrequency();
  std::optional< double > getCpuPower();
  std::optional< double > getGpuPower();
  std::optional< double > getIGpuPower();
  // Extended discrete GPU metrics (NVIDIA, -1 when unavailable)
  std::optional< int > getDGpuComputeUtilPct();       ///< Compute utilization 0-100 %
  std::optional< int > getDGpuMemoryUtilPct();        ///< Memory-controller utilization 0-100 %
  std::optional< int > getDGpuVramUsedMiB();          ///< Used VRAM in MiB
  std::optional< int > getDGpuVramTotalMiB();         ///< Total VRAM in MiB
  std::optional< std::string > getDGpuPerfLimitReason(); ///< Perf-cap/throttle reason
  std::optional< int > getDGpuEncoderUtilPct();       ///< NVENC utilization 0-100 %
  std::optional< int > getDGpuDecoderUtilPct();       ///< NVDEC utilization 0-100 %
  std::optional< int > getDGpuCurrentPstate();        ///< Current P-state index (0-15)
  std::optional< int > getDGpuGrClockOffsetMHz();     ///< Graphics-clock offset at current P-state
  std::optional< int > getDGpuMemClockOffsetMHz();    ///< Memory-clock offset at current P-state
  std::optional< int > getDGpuVramFrequencyMHz();     ///< VRAM frequency in MHz
  std::optional< int > getDGpuCoreVoltageMv();        ///< Core voltage in mV
  std::optional< int > getFanSpeedRPM();
  std::optional< int > getGpuFanSpeedRPM();
  std::optional< int > getFanSpeedPercent();
  std::optional< int > getGpuFanSpeedPercent();
  // Water cooler readings (if available from daemon)
  std::optional< int > getWaterCoolerFanSpeed();
  std::optional< int > getWaterCoolerPumpLevel();

  // Water cooler control
  bool enableWaterCooler( bool enable );
  std::optional< bool > isWaterCoolerEnabled();
  bool setWaterCoolerFanSpeed( int dutyCyclePercent );
  bool setWaterCoolerPumpVoltage( int voltageCode );   // PumpVoltage enum cast to int
  bool setWaterCoolerLEDColor( int r, int g, int b, int mode );  // RGBState enum cast to int
  bool turnOffWaterCoolerLED();

  // Monitoring history
  std::optional< QByteArray > getMonitorDataSince( qint64 sinceTimestampMs );
  bool setMonitorHistoryHorizon( int seconds );
  std::optional< int > getMonitorHistoryHorizon();

  // Signal Subscription
  using ProfileChangedCallback = std::function< void( const std::string &profileId ) >;
  using PowerStateChangedCallback = std::function< void( const std::string &state ) >;

  void subscribeProfileChanged( ProfileChangedCallback callback );
  void subscribePowerStateChanged( PowerStateChangedCallback callback );

  // Connection status
  bool isConnected() const;

signals:
  void profileChanged( const QString &profileId,
                       const QString &keyboardProfileId,
                       const QString &fanProfileId );
  void powerStateChanged( const QString &state );
  void connectionStatusChanged( bool connected );

private slots:
  void onProfileChangedSignal( const QString &profileId,
                               const QString &keyboardProfileId,
                               const QString &fanProfileId );
  void onPowerStateChangedSignal( const QString &state );
  void onServiceRegistered( const QString &service );
  void onServiceUnregistered( const QString &service );

private:
  void connectToDaemon();          ///< (Re)create the interface and subscribe to D-Bus signals
  void subscribeDbusSignals();     ///< Connect D-Bus signals (idempotent - disconnects first)

  std::unique_ptr< QDBusInterface > m_interface;
  QDBusServiceWatcher *m_serviceWatcher = nullptr;
  bool m_connected = false;

  static constexpr const char *DBUS_SERVICE = "com.uniwill.uccd";
  static constexpr const char *DBUS_PATH = "/com/uniwill/uccd";
  static constexpr const char *DBUS_INTERFACE = "com.uniwill.uccd";

  // Helper for DBus calls
  template< typename T >
  std::optional< T > callMethod( const QString &method ) const;

  template< typename T, typename... Args >
  std::optional< T > callMethod( const QString &method, const Args &...args ) const;

  bool callVoidMethod( const QString &method ) const;

  template< typename... Args >
  bool callVoidMethod( const QString &method, const Args &...args ) const;
};

} // namespace ucc
