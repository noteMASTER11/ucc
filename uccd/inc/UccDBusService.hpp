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
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusError>
#include <QDBusContext>
#include <QVariantMap>
#include <atomic>
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include "CommonTypes.hpp"
#include "workers/DaemonWorker.hpp"
#include "workers/HardwareMonitorWorker.hpp"
#include "workers/DisplayWorker.hpp"
#include "workers/CpuWorker.hpp"
#include "workers/FanControlWorker.hpp"
#include "KeyboardBacklightController.hpp"
#include "workers/ProfileSettingsWorker.hpp"
#include "workers/LCTWaterCoolerWorker.hpp"
#include "workers/NvidiaOCWorker.hpp"
#include "FnLockController.hpp"
#include "profiles/UccProfile.hpp"
#include "profiles/DefaultProfiles.hpp"
#include "ProfileManager.hpp"
#include "SettingsManager.hpp"
#include "AutosaveManager.hpp"
#include "TccSettings.hpp"
#include "MetricsHistoryStore.hpp"
#include "SystemInfo.hpp"
#include "tuxedo_io_lib/tuxedo_io_api.hh"

// Forward declarations
class HardwareMonitorWorker;
class UccDBusService;

// helper functions for JSON serialization
std::string dgpuInfoToJSON( const DGpuInfo &info );
std::string igpuInfoToJSON( const IGpuInfo &info );

/**
 * @brief Time-stamped data structure
 *
 * Holds a value along with its timestamp for DBus transmission.
 */
template< typename T >
struct TimeData
{
  int64_t timestamp;
  T data;

  TimeData() : timestamp( 0 ), data{} {}

  TimeData( int64_t ts, const T &value ) : timestamp( ts ), data( value ) {}

  void set( int64_t ts, const T &value )
  {
    timestamp = ts;
    data = value;
  }
};

/**
 * @brief Fan data structure
 *
 * Contains timestamped speed and temperature data for a fan.
 */
struct FanData
{
  TimeData< int32_t > speed;
  TimeData< int32_t > temp;

  FanData() : speed(), temp() {}
};

/**
 * @brief DBus data container
 *
 * Contains all data that is exposed via the DBus interface.
 * This structure mirrors the UccDBusData TypeScript class.
 */
class UccDBusData
{
public:
  std::string device;
  std::string displayModes;
  std::atomic< bool > isX11;
  std::atomic< bool > tuxedoWmiAvailable;
  std::atomic< bool > fanHwmonAvailable;
  std::string uccdVersion;
  std::string systemInfoJSON;
  std::vector< FanData > fans;
  std::atomic< bool > webcamSwitchAvailable;
  std::atomic< bool > webcamSwitchStatus;
  std::atomic< bool > forceYUV420OutputSwitchAvailable;
  std::string dGpuInfoValuesJSON;
  std::string iGpuInfoValuesJSON;
  std::string cpuPowerValuesJSON;
  std::string primeState;
  std::atomic< bool > modeReapplyPending;
  std::string tempProfileName;
  std::string tempProfileId;
  std::string activeProfileJSON;
  std::string profilesJSON;
  std::string customProfilesJSON;
  std::string defaultProfilesJSON;
  std::string defaultValuesProfileJSON;
  std::string settingsJSON;
  std::vector< std::string > odmProfilesAvailable;
  std::string odmPowerLimitsJSON;
  std::string keyboardBacklightCapabilitiesJSON;
  std::string keyboardBacklightStatesJSON;
  std::atomic< int32_t > fansMinSpeed;
  std::atomic< bool > fansOffAvailable;
  std::string chargingProfilesAvailable;
  std::string currentChargingProfile;
  std::string chargingPrioritiesAvailable;
  std::string currentChargingPriority;
  std::string chargeStartAvailableThresholds;
  std::string chargeEndAvailableThresholds;
  std::atomic< int32_t > chargeStartThreshold;
  std::atomic< int32_t > chargeEndThreshold;
  std::string chargeType;
  std::atomic< bool > fnLockSupported;
  std::atomic< bool > fnLockStatus;
  std::atomic< bool > sensorDataCollectionStatus;
  std::atomic< bool > d0MetricsUsage;
  std::atomic< int32_t > nvidiaPowerCTRLDefaultPowerLimit;
  std::atomic< int32_t > nvidiaPowerCTRLMaxPowerLimit;
  std::atomic< bool > nvidiaPowerCTRLAvailable;
  std::atomic< bool > waterCoolerAvailable;
  std::atomic< bool > waterCoolerConnected;
  std::atomic< bool > waterCoolerScanningEnabled;
  std::atomic< bool > waterCoolerSupported;
  std::atomic< bool > cTGPAdjustmentSupported;
  std::atomic< bool > deviceSupported{ false };
  std::atomic< int32_t > cpuFrequencyMHz;

  std::mutex dataMutex;

  explicit UccDBusData( int numberFans = 3 )
    : device( "unknown" ),
      displayModes( "[]" ),
      isX11( false ),
      tuxedoWmiAvailable( false ),
      fanHwmonAvailable( false ),
      uccdVersion( "0.0.0" ),
      fans( static_cast< size_t >( numberFans ) ),
      webcamSwitchAvailable( false ),
      webcamSwitchStatus( false ),
      forceYUV420OutputSwitchAvailable( false ),
      dGpuInfoValuesJSON( "{}" ),
      iGpuInfoValuesJSON( "{}" ),
      cpuPowerValuesJSON( "{}" ),
      primeState( "unknown" ),
      modeReapplyPending( false ),
      tempProfileName( "" ),
      tempProfileId( "" ),
      activeProfileJSON( "{}" ),
      profilesJSON( "[]" ),
      customProfilesJSON( "[]" ),
      defaultProfilesJSON( "[]" ),
      defaultValuesProfileJSON( "{}" ),
      settingsJSON( "{}" ),
      odmProfilesAvailable(),
      odmPowerLimitsJSON( "[]" ),
      keyboardBacklightCapabilitiesJSON( "{}" ),
      keyboardBacklightStatesJSON( "{}" ),
      fansMinSpeed( 0 ),
      fansOffAvailable( false ),
      chargingProfilesAvailable( "[]" ),
      currentChargingProfile( "" ),
      chargingPrioritiesAvailable( "[]" ),
      currentChargingPriority( "" ),
      chargeStartAvailableThresholds( "[]" ),
      chargeEndAvailableThresholds( "[]" ),
      chargeStartThreshold( -1 ),
      chargeEndThreshold( -1 ),
      chargeType( "Unknown" ),
      fnLockSupported( false ),
      fnLockStatus( false ),
      sensorDataCollectionStatus( false ),
      d0MetricsUsage( false ),
      nvidiaPowerCTRLDefaultPowerLimit( 0 ),
      nvidiaPowerCTRLMaxPowerLimit( 1000 ),
      nvidiaPowerCTRLAvailable( false ),
        waterCoolerAvailable( false ),
        waterCoolerConnected( false ),
        waterCoolerScanningEnabled( ucc::WATER_COOLER_INITIAL_STATE ),
        waterCoolerSupported( false ),
        cTGPAdjustmentSupported( false ),
        cpuFrequencyMHz( -1 )
  {
  }
};

/**
 * @brief QObject registered on the D-Bus system bus.
 *
 * Inherits QDBusContext so adaptor slots can retrieve the caller's
 * connection and message for Polkit authorization checks.
 */
class UccDBusObject : public QObject, protected QDBusContext
{
  Q_OBJECT
public:
  explicit UccDBusObject( QObject *parent = nullptr ) : QObject( parent ) {}
  using QDBusContext::connection;
  using QDBusContext::message;
};

/**
 * @brief TCC DBus Interface Adaptor
 *
 * Implements the com.uniwill.uccd DBus interface using Qt's DBus adaptor framework.
 * Handles all method calls from DBus clients and provides access to daemon data.
 */
class UccDBusInterfaceAdaptor : public QDBusAbstractAdaptor
{
  Q_OBJECT
  Q_CLASSINFO( "D-Bus Interface", "com.uniwill.uccd" )

public:
  static constexpr const char* INTERFACE_NAME = "com.uniwill.uccd";

  /**
   * @brief Constructor
   * @param parent Parent QObject (the service object registered on D-Bus)
   * @param data Shared data structure (includes mutex)
   * @param service Reference to UccDBusService for profile operations
   */
  explicit UccDBusInterfaceAdaptor( QObject *parent,
                                    UccDBusData &data,
                                    UccDBusService *service = nullptr );

  ~UccDBusInterfaceAdaptor() override = default;

public slots:
  // device and system information
  QString GetDeviceName();
  QString GetSystemInfoJSON();
  bool IsDeviceSupported();
  QString GetDisplayModesJSON();
  bool GetIsX11();
  bool TuxedoWmiAvailable();
  bool FanHwmonAvailable();
  QString UccdVersion();

  // fan data methods
  QVariantMap GetFanDataCPU();
  QVariantMap GetFanDataGPU1();
  QVariantMap GetFanDataGPU2();

  // webcam and display methods
  bool WebcamSWAvailable();
  bool GetWebcamSWStatus();
  bool GetForceYUV420OutputSwitchAvailable();
  int GetDisplayBrightness();
  bool SetDisplayBrightness( int brightness );
  bool SetDisplayRefreshRate( const QString &display, int refreshRate );

  // gpu information methods
  QString GetDGpuInfoValuesJSON();
  QString GetIGpuInfoValuesJSON();
  QString GetCpuPowerValuesJSON();

  // graphics methods
  QString GetPrimeState();
  bool ConsumeModeReapplyPending();

  // profile methods
  QString GetActiveProfileJSON();
  QString GetPowerState();
  bool SetTempProfile( const QString &profileName );
  bool SetActiveProfile( const QString &id );
  bool ApplyProfile( const QString &profileJSON );
  QString GetProfilesJSON();
  QString GetCustomProfilesJSON();
  // Fan profile get/set for editable (custom) profiles only
  bool SetFanProfileCPU( const QString &pointsJSON );
  bool SetFanProfileDGPU( const QString &pointsJSON );
  bool ApplyFanProfiles( const QString &fanProfilesJSON );
  bool RevertFanProfiles();
  QString GetDefaultProfilesJSON();
  QString GetCpuFrequencyLimitsJSON();
  QString GetDefaultValuesProfileJSON();
  bool AddCustomProfile( const QString &profileJSON );
  bool SaveCustomProfile( const QString &profileJSON );
  bool DeleteCustomProfile( const QString &profileId );
  bool UpdateCustomProfile( const QString &profileJSON );
  QString GetFanProfile( const QString &name );
  QString GetFanProfileNames();
  QString GetGpuProfile( const QString &id );
  QString GetGpuProfileNames();
  bool SetFanProfile( const QString &name, const QString &json );

  // settings methods
  QString GetSettingsJSON();
  bool SetStateMap( const QString &state, const QString &profileId );
  bool SetBatchStateMap( const QString &stateMapJSON );

  // odm methods
  QStringList ODMProfilesAvailable();
  QString ODMPowerLimitsJSON();
  bool SetODMPowerLimitsJSON( const QString &limitsJSON );

  // keyboard backlight methods
  QString GetKeyboardBacklightCapabilitiesJSON();
  QString GetKeyboardBacklightStatesJSON();
  bool SetKeyboardBacklightStatesJSON( const QString &keyboardBacklightStatesJSON );

  // fan control methods
  int GetFansMinSpeed();
  bool GetFansOffAvailable();

  // charging methods
  QString GetChargingProfilesAvailable();
  QString GetCurrentChargingProfile();
  bool SetChargingProfile( const QString &profileDescriptor );
  QString GetChargingPrioritiesAvailable();
  QString GetCurrentChargingPriority();
  bool SetChargingPriority( const QString &priorityDescriptor );
  QString GetChargeStartAvailableThresholds();
  QString GetChargeEndAvailableThresholds();
  int GetChargeStartThreshold();
  int GetChargeEndThreshold();
  bool SetChargeStartThreshold( int value );
  bool SetChargeEndThreshold( int value );
  QString GetChargeType();
  bool SetChargeType( const QString &type );

  // fn lock methods
  bool GetFnLockSupported();
  bool GetFnLockStatus();
  void SetFnLockStatus( bool status );

  // sensor data collection methods
  void SetSensorDataCollectionStatus( bool status );
  bool GetSensorDataCollectionStatus();
  void SetDGpuD0Metrics( bool status );

  // nvidia power control methods
  int GetNVIDIAPowerCTRLDefaultPowerLimit();
  int GetNVIDIAPowerCTRLMaxPowerLimit();
  bool GetNVIDIAPowerCTRLAvailable();
  int GetNVIDIAPowerOffset();
  bool SetNVIDIAPowerOffset( int offset );
  QString GetAvailableGovernors();
  QString GetAvailableEPPs();
  int GetCpuCoreCount();

  // NVIDIA GPU OC methods
  bool GetNvidiaOCAvailable();
  QString GetNvidiaOCState( int deviceIndex );
  bool SetNvidiaClockOffset( int deviceIndex, int clockType, int pstate, int offsetMHz );
  bool SetNvidiaGpuLockedClocks( int deviceIndex, int minMHz, int maxMHz );
  bool SetNvidiaVramLockedClocks( int deviceIndex, int minMHz, int maxMHz );
  bool ResetNvidiaGpuLockedClocks( int deviceIndex );
  bool ResetNvidiaVramLockedClocks( int deviceIndex );
  bool ResetNvidiaAllClockOffsets( int deviceIndex );
  bool SetNvidiaGpuPowerLimit( int deviceIndex, double watts );
  bool ResetNvidiaGpuPowerLimit( int deviceIndex );
  bool ApplyNvidiaGpuOCProfile( const QString &profileJSON, int deviceIndex );
  bool ResetNvidiaGpuOCAll( int deviceIndex );

  // water cooler methods
  bool GetWaterCoolerAvailable();
  bool GetWaterCoolerConnected();
  int GetWaterCoolerFanSpeed();
  int GetWaterCoolerPumpLevel();
  bool EnableWaterCooler( bool enable );
  bool IsWaterCoolerEnabled();
  bool SetWaterCoolerFanSpeed( int dutyCyclePercent );
  bool SetWaterCoolerPumpVoltage( int voltage );
  bool SetWaterCoolerLEDColor( int red, int green, int blue, int mode );
  bool TurnOffWaterCoolerLED();
  bool TurnOffWaterCoolerFan();
  bool TurnOffWaterCoolerPump();
  bool IsWaterCoolerAutoControlEnabled();

  // device capability methods
  bool GetWaterCoolerSupported();
  bool GetCTGPAdjustmentSupported();

  // monitoring history methods
  QByteArray GetMonitorDataSince( qlonglong sinceTimestampMs );
  void SetMonitorHistoryHorizon( int seconds );
  int GetMonitorHistoryHorizon();
  int GetCpuFrequencyMHz();

signals:
  void ProfileChanged( const QString &profileId,
                       const QString &keyboardProfileId,
                       const QString &fanProfileId,
                       const QString &gpuProfileId );
  void ModeReapplyPendingChanged( bool pending );
  void PowerStateChanged( const QString &state );
  void WaterCoolerStatusChanged( const QString &status );

public:
  // signal emitters (call these from service code)
  void emitModeReapplyPendingChanged( bool pending );
  void emitProfileChanged( const std::string &profileId,
                           const std::string &keyboardProfileId = {},
                           const std::string &fanProfileId = {},
                           const std::string &gpuProfileId = {} );
  void emitPowerStateChanged( const std::string &state );
  void emitWaterCoolerStatusChanged( const std::string &status );

  // allow UccDBusService to access timeout handling
  friend class UccDBusService;

private:
  UccDBusData &m_data;
  UccDBusService *m_service;
  std::chrono::steady_clock::time_point m_lastDataCollectionAccess;

  void resetDataCollectionTimeout();
  QVariantMap exportFanData( const FanData &fanData );

  /**
   * @brief Check Polkit authorization for the current D-Bus caller.
   *
   * Retrieves the incoming D-Bus message from the parent UccDBusObject
   * (which inherits QDBusContext) and delegates to PolkitAuthority.
   *
   * @param actionId One of PolkitAuthority::ACTION_* constants
   * @return true if the caller is authorized
   */
  bool checkAuth( const char *actionId ) noexcept;
};

/**
 * @brief TCC DBus Service Worker
 *
 * Manages the DBus service lifecycle as a daemon worker.
 * Exports the TCC interface on the system bus and handles periodic updates.
 */
class UccDBusService : public DaemonWorker
{
public:
  /**
   * @brief Constructor
   */
  UccDBusService();

  virtual ~UccDBusService() = default;

  // Prevent copy and move
  UccDBusService( const UccDBusService & ) = delete;
  UccDBusService( UccDBusService && ) = delete;
  UccDBusService &operator=( const UccDBusService & ) = delete;
  UccDBusService &operator=( UccDBusService && ) = delete;

  /**
   * @brief Get the DBus interface adaptor
   * @return Pointer to adaptor or nullptr if not initialized
   */
  UccDBusInterfaceAdaptor *getAdaptor() noexcept
  {
    return m_adaptor.get();
  }

  // Control water cooler scanning (can be called by DBus adaptor)
  void setWaterCoolerScanningEnabled( bool enable );

  /**
   * @brief Get the CPU worker
   * @return Pointer to CPU worker or nullptr if not initialized
   */
  CpuWorker *getCpuWorker() noexcept
  {
    return m_cpuWorker.get();
  }

  /**
   * @brief Get the DBus data reference for testing
   */
  const UccDBusData &getDbusData() const noexcept
  {
    return m_dbusData;
  }

  // profile management methods
  UccProfile getCurrentProfile() const;
  bool setCurrentProfileByName( const std::string &profileName );
  bool setCurrentProfileById( const std::string &id );
  bool applyProfileJSON( const std::string &profileJSON );
  std::vector< UccProfile > getAllProfiles() const;
  std::vector< UccProfile > getDefaultProfiles() const;
  std::vector< UccProfile > getCustomProfiles() const;
  UccProfile getDefaultProfile() const;
  void updateDBusActiveProfileData();
  void updateDBusSettingsData();

  // profile manipulation methods
  bool addCustomProfile( const UccProfile &profile );
  bool deleteCustomProfile( const std::string &profileId );
  bool updateCustomProfile( const UccProfile &profile );

  // Allow UccDBusInterfaceAdaptor to access private members
  friend class UccDBusInterfaceAdaptor;

public:
  /// Register the D-Bus service.  Call from the main thread before start().
  bool initDBus();

  /// Resolve and apply the startup profile for the current power state.
  /// Call after initDBus() and before start().
  void initializeStartupProfile();

  /// Gracefully stop all worker threads and the service itself.
  /// Call before destruction to ensure a timely shutdown.
  void shutdown();

  void onStart() override;

protected:
  void onWork() override;
  void onExit() override;

private:
  struct BuiltinGpuProfile
  {
    std::string id;
    std::string name;
    std::string json;
  };

  static constexpr const char* INTERFACE_NAME = "com.uniwill.uccd";
  UccDBusData m_dbusData;
  TuxedoIOAPI m_io;
  std::unique_ptr< UccDBusObject > m_dbusObject;  // The QObject registered on the D-Bus bus
  std::unique_ptr< UccDBusInterfaceAdaptor > m_adaptor;
  bool m_started;

  // profile management
  ProfileManager m_profileManager;
  SettingsManager m_settingsManager;
  AutosaveManager m_autosaveManager;
  TccSettings m_settings;
  TccAutosave m_autosave;
  UccProfile m_activeProfile;
  std::vector< UccProfile > m_defaultProfiles;
  std::vector< UccProfile > m_customProfiles;
  std::vector< BuiltinGpuProfile > m_builtinGpuProfiles;

  // state switching
  ProfileState m_currentState;
  std::string m_currentStateProfileId;

  // water cooler state tracking
  bool m_previousWaterCoolerConnected;
  std::atomic< int32_t > m_waterCoolerLedMode{ 0 };  // Tracks the GUI-requested LED mode (may be Temperature)

  // Pump hysteresis: on the way up the table threshold is used directly;
  // on the way down the pump only steps down once the temperature has fallen
  // at least PUMP_HYSTERESIS_DEG below the threshold that caused the last step-up.
  static constexpr int PUMP_HYSTERESIS_DEG = 3;
  int m_pumpHysSpeedIdx{ 0 };    // last applied speed index (0=Off … 4=V12)
  int m_pumpHysThreshold{ 0 };   // table entryTemp that last triggered a step-up

  // EWMA filter for the temperature fed to water-cooler fan + pump auto-control.
  // The FanControlWorker has its own EWMA per fan, but the WC callback receives
  // the raw sensor reading.  This filter smooths it with the same asymmetric
  // weights (fast rise, slow fall) so the pump doesn't bounce on noisy sensors.
  double m_wcTempFiltered{ -1.0 };
  static constexpr double WC_TEMP_ALPHA_RISING  = 0.5;
  static constexpr double WC_TEMP_ALPHA_FALLING = 0.15;

  // Water cooler debounce – avoids reacting to brief BLE connect/disconnect
  // glitches that cause rapid power-state oscillation.
  bool m_wcDebouncePending = false;
  bool m_wcDebouncedTarget = false;                           // the state we are debouncing towards
  std::chrono::steady_clock::time_point m_wcDebounceStart{};  // when the pending change was first seen
  static constexpr int WC_CONNECT_DEBOUNCE_S    = 3;          // seconds stable before accepting "connected"
  static constexpr int WC_DISCONNECT_DEBOUNCE_S = 10;         // seconds stable before accepting "disconnected"

  void setupGpuDataCallback();
  void rebuildBuiltinGpuProfiles();
  int readCurrentCTGPOffset() const;
  void readHardwareCapabilities();
  void updateFanData();
  void loadProfiles();
  void loadSettings();
  void applyStartupProfile();
  void loadAutosave();
  void saveAutosave();
  void initializeProfiles();
  void initializeDisplayModes();
  void serializeProfilesJSON();
  void applyProfileForCurrentState();
  void applyFanAndPumpSettings( const UccProfile &profile );
  void applyGpuOCFromProfile( const UccProfile &profile );
  void fillDeviceSpecificDefaults( std::vector< UccProfile > &profiles );
  void snapProfileFrequencies( UccProfile &profile );
  std::optional< UniwillDeviceID > identifyDevice();
  void computeDeviceCapabilities();
  bool syncOutputPortsSetting();
  std::vector< std::vector< std::string > > getOutputPorts();

  // workers
  std::unique_ptr< HardwareMonitorWorker > m_hardwareMonitorWorker;
  std::unique_ptr< DisplayWorker > m_displayWorker;
  std::unique_ptr< CpuWorker > m_cpuWorker;
  std::unique_ptr< ProfileSettingsWorker > m_profileSettingsWorker;
  std::unique_ptr< FanControlWorker > m_fanControlWorker;
  KeyboardBacklightController m_keyboardBacklightController;
  std::unique_ptr< LCTWaterCoolerWorker > m_waterCoolerWorker;
  std::unique_ptr< NvidiaOCWorker > m_nvidiaOCWorker;

  // Shared NVML instance — created once, used by all workers and readHardwareCapabilities
  std::shared_ptr< NvmlWrapper > m_nvml;

  // identified device
  std::optional< UniwillDeviceID > m_deviceId;
  SystemInfo m_systemInfo;

  // periodic validation counters
  uint32_t m_nvidiaValidationCounter = 0;
  bool m_nvidiaPowerLimitsInitialized = false;

  // monitoring history ring buffer (daemon-side storage for graph tab)
  MetricsHistoryStore m_metricsStore;

  // controllers
  FnLockController m_fnLockController;

  static constexpr const char *SERVICE_NAME = "com.uniwill.uccd";
  static constexpr const char *OBJECT_PATH = "/com/uniwill/uccd";
};
