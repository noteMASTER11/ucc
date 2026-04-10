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

#include "UccDBusService.hpp"
#include "CommonTypes.hpp"
#include "NvmlWrapper.hpp"
#include "profiles/DefaultProfiles.hpp"
#include "profiles/FanProfile.hpp"
#include "PolkitAuthority.hpp"
#include "StateUtils.hpp"
#include "Utils.hpp"
#include "SysfsNode.hpp"
#include <sstream>
#include <iomanip>
#include <map>
#include <thread>
#include <cmath>
#include <climits>
#include <fstream>
#include <filesystem>
#include <syslog.h>
#include <libudev.h>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QEventLoop>

namespace
{
constexpr const char *BUILTIN_GPU_PROFILE_ID = "gpu-default-builtin";
constexpr const char *BUILTIN_GPU_PROFILE_NAME = "Default [Built-in]";
}

static std::string jsonEscape( const std::string &value );

// helper function to convert GPU info to JSON
std::string dgpuInfoToJSON( const DGpuInfo &info )
{
  std::ostringstream oss;
  oss << std::fixed << std::setprecision( 2 );
  oss << "{"
  << "\"temp\":" << info.m_temp << ","
      << "\"coreFrequency\":" << info.m_coreFrequency << ","
      << "\"vramFrequency\":" << info.m_vramFrequency << ","
      << "\"maxCoreFrequency\":" << info.m_maxCoreFrequency << ","
      << "\"powerDraw\":" << info.m_powerDraw << ","
      << "\"maxPowerLimit\":" << info.m_maxPowerLimit << ","
      << "\"enforcedPowerLimit\":" << info.m_enforcedPowerLimit << ","
      << "\"computeUtilPct\":" << info.m_computeUtilPct << ","
      << "\"memoryUtilPct\":" << info.m_memoryUtilPct << ","
      << "\"vramUsedMiB\":" << info.m_vramUsedMiB << ","
      << "\"vramTotalMiB\":" << info.m_vramTotalMiB << ","
      << "\"perfLimitReason\":\"" << jsonEscape( info.m_perfLimitReason ) << "\","
      << "\"encoderUtilPct\":" << info.m_encoderUtilPct << ","
      << "\"decoderUtilPct\":" << info.m_decoderUtilPct << ","
      << "\"currentPstate\":" << info.m_currentPstate << ","
      << "\"grClockOffsetMHz\":" << ( info.m_grClockOffsetMHz == INT_MIN ? -999 : info.m_grClockOffsetMHz ) << ","
      << "\"memClockOffsetMHz\":" << ( info.m_memClockOffsetMHz == INT_MIN ? -999 : info.m_memClockOffsetMHz ) << ","
      << "\"coreVoltageMv\":" << info.m_coreVoltageMv << ","
      << "\"d0MetricsUsage\":" << ( info.m_d0MetricsUsage ? "true" : "false" )
      << "}";
  return oss.str();
}

std::string igpuInfoToJSON( const IGpuInfo &info )
{
  std::ostringstream oss;
  oss << std::fixed << std::setprecision( 2 );
  oss << "{"
      << "\"temp\":" << info.m_temp << ","
      << "\"coreFrequency\":" << info.m_coreFrequency << ","
      << "\"maxCoreFrequency\":" << info.m_maxCoreFrequency << ","
      << "\"powerDraw\":" << info.m_powerDraw << ","
      << "\"vendor\":\"" << info.m_vendor << "\""
      << "}";
  return oss.str();
}

static std::string jsonEscape( const std::string &value )
{
  std::ostringstream oss;
  for ( const char c : value )
  {
    switch ( c )
    {
      case '"': oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default: oss << c; break;
    }
  }
  return oss.str();
}


static int32_t getDefaultOnlineCores()
{
  const auto cores = std::thread::hardware_concurrency();
  return cores > 0 ? static_cast< int32_t >( cores ) : -1;
}

static int32_t readSysFsInt( const std::string &path, int32_t defaultValue )
{
  std::ifstream file( path );
  if ( !file.is_open() )
    return defaultValue;

  int32_t value;
  if ( !( file >> value ) )
    return defaultValue;

  return value;
}

static int32_t getCpuMinFrequency()
{
  // Read from cpu0 cpuinfo_min_freq
  return readSysFsInt( "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", -1 );
}

static int32_t getCpuMaxFrequency()
{
  // Read from cpu0 cpuinfo_max_freq
  return readSysFsInt( "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", -1 );
}

static int32_t optionalValueOr( const std::optional< int32_t > &value, int32_t fallback )
{
  return value.has_value() ? value.value() : fallback;
}

static std::string profileToJSON( const UccProfile &profile,
                                  int32_t defaultOnlineCores,
                                  int32_t defaultScalingMin,
                                  int32_t defaultScalingMax )
{
  std::ostringstream oss;
  oss << "{"
      << "\"id\":\"" << jsonEscape( profile.id ) << "\" ,"
      << "\"name\":\"" << jsonEscape( profile.name ) << "\" ,"
      << "\"description\":\"" << jsonEscape( profile.description ) << "\" ,"
      << "\"display\":{"
      << "\"brightness\":" << profile.display.brightness << ","
      << "\"useBrightness\":" << ( profile.display.useBrightness ? "true" : "false" ) << ","
      << "\"refreshRate\":" << profile.display.refreshRate << ","
      << "\"useRefRate\":" << ( profile.display.useRefRate ? "true" : "false" ) << ","
      << "\"xResolution\":" << profile.display.xResolution << ","
      << "\"yResolution\":" << profile.display.yResolution << ","
      << "\"useResolution\":" << ( profile.display.useResolution ? "true" : "false" )
      << "},"
      << "\"cpu\":{"
      << "\"governor\":\"" << jsonEscape( profile.cpu.governor ) << "\" ,"
      << "\"energyPerformancePreference\":\"" << jsonEscape( profile.cpu.energyPerformancePreference ) << "\" ,"
      << "\"noTurbo\":" << ( profile.cpu.noTurbo ? "true" : "false" ) << ","
      << "\"onlineCores\":" << optionalValueOr( profile.cpu.onlineCores, defaultOnlineCores ) << ","
      << "\"scalingMinFrequency\":" << optionalValueOr( profile.cpu.scalingMinFrequency, defaultScalingMin ) << ","
      << "\"scalingMaxFrequency\":" << optionalValueOr( profile.cpu.scalingMaxFrequency, defaultScalingMax )
      << "},"
      << "\"webcam\":{"
      << "\"status\":" << ( profile.webcam.status ? "true" : "false" ) << ","
      << "\"useStatus\":" << ( profile.webcam.useStatus ? "true" : "false" )
      << "},"
      << "\"fan\":{"
      << "\"useControl\":" << ( profile.fan.useControl ? "true" : "false" ) << ","
      << "\"fanProfile\":\"" << jsonEscape( profile.fan.fanProfile ) << "\" ,"
      << "\"sameSpeed\":" << ( profile.fan.sameSpeed ? "true" : "false" ) << ","
      << "\"autoControlWC\":" << ( profile.fan.autoControlWC ? "true" : "false" ) << ","
      << "\"enableWaterCooler\":" << ( profile.fan.enableWaterCooler ? "true" : "false" );

  // Embed fan tables if present
  if ( !profile.fan.tableCPU.empty() )
    oss << ",\"tableCPU\":" << ProfileManager::fanTableToJSON( profile.fan.tableCPU );
  if ( !profile.fan.tableGPU.empty() )
    oss << ",\"tableGPU\":" << ProfileManager::fanTableToJSON( profile.fan.tableGPU );
  if ( !profile.fan.tablePump.empty() )
    oss << ",\"tablePump\":" << ProfileManager::fanTableToJSON( profile.fan.tablePump );
  if ( !profile.fan.tableWaterCoolerFan.empty() )
    oss << ",\"tableWaterCoolerFan\":" << ProfileManager::fanTableToJSON( profile.fan.tableWaterCoolerFan );

  oss << "},"
      << "\"odmProfile\":{"
      << "\"name\":\"" << jsonEscape( profile.odmProfile.name.value_or( "" ) ) << "\""
      << "},"
      << "\"odmPowerLimits\":{"
      << "\"tdpValues\":[";

  for ( size_t i = 0; i < profile.odmPowerLimits.tdpValues.size(); ++i )
  {
    if ( i > 0 )
      oss << ",";
    oss << profile.odmPowerLimits.tdpValues[ i ];
  }

  oss << "]}";

  // GPU OC profile reference and embedded data
  if ( !profile.gpuProfileId.empty() )
  {
    oss << ",\"gpuProfileId\":\"" << jsonEscape( profile.gpuProfileId ) << "\"";
  }
  if ( !profile.gpuOCProfileData.empty() && profile.gpuOCProfileData != "{}" )
  {
    oss << ",\"gpuOCProfileData\":" << profile.gpuOCProfileData;
  }

  // Keyboard section
  if ( !profile.keyboard.keyboardProfileData.empty() && profile.keyboard.keyboardProfileData != "{}" )
  {
    oss << ",\"keyboard\":" << profile.keyboard.keyboardProfileData;
  }
  else
  {
    oss << ",\"keyboard\":{}";
  }

  // Prefer UUID over display name — tray/GUI use the ID for combo-box indexing
  {
    const std::string &kbRef = !profile.keyboard.keyboardProfileId.empty()
                              ? profile.keyboard.keyboardProfileId
                              : profile.keyboard.keyboardProfileName;
    if ( !kbRef.empty() )
      oss << ",\"selectedKeyboardProfile\":\"" << jsonEscape( kbRef ) << "\"";
  }

  // Charging profile (firmware-level mode, per-profile)
  if ( !profile.chargingProfile.empty() )
  {
    oss << ",\"chargingProfile\":\"" << jsonEscape( profile.chargingProfile ) << "\"";
  }
  if ( !profile.chargingPriority.empty() )
  {
    oss << ",\"chargingPriority\":\"" << jsonEscape( profile.chargingPriority ) << "\"";
  }
  if ( !profile.chargeType.empty() )
  {
    oss << ",\"chargeType\":\"" << jsonEscape( profile.chargeType ) << "\"";
  }
  if ( profile.chargeStartThreshold >= 0 )
  {
    oss << ",\"chargeStartThreshold\":" << profile.chargeStartThreshold;
  }
  if ( profile.chargeEndThreshold >= 0 )
  {
    oss << ",\"chargeEndThreshold\":" << profile.chargeEndThreshold;
  }

  oss << "}";

  return oss.str();
}


static std::string buildSettingsJSON( const std::string &keyboardBacklightStatesJSON,
                                      const std::string &chargingProfile,
                                      const TccSettings &settings )
{
  std::ostringstream oss;
  oss << "{"
      << "\"fahrenheit\":" << ( settings.fahrenheit ? "true" : "false" ) << ","
      << "\"stateMap\":{";

  // Serialize stateMap
  bool first = true;
  for ( const auto &[key, value] : settings.stateMap )
  {
    if ( !first )
      oss << ",";
    first = false;
    oss << "\"" << jsonEscape( key ) << "\":\"" << jsonEscape( value ) << "\"";
  }

  oss << "},"
      << "\"shutdownTime\":" << ( settings.shutdownTime.has_value() ? "\"" + jsonEscape( *settings.shutdownTime ) + "\"" : "null" ) << ","
      << "\"cpuSettingsEnabled\":" << ( settings.cpuSettingsEnabled ? "true" : "false" ) << ","
      << "\"fanControlEnabled\":" << ( settings.fanControlEnabled ? "true" : "false" ) << ","
      << "\"keyboardBacklightControlEnabled\":" << ( settings.keyboardBacklightControlEnabled ? "true" : "false" ) << ","
      << "\"ycbcr420Workaround\":[],"
      << "\"chargingProfile\":\"" << jsonEscape( chargingProfile ) << "\" ,"
      << "\"chargingPriority\":" << ( settings.chargingPriority.has_value() ? "\"" + jsonEscape( *settings.chargingPriority ) + "\"" : "null" ) << ","
      << "\"keyboardBacklightStates\":" << keyboardBacklightStatesJSON
      << "}";
  return oss.str();
}

// UccDBusInterfaceAdaptor implementation

UccDBusInterfaceAdaptor::UccDBusInterfaceAdaptor( QObject *parent,
                                                  UccDBusData &data,
                                                  UccDBusService *service )
  : QDBusAbstractAdaptor( parent ),
    m_data( data ),
    m_service( service ),
    m_lastDataCollectionAccess( std::chrono::steady_clock::now() )
{
  // Qt's MOC handles introspection and method dispatch automatically
  // via Q_CLASSINFO and public slots declarations
  setAutoRelaySignals( true );
  syslog( LOG_INFO, "UccDBusInterfaceAdaptor: registered interface %s", UccDBusInterfaceAdaptor::INTERFACE_NAME );
}

bool UccDBusInterfaceAdaptor::checkAuth( const char *actionId ) noexcept
{
  auto *dbusObj = qobject_cast< UccDBusObject * >( parent() );
  if ( not dbusObj )
  {
    syslog( LOG_ERR, "PolkitAuth: parent is not UccDBusObject" );
    return false;
  }
  const QString methodName = dbusObj->message().member();
  std::cerr << "[PolkitAuth] method='" << methodName.toStdString()
            << "' action='" << actionId << "'\n";
  return PolkitAuthority::checkAuthorization(
      dbusObj->connection(), dbusObj->message(), actionId );
}


void UccDBusInterfaceAdaptor::resetDataCollectionTimeout()
{
  // Note: caller must hold m_data.dataMutex lock (for m_lastDataCollectionAccess)
  m_lastDataCollectionAccess = std::chrono::steady_clock::now();
  m_data.sensorDataCollectionStatus = true;
}

QVariantMap
UccDBusInterfaceAdaptor::exportFanData( const FanData &fanData )
{
  // Cast int64_t → qlonglong and int32_t → int so QtDBus can marshal them.
  // int64_t is 'long' on x86_64 Linux which QtDBus does not recognise,
  // whereas qlonglong ('long long') maps to D-Bus 'x' (INT64).
  QVariantMap speedData;
  speedData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( fanData.speed.timestamp ) );
  speedData[ "data" ] = QVariant::fromValue( static_cast< int >( fanData.speed.data ) );

  QVariantMap tempData;
  tempData[ "timestamp" ] = QVariant::fromValue( static_cast< qlonglong >( fanData.temp.timestamp ) );
  tempData[ "data" ] = QVariant::fromValue( static_cast< int >( fanData.temp.data ) );

  QVariantMap result;
  result[ "speed" ] = QVariant::fromValue( speedData );
  result[ "temp" ] = QVariant::fromValue( tempData );
  return result;
}

// device and system information methods

QString UccDBusInterfaceAdaptor::GetDeviceName()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.device );
}

QString UccDBusInterfaceAdaptor::GetSystemInfoJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.systemInfoJSON );
}

bool UccDBusInterfaceAdaptor::IsDeviceSupported()
{
  return m_data.deviceSupported.load();
}

QString UccDBusInterfaceAdaptor::GetDisplayModesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.displayModes );
}

bool UccDBusInterfaceAdaptor::GetIsX11()
{
  return m_data.isX11;
}

bool UccDBusInterfaceAdaptor::TuxedoWmiAvailable()
{
  return m_data.tuxedoWmiAvailable;
}

bool UccDBusInterfaceAdaptor::FanHwmonAvailable()
{
  return m_data.fanHwmonAvailable;
}

QString UccDBusInterfaceAdaptor::UccdVersion()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.uccdVersion );
}

// fan data methods

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataCPU()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  if ( m_data.fans.size() > 0 )
    return exportFanData( m_data.fans[ 0 ] );

  return {};
}

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataGPU1()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  if ( m_data.fans.size() > 1 )
    return exportFanData( m_data.fans[ 1 ] );

  return {};
}

QVariantMap
UccDBusInterfaceAdaptor::GetFanDataGPU2()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  if ( m_data.fans.size() > 2 )
    return exportFanData( m_data.fans[ 2 ] );

  return {};
}

// webcam and display methods

bool UccDBusInterfaceAdaptor::WebcamSWAvailable()
{
  return m_data.webcamSwitchAvailable;
}

bool UccDBusInterfaceAdaptor::GetWebcamSWStatus()
{
  return m_data.webcamSwitchStatus;
}

bool UccDBusInterfaceAdaptor::GetForceYUV420OutputSwitchAvailable()
{
  return m_data.forceYUV420OutputSwitchAvailable;
}

bool UccDBusInterfaceAdaptor::SetDisplayRefreshRate( const QString &display, int refreshRate )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Note: display parameter is currently ignored - only works with primary display
  // TODO: Support multiple displays in the future
  (void)display;

  if ( m_service && m_service->m_displayWorker )
  {
    return m_service->m_displayWorker->setRefreshRate( refreshRate );
  }

  return false;
}

int UccDBusInterfaceAdaptor::GetDisplayBrightness()
{
  if ( m_service )
  {
    // return autosave-stored brightness (percent)
    return m_service->m_autosave.displayBrightness;
  }
  return -1;
}

bool UccDBusInterfaceAdaptor::SetDisplayBrightness( int brightness )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service )
  {
    // clamp
    if ( brightness < 0 ) brightness = 0;
    if ( brightness > 100 ) brightness = 100;

    // update autosave
    m_service->m_autosave.displayBrightness = brightness;

    // try to apply immediately via DisplayWorker if available
    if ( m_service->m_displayWorker )
    {
      return m_service->m_displayWorker->setBrightness( brightness );
    }

    // if no worker, still return true (autosave updated)
    return true;
  }
  return false;
}

// gpu information methods

QString UccDBusInterfaceAdaptor::GetDGpuInfoValuesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  resetDataCollectionTimeout();
  return QString::fromStdString( m_data.dGpuInfoValuesJSON );
}

QString UccDBusInterfaceAdaptor::GetIGpuInfoValuesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  resetDataCollectionTimeout();
  return QString::fromStdString( m_data.iGpuInfoValuesJSON );
}

QString UccDBusInterfaceAdaptor::GetCpuPowerValuesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  resetDataCollectionTimeout();
  return QString::fromStdString( m_data.cpuPowerValuesJSON );
}

// graphics methods

QString UccDBusInterfaceAdaptor::GetPrimeState()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.primeState );
}

bool UccDBusInterfaceAdaptor::ConsumeModeReapplyPending()
{
  return m_data.modeReapplyPending.exchange( false );
}

// profile methods

QString UccDBusInterfaceAdaptor::GetActiveProfileJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.activeProfileJSON );
}

bool UccDBusInterfaceAdaptor::SetFanProfileCPU( const QString &pointsJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( !m_service )
    return false;

  const std::string json = pointsJSON.toStdString();
  std::cerr << "[DBus] SetFanProfileCPU called with JSON: " << json << std::endl;

  try
  {
    auto table = ProfileManager::parseFanTableFromJSON( json );
    std::cerr << "[DBus] Parsed table size: " << table.size() << std::endl;
    if ( table.size() != 17 )
      return false;

    UccProfile profile = m_service->getCurrentProfile();
    std::cerr << "[DBus] Current profile ID: " << profile.id << std::endl;
    auto custom = m_service->getCustomProfiles();
    bool editable = false;
    for ( const auto &p : custom )
    {
      if ( p.id == profile.id ) { editable = true; break; }
    }
    std::cerr << "[DBus] Profile editable: " << editable << std::endl;
    if ( !editable )
      return false;

    // Apply as temporary table (do not persist in daemon profiles)
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->applyTemporaryFanCurves( table, {} );
      return true;
    }
    return false;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::SetFanProfileDGPU( const QString &pointsJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( !m_service )
    return false;

  const std::string json = pointsJSON.toStdString();
  std::cerr << "[DBus] SetFanProfileDGPU called with JSON: " << json << std::endl;

  try
  {
    auto table = ProfileManager::parseFanTableFromJSON( json );
    std::cerr << "[DBus] Parsed table size: " << table.size() << std::endl;
    if ( table.size() != 17 )
      return false;

    UccProfile profile = m_service->getCurrentProfile();
    std::cerr << "[DBus] Current profile ID: " << profile.id << std::endl;
    auto custom = m_service->getCustomProfiles();
    bool editable = false;
    for ( const auto &p : custom )
    {
      if ( p.id == profile.id ) { editable = true; break; }
    }
    std::cerr << "[DBus] Profile editable: " << editable << std::endl;
    if ( !editable )
      return false;

    // Apply as temporary table (do not persist in daemon profiles)
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->applyTemporaryFanCurves( {}, table );
      return true;
    }
    return false;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::ApplyFanProfiles( const QString &fanProfilesJSONq )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( not m_service )
    return false;

  const std::string fanProfilesJSON = fanProfilesJSONq.toStdString();

  try
  {
    // Helper: extract a JSON array by key name
    auto extractArray = [&]( const std::string &key ) -> std::string {
      std::string search = "\"" + key + "\":";
      size_t pos = fanProfilesJSON.find( search );
      if ( pos == std::string::npos ) return {};
      size_t bracketStart = fanProfilesJSON.find( '[', pos );
      if ( bracketStart == std::string::npos ) return {};
      int depth = 0;
      for ( size_t i = bracketStart; i < fanProfilesJSON.length(); ++i )
      {
        if ( fanProfilesJSON[i] == '[' ) ++depth;
        else if ( fanProfilesJSON[i] == ']' ) --depth;
        if ( depth == 0 )
          return fanProfilesJSON.substr( bracketStart, i - bracketStart + 1 );
      }
      return {};
    };

    auto parseTable = [&]( const std::string &key ) -> std::vector< FanTableEntry > {
      std::string json = extractArray( key );
      if ( json.empty() ) return {};
      auto table = ProfileManager::parseFanTableFromJSON( json );
      std::cerr << "[DBus] Parsed " << key << " table size: " << table.size() << std::endl;
      return table;
    };

    auto cpuTable            = parseTable( "cpu" );
    auto gpuTable            = parseTable( "gpu" );
    auto waterCoolerFanTable = parseTable( "waterCoolerFan" );
    auto pumpTable           = parseTable( "pump" );

    // Apply the temporary fan curves
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, waterCoolerFanTable, pumpTable );
      std::cerr << "[DBus] Applied temporary fan profiles" << std::endl;
    }

    // If the caller provided a fan profile ID, update the active profile
    // reference and notify all clients so they stay in sync.
    {
      auto extractStr = []( const std::string &json, const std::string &key ) -> std::string {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find( search );
        if ( pos == std::string::npos ) return {};
        pos += search.length();
        size_t end = json.find( '"', pos );
        if ( end == std::string::npos ) return {};
        return json.substr( pos, end - pos );
      };
      std::string fanProfileId = extractStr( fanProfilesJSON, "fanProfileId" );
      if ( !fanProfileId.empty() )
      {
        m_service->m_activeProfile.fan.fanProfile = fanProfileId;
        m_service->updateDBusActiveProfileData();
        if ( m_service->m_adaptor )
          emitProfileChanged( m_service->m_activeProfile.id,
                              m_service->m_activeProfile.keyboard.keyboardProfileId,
                              fanProfileId );
      }
    }

    return true;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::RevertFanProfiles()
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( !m_service )
    return false;

  std::cerr << "[DBus] RevertFanProfiles called" << std::endl;

  try
  {
    // Clear temporary fan curves by resetting the flag and reloading profile
    if ( m_service->m_fanControlWorker )
    {
      m_service->m_fanControlWorker->clearTemporaryCurves();
      std::cerr << "[DBus] Cleared temporary fan curves" << std::endl;
    }

    // Reload the current profile to reset fan logics
    auto profile = m_service->getCurrentProfile();
    // The onWork method will call updateFanLogicsFromProfile which will now use profile curves

    return true;
  }
  catch ( ... )
  {
    return false;
  }
}

bool UccDBusInterfaceAdaptor::SetTempProfile( const QString &profileName )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  m_data.tempProfileName = profileName.toStdString();
  return true;
}

bool UccDBusInterfaceAdaptor::SetActiveProfile( const QString &id )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Immediately set the active profile
  return m_service->setCurrentProfileById( id.toStdString() );
}

bool UccDBusInterfaceAdaptor::ApplyProfile( const QString &profileJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  // Apply the profile configuration sent by the GUI
  return m_service->applyProfileJSON( profileJSON.toStdString() );
}



QString UccDBusInterfaceAdaptor::GetProfilesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.profilesJSON );
}

QString UccDBusInterfaceAdaptor::GetCustomProfilesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  //std::cout << "[DBus] GetCustomProfilesJSON called, returning "
  //          << m_data.customProfilesJSON.length() << " bytes" << std::endl;
  return QString::fromStdString( m_data.customProfilesJSON );
}

QString UccDBusInterfaceAdaptor::GetDefaultProfilesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.defaultProfilesJSON );
}

QString UccDBusInterfaceAdaptor::GetCpuFrequencyLimitsJSON()
{
  const int32_t minFreq = getCpuMinFrequency();
  const int32_t maxFreq = getCpuMaxFrequency();

  std::ostringstream json;
  json << "{\"min\":" << minFreq << ",\"max\":" << maxFreq << "}";
  return QString::fromStdString( json.str() );
}

QString UccDBusInterfaceAdaptor::GetDefaultValuesProfileJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.defaultValuesProfileJSON );
}

bool UccDBusInterfaceAdaptor::AddCustomProfile( const QString &profileJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service )
  {
    std::cerr << "[Profile] AddCustomProfile called but service not available" << std::endl;
    return false;
  }

  try
  {
    // Parse the profile JSON and add it
    auto profile = ProfileManager::parseProfileJSON( profileJSON.toStdString() );

    // Generate new ID if empty
    if ( profile.id.empty() )
    {
      profile.id = generateProfileId();
    }

    std::cout << "[Profile] Adding custom profile '" << profile.name
              << "' (id: " << profile.id << ")" << std::endl;

    bool result = m_service->addCustomProfile( profile );

    if ( result )
    {
      std::cout << "[Profile] Successfully added profile '" << profile.name << "'" << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to add profile '" << profile.name << "'" << std::endl;
    }

    return result;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Exception in AddCustomProfile: " << e.what() << std::endl;
    return false;
  }
  catch ( ... )
  {
    std::cerr << "[Profile] Unknown exception in AddCustomProfile" << std::endl;
    return false;
  }
}

bool UccDBusInterfaceAdaptor::DeleteCustomProfile( const QString &profileId )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service )
  {
    std::cerr << "[Profile] DeleteCustomProfile called but service not available" << std::endl;
    return false;
  }

  const std::string id = profileId.toStdString();
  std::cout << "[Profile] Deleting custom profile with id: " << id << std::endl;

  bool result = m_service->deleteCustomProfile( id );

  if ( result )
  {
    std::cout << "[Profile] Successfully deleted profile '" << id << "'" << std::endl;
  }
  else
  {
    std::cerr << "[Profile] Failed to delete profile '" << id << "' (not found or error)" << std::endl;
  }

  return result;
}

bool UccDBusInterfaceAdaptor::UpdateCustomProfile( const QString &profileJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service )
  {
    std::cerr << "[Profile] UpdateCustomProfile called but service not available" << std::endl;
    return false;
  }

  try
  {
    const std::string jsonStr = profileJSON.toStdString();
    std::cout << "[Profile] Received profile JSON (first 200 chars): "
              << jsonStr.substr(0, 200) << "..." << std::endl;

    // Parse the profile JSON and update it
    auto profile = ProfileManager::parseProfileJSON( jsonStr );

    if ( profile.id.empty() )
    {
      std::cerr << "[Profile] UpdateCustomProfile called with empty profile ID" << std::endl;
      return false; // Must have an ID to update
    }

    std::cout << "[Profile] Updating custom profile '" << profile.name
              << "' (id: " << profile.id << ")" << std::endl;
    std::cout << "[Profile]   Fan control: " << (profile.fan.useControl ? "enabled" : "disabled") << std::endl;
    std::cout << "[Profile]   Fan profile: " << profile.fan.fanProfile << std::endl;
    std::cout << "[Profile]   Auto control WC: " << (profile.fan.autoControlWC ? "enabled" : "disabled") << std::endl;
    std::cout << "[Profile]   Fan profile name: " << profile.fan.fanProfile << std::endl;

    bool result = m_service->updateCustomProfile( profile );

    if ( result )
    {
      std::cout << "[Profile] Successfully updated profile '" << profile.name << "'" << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to update profile '" << profile.name << "' (not found or error)" << std::endl;
    }

    return result;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Exception in UpdateCustomProfile: " << e.what() << std::endl;
    return false;
  }
  catch ( ... )
  {
    std::cerr << "[Profile] Unknown exception in UpdateCustomProfile" << std::endl;
    return false;
  }
}

bool UccDBusInterfaceAdaptor::SaveCustomProfile( const QString &profileJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( !m_service )
  {
    std::cerr << "[Profile] SaveCustomProfile called but service not available" << std::endl;
    return false;
  }

  try
  {
    const std::string jsonStr = profileJSON.toStdString();
    std::cout << "[Profile] Received SaveCustomProfile JSON (first 200 chars): "
              << jsonStr.substr(0, 200) << "..." << std::endl;

    // Parse the profile JSON
    auto profile = ProfileManager::parseProfileJSON( jsonStr );

    // Check if name collides with a built-in profile
    for ( const auto &builtIn : m_service->m_defaultProfiles )
    {
      if ( builtIn.name == profile.name )
      {
        std::cerr << "[Profile] SaveCustomProfile: name '" << profile.name
                  << "' collides with built-in profile, rejected" << std::endl;
        return false;
      }
    }

    // Check if a profile with the same name already exists in memory
    auto existingProfileIt = std::ranges::find_if( m_service->m_customProfiles,
                                          [&profile]( const UccProfile &p ) { return p.name == profile.name; } );

    // Also check settings for profiles with the same name (in case parsing failed on load)
    std::string existingIdFromSettings;
    for ( const auto &[profileId, profileJson] : m_service->m_settings.profiles )
    {
      try
      {
        auto parsedProfile = ProfileManager::parseProfileJSON( profileJson );
        if ( parsedProfile.name == profile.name )
        {
          existingIdFromSettings = profileId;
          break;
        }
      }
      catch ( const std::exception &e )
      {
        // Skip malformed entries
        continue;
      }
    }

    bool result;
    if ( existingProfileIt != m_service->m_customProfiles.end() )
    {
      // Profile with same name exists in memory
      // Check if they have the SAME ID (genuine update) or DIFFERENT ID (name collision)
      if ( profile.id == existingProfileIt->id )
      {
        // Same profile, update it
        std::cout << "[Profile] SaveCustomProfile: updating existing profile '" << profile.name << "' (id: " << profile.id << ")" << std::endl;
        result = m_service->updateCustomProfile( profile );
      }
      else if ( !profile.id.empty() )
      {
        // Different ID but same name - GUI sent a new profile with same name
        // Respect the GUI's ID, don't overwrite it with the old one
        std::cout << "[Profile] SaveCustomProfile: received profile with new ID '" << profile.id << "' but same name as existing profile (id: " << existingProfileIt->id << ")" << std::endl;
        std::cout << "[Profile] Treating as NEW profile since IDs differ" << std::endl;
        result = m_service->addCustomProfile( profile );
      }
      else
      {
        // Received profile has no ID but same name exists - assign old ID
        profile.id = existingProfileIt->id;
        std::cout << "[Profile] SaveCustomProfile: received profile with no ID, using existing ID '" << profile.id << "'" << std::endl;
        result = m_service->updateCustomProfile( profile );
      }
    }
    else if ( !existingIdFromSettings.empty() )
    {
      // Profile with same name exists in settings but not in memory
      // Check if ID matches
      if ( profile.id == existingIdFromSettings )
      {
        // Same profile, reuse the ID
        std::cout << "[Profile] SaveCustomProfile: updating existing profile '" << profile.name << "' from settings (id: " << profile.id << ")" << std::endl;
        m_service->m_customProfiles.push_back( profile );
        result = true;
      }
      else if ( !profile.id.empty() )
      {
        // Different ID but same name - treat as new profile
        std::cout << "[Profile] SaveCustomProfile: received profile with new ID '" << profile.id << "' but same name in settings (id: " << existingIdFromSettings << ")" << std::endl;
        std::cout << "[Profile] Treating as NEW profile since IDs differ" << std::endl;
        result = m_service->addCustomProfile( profile );
      }
      else
      {
        // No ID provided, use the one from settings
        profile.id = existingIdFromSettings;
        std::cout << "[Profile] SaveCustomProfile: received profile with no ID, using existing ID from settings '" << profile.id << "'" << std::endl;
        m_service->m_customProfiles.push_back( profile );
        result = true;
      }
    }
    else
    {
      // No profile with this name exists, add as new
      if ( profile.id.empty() )
      {
        profile.id = generateProfileId();
        std::cout << "[Profile] SaveCustomProfile: adding new profile '" << profile.name << "' with generated id " << profile.id << std::endl;
      }
      else
      {
        std::cout << "[Profile] SaveCustomProfile: adding new profile '" << profile.name << "' with provided id " << profile.id << std::endl;
      }
      result = m_service->addCustomProfile( profile );
      if ( result )
      {
        std::cout << "[Profile] Successfully added profile '" << profile.name << "'" << std::endl;
      }
      else
      {
        std::cerr << "[Profile] Failed to add profile '" << profile.name << "'" << std::endl;
        return false;
      }
    }

    // Store the profile in settings for persistence using the corrected ID
    std::string storedJSON = ProfileManager::profileToJSON( profile );
    m_service->m_settings.profiles[profile.id] = storedJSON;

    // Clean up old profile entries with same name but different ID
    // This prevents accumulating duplicate profiles in settings
    std::vector<std::string> keysToDelete;
    for ( const auto &[mapKey, mapJson] : m_service->m_settings.profiles )
    {
      if ( mapKey != profile.id )  // Different key
      {
        try
        {
          auto parsedProfile = ProfileManager::parseProfileJSON( mapJson );
          if ( parsedProfile.name == profile.name )  // Same name
          {
            std::cout << "[Settings] Removing old profile entry with key '" << mapKey << "' (name: " << parsedProfile.name << ", same name as new id: " << profile.id << ")" << std::endl;
            keysToDelete.push_back( mapKey );
          }
        }
        catch ( const std::exception &e )
        {
          // Ignore parse errors
          continue;
        }
      }
    }

    // Delete the old entries
    for ( const auto &keyToDelete : keysToDelete )
    {
      m_service->m_settings.profiles.erase( keyToDelete );
    }

    // Also remove from m_customProfiles if it exists with old ID
    for ( auto &memProfile : m_service->m_customProfiles )
    {
      if ( memProfile.name == profile.name && memProfile.id != profile.id )
      {
        std::cout << "[Settings] Removing old profile from memory with id '" << memProfile.id << "' (name: " << memProfile.name << ")" << std::endl;
        // Mark for deletion by swapping with last element
        // Will be erased below
        memProfile.id = "";  // Mark for deletion
      }
    }

    // Remove marked entries
    if ( auto it = std::remove_if( m_service->m_customProfiles.begin(), m_service->m_customProfiles.end(),
                             [](const UccProfile &p) { return p.id.empty(); } );
         it != m_service->m_customProfiles.end() )
    {
      m_service->m_customProfiles.erase( it, m_service->m_customProfiles.end() );
    }

    // Always persist settings after saving a profile
    if ( m_service->m_settingsManager.writeSettings( m_service->m_settings ) )
    {
      std::cout << "[Settings] Settings persisted after saving profile" << std::endl;
    }
    else
    {
      std::cerr << "[Settings] Failed to persist settings after saving profile" << std::endl;
    }

    return result;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Exception in SaveCustomProfile: " << e.what() << std::endl;
    return false;
  }
  catch ( ... )
  {
    std::cerr << "[Profile] Unknown exception in SaveCustomProfile" << std::endl;
    return false;
  }
}

QString UccDBusInterfaceAdaptor::GetFanProfile( const QString &name )
{
  return QString::fromStdString( getFanProfileJson( name.toStdString() ) );
}

QString UccDBusInterfaceAdaptor::GetFanProfileNames()
{
  // Return JSON array of objects with id and name for each built-in fan profile
  std::string json = "[";
  for ( size_t i = 0; i < defaultFanProfiles.size(); ++i )
  {
    if ( i > 0 ) json += ",";
    json += "{\"id\":\"" + defaultFanProfiles[i].id + "\","
            "\"name\":\"" + defaultFanProfiles[i].name + "\"}";
  }
  json += "]";
  return QString::fromStdString( json );
}

QString UccDBusInterfaceAdaptor::GetGpuProfile( const QString &id )
{
  if ( !m_service )
    return QStringLiteral( "{}" );

  const std::string requestedId = id.toStdString();
  auto it = std::find_if( m_service->m_builtinGpuProfiles.begin(),
                          m_service->m_builtinGpuProfiles.end(),
                          [&requestedId]( const UccDBusService::BuiltinGpuProfile &profile ) {
                            return profile.id == requestedId;
                          } );

  if ( it == m_service->m_builtinGpuProfiles.end() )
    return QStringLiteral( "{}" );

  return QString::fromStdString( it->json );
}

QString UccDBusInterfaceAdaptor::GetGpuProfileNames()
{
  if ( !m_service )
    return QStringLiteral( "[]" );

  QJsonArray arr;
  for ( const auto &profile : m_service->m_builtinGpuProfiles )
  {
    QJsonObject obj;
    obj[ "id" ] = QString::fromStdString( profile.id );
    obj[ "name" ] = QString::fromStdString( profile.name );
    arr.append( obj );
  }

  return QString::fromUtf8( QJsonDocument( arr ).toJson( QJsonDocument::Compact ) );
}

bool UccDBusInterfaceAdaptor::SetFanProfile( const QString &name, const QString &json )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  return setFanProfileJson( name.toStdString(), json.toStdString() );
}

// settings methods

QString UccDBusInterfaceAdaptor::GetSettingsJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.settingsJSON );
}

QString UccDBusInterfaceAdaptor::GetPowerState()
{
  // Return the current power state string (e.g. "power_ac" or "power_bat")
  try {
    // m_service owns m_currentState
    if ( m_service )
    {
      return QString::fromStdString( profileStateToString( m_service->m_currentState ) );
    }
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[DBus] GetPowerState exception: " << e.what() << std::endl;
  }
  return QString("power_ac");
}

bool UccDBusInterfaceAdaptor::SetStateMap( const QString &state, const QString &profileId )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service )
  {
    return false;
  }

  const std::string stateStr = state.toStdString();
  const std::string profileIdStr = profileId.toStdString();
  std::cout << "[DBus] SetStateMap: " << stateStr << " -> " << profileIdStr << std::endl;

  // Verify the profile exists before updating stateMap
  if ( stateStr == "power_ac" || stateStr == "power_bat" || stateStr == "power_wc" )
  {
    // Check if profile exists in:
    // 1. m_customProfiles (parsed objects)
    // 2. m_settings.profiles (authoritative source from file)
    // 3. m_defaultProfiles (built-in profiles)
    bool profileExists = false;

    for ( const auto &profile : m_service->m_customProfiles )
    {
      if ( profile.id == profileIdStr )
      {
        profileExists = true;
        break;
      }
    }

    if ( !profileExists && m_service->m_settings.profiles.find( profileIdStr ) != m_service->m_settings.profiles.end() )
    {
      profileExists = true;
    }

    if ( !profileExists )
    {
      for ( const auto &profile : m_service->m_defaultProfiles )
      {
        if ( profile.id == profileIdStr )
        {
          profileExists = true;
          break;
        }
      }
    }

    if ( !profileExists )
    {
      std::cerr << "[DBus] SetStateMap: Profile ID '" << profileIdStr << "' does not exist, rejecting" << std::endl;
      return false;
    }

    // Profile exists, safe to update
    m_service->m_settings.stateMap[stateStr] = profileIdStr;
    const bool wrote = m_service->m_settingsManager.writeSettings( m_service->m_settings );
    if ( !wrote )
      std::cerr << "[Settings] Failed to persist stateMap update" << std::endl;
    m_service->updateDBusSettingsData();
    return wrote;
  }

  return false;
}

bool UccDBusInterfaceAdaptor::SetBatchStateMap( const QString &stateMapJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( !m_service )
    return false;

  QJsonDocument doc = QJsonDocument::fromJson( stateMapJSON.toUtf8() );
  if ( !doc.isObject() )
  {
    std::cerr << "[DBus] SetBatchStateMap: Invalid JSON" << std::endl;
    return false;
  }

  QJsonObject map = doc.object();
  static const QStringList validStates = { "power_ac", "power_bat", "power_wc" };
  bool anyChanged = false;

  for ( auto it = map.constBegin(); it != map.constEnd(); ++it )
  {
    const std::string stateStr = it.key().toStdString();
    const std::string profileIdStr = it.value().toString().toStdString();

    if ( !validStates.contains( it.key() ) )
    {
      std::cerr << "[DBus] SetBatchStateMap: Invalid state '" << stateStr << "', skipping" << std::endl;
      continue;
    }

    // Verify the profile exists (same checks as SetStateMap)
    bool profileExists = false;
    for ( const auto &profile : m_service->m_customProfiles )
    {
      if ( profile.id == profileIdStr ) { profileExists = true; break; }
    }
    if ( !profileExists && m_service->m_settings.profiles.find( profileIdStr ) != m_service->m_settings.profiles.end() )
      profileExists = true;
    if ( !profileExists )
    {
      for ( const auto &profile : m_service->m_defaultProfiles )
      {
        if ( profile.id == profileIdStr ) { profileExists = true; break; }
      }
    }
    if ( !profileExists )
    {
      std::cerr << "[DBus] SetBatchStateMap: Profile ID '" << profileIdStr << "' does not exist, skipping" << std::endl;
      continue;
    }

    std::cout << "[DBus] SetBatchStateMap: " << stateStr << " -> " << profileIdStr << std::endl;
    m_service->m_settings.stateMap[stateStr] = profileIdStr;
    anyChanged = true;
  }

  if ( !anyChanged )
    return false;

  // Write settings only once for the entire batch
  const bool wrote = m_service->m_settingsManager.writeSettings( m_service->m_settings );
  if ( !wrote )
    std::cerr << "[Settings] Failed to persist batch stateMap update" << std::endl;
  m_service->updateDBusSettingsData();

  // If the current power state was among the changed entries, apply the new profile immediately
  const std::string currentStateKey = profileStateToString( m_service->m_currentState );
  if ( map.contains( QString::fromStdString( currentStateKey ) ) )
  {
    std::cout << "[DBus] SetBatchStateMap: Current state '" << currentStateKey
              << "' was updated, applying profile" << std::endl;
    m_service->applyProfileForCurrentState();
  }

  return wrote;
}

// odm methods

QStringList UccDBusInterfaceAdaptor::ODMProfilesAvailable()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  QStringList result;
  for ( const auto &s : m_data.odmProfilesAvailable )
    result.append( QString::fromStdString( s ) );
  return result;
}

QString UccDBusInterfaceAdaptor::ODMPowerLimitsJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.odmPowerLimitsJSON );
}

// keyboard backlight methods

QString UccDBusInterfaceAdaptor::GetKeyboardBacklightCapabilitiesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.keyboardBacklightCapabilitiesJSON );
}

QString UccDBusInterfaceAdaptor::GetKeyboardBacklightStatesJSON()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.keyboardBacklightStatesJSON );
}

bool UccDBusInterfaceAdaptor::SetKeyboardBacklightStatesJSON( const QString &keyboardBacklightStatesJSON )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service->m_settings.keyboardBacklightControlEnabled ) return false;

  auto inputJSON = keyboardBacklightStatesJSON.toStdString();

  // The caller sends a JSON object:
  //   { "keyboardProfileId": "...", "states": [...] }
  // Extract optional metadata before applying to hardware.
  auto extractStr = []( const std::string &json, const std::string &key ) -> std::string {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find( search );
    if ( pos == std::string::npos ) return {};
    pos += search.length();
    size_t end = json.find( '"', pos );
    if ( end == std::string::npos ) return {};
    return json.substr( pos, end - pos );
  };
  std::string keyboardProfileId = extractStr( inputJSON, "keyboardProfileId" );

  // Apply the states array to hardware (extracts "states" from the object)
  if ( !m_service->m_keyboardBacklightController.applyProfileKeyboardStates( inputJSON ) )
    return false;

  // Update the D-Bus readable state with the states *array* so
  // GetKeyboardBacklightStatesJSON returns a clean array.
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.keyboardBacklightStatesJSON =
      m_service->m_keyboardBacklightController.currentStatesJSON();
  }

  // If the caller provided a keyboard profile ID, update the active profile
  // reference and notify all clients so they stay in sync.
  if ( !keyboardProfileId.empty() )
  {
    m_service->m_activeProfile.keyboard.keyboardProfileId = keyboardProfileId;
    m_service->updateDBusActiveProfileData();
    if ( m_service->m_adaptor )
      emitProfileChanged( m_service->m_activeProfile.id,
                          keyboardProfileId,
                          m_service->m_activeProfile.fan.fanProfile );
  }

  return true;
}



// fan control methods

int UccDBusInterfaceAdaptor::GetFansMinSpeed()
{
  return m_data.fansMinSpeed;
}

bool UccDBusInterfaceAdaptor::GetFansOffAvailable()
{
  return m_data.fansOffAvailable;
}

// charging methods (stubs for now)

QString UccDBusInterfaceAdaptor::GetChargingProfilesAvailable()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargingProfilesAvailable );
}

QString UccDBusInterfaceAdaptor::GetCurrentChargingProfile()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.currentChargingProfile );
}

bool UccDBusInterfaceAdaptor::SetChargingProfile( const QString &profileDescriptor )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  bool result = m_service->m_profileSettingsWorker->applyChargingProfile( profileDescriptor.toStdString() );

  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.currentChargingProfile = profileDescriptor.toStdString();
  }

  return result;
}

QString UccDBusInterfaceAdaptor::GetChargingPrioritiesAvailable()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargingPrioritiesAvailable );
}

QString UccDBusInterfaceAdaptor::GetCurrentChargingPriority()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.currentChargingPriority );
}

bool UccDBusInterfaceAdaptor::SetChargingPriority( const QString &priorityDescriptor )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  bool result = m_service->m_profileSettingsWorker->applyChargingPriority( priorityDescriptor.toStdString() );

  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.currentChargingPriority = priorityDescriptor.toStdString();
  }

  return result;
}

QString UccDBusInterfaceAdaptor::GetChargeStartAvailableThresholds()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargeStartAvailableThresholds );
}

QString UccDBusInterfaceAdaptor::GetChargeEndAvailableThresholds()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargeEndAvailableThresholds );
}

int UccDBusInterfaceAdaptor::GetChargeStartThreshold()
{
  return m_data.chargeStartThreshold;
}

int UccDBusInterfaceAdaptor::GetChargeEndThreshold()
{
  return m_data.chargeEndThreshold;
}

bool UccDBusInterfaceAdaptor::SetChargeStartThreshold( int value )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( value < 0 || value > 100 )
    return false;
  bool result = m_service->m_profileSettingsWorker->setChargeStartThreshold( value );

  if ( result )
    m_data.chargeStartThreshold = value;

  return result;
}

bool UccDBusInterfaceAdaptor::SetChargeEndThreshold( int value )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  if ( value < 0 || value > 100 )
    return false;
  bool result = m_service->m_profileSettingsWorker->setChargeEndThreshold( value );

  if ( result )
    m_data.chargeEndThreshold = value;

  return result;
}

QString UccDBusInterfaceAdaptor::GetChargeType()
{
  std::lock_guard< std::mutex > lock( m_data.dataMutex );
  return QString::fromStdString( m_data.chargeType );
}

bool UccDBusInterfaceAdaptor::SetChargeType( const QString &type )
{
  if ( !checkAuth( PolkitAuthority::ACTION_MANAGE_HARDWARE ) ) return false;
  bool result = m_service->m_profileSettingsWorker->setChargeType( type.toStdString() );

  if ( result )
  {
    std::lock_guard< std::mutex > lock( m_data.dataMutex );
    m_data.chargeType = type.toStdString();
  }

  return result;
}

// fn lock methods (stubs for now)

bool UccDBusInterfaceAdaptor::GetFnLockSupported()
{
  return m_service->m_fnLockController.isSupported();
}

bool UccDBusInterfaceAdaptor::GetFnLockStatus()
{
  return m_service->m_fnLockController.getStatus();
}

void UccDBusInterfaceAdaptor::SetFnLockStatus( bool status )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  m_service->m_fnLockController.setStatus( status );
}

// sensor data collection methods

void UccDBusInterfaceAdaptor::SetSensorDataCollectionStatus( bool status )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  m_data.sensorDataCollectionStatus = status;
}

bool UccDBusInterfaceAdaptor::GetSensorDataCollectionStatus()
{
  return m_data.sensorDataCollectionStatus;
}

void UccDBusInterfaceAdaptor::SetDGpuD0Metrics( bool status )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  m_data.d0MetricsUsage = status;
}

// nvidia power control methods

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLDefaultPowerLimit()
{
  return m_data.nvidiaPowerCTRLDefaultPowerLimit;
}

int UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLMaxPowerLimit()
{
  return m_data.nvidiaPowerCTRLMaxPowerLimit;
}

bool UccDBusInterfaceAdaptor::GetNVIDIAPowerCTRLAvailable()
{
  return m_data.nvidiaPowerCTRLAvailable;
}

bool UccDBusInterfaceAdaptor::SetNVIDIAPowerOffset( int offset )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_profileSettingsWorker ) return false;
  if ( !m_data.nvidiaPowerCTRLAvailable ) return false;

  return m_service->m_profileSettingsWorker->applyNVIDIAPowerOffset( offset );
}

QString UccDBusInterfaceAdaptor::GetAvailableGovernors()
{
  if ( m_service && m_service->getCpuWorker() )
  {
    auto governors = m_service->getCpuWorker()->getAvailableGovernors();
    if ( governors )
    {
      std::string json = "[";
      for ( size_t i = 0; i < governors->size(); ++i )
      {
        if ( i > 0 ) json += ",";
        json += "\"" + (*governors)[i] + "\"";
      }
      json += "]";
      return QString::fromStdString( json );
    }
  }
  return QStringLiteral("[]");
}

QString UccDBusInterfaceAdaptor::GetAvailableEPPs()
{
  if ( m_service && m_service->getCpuWorker() )
  {
    auto epps = m_service->getCpuWorker()->getAvailableEPPs();
    if ( epps )
    {
      std::string json = "[";
      for ( size_t i = 0; i < epps->size(); ++i )
      {
        if ( i > 0 ) json += ",";
        json += "\"" + (*epps)[i] + "\"";
      }
      json += "]";
      return QString::fromStdString( json );
    }
  }
  return QStringLiteral("[]");
}

int UccDBusInterfaceAdaptor::GetCpuCoreCount()
{
  if ( m_service && m_service->getCpuWorker() )
    return m_service->getCpuWorker()->getCoreCount();
  return -1;
}

// water cooler methods

bool UccDBusInterfaceAdaptor::GetWaterCoolerAvailable()
{
  return m_data.waterCoolerAvailable;
}

bool UccDBusInterfaceAdaptor::GetWaterCoolerConnected()
{
  return m_data.waterCoolerConnected;
}

int UccDBusInterfaceAdaptor::GetWaterCoolerFanSpeed()
{ return m_service ? static_cast< int >( m_service->m_waterCoolerWorker->getLastFanSpeed() ) : -1; }

int UccDBusInterfaceAdaptor::GetWaterCoolerPumpLevel()
{ return m_service ? static_cast< int >( m_service->m_waterCoolerWorker->getLastPumpVoltage() ) : -1; }

bool UccDBusInterfaceAdaptor::EnableWaterCooler( bool enable )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // Update shared DBus flag and request service to perform actions when disabling
  m_data.waterCoolerScanningEnabled = enable;

  // When scanning is disabled, mark unavailable and disconnected so clients
  // immediately see the device as gone
  if ( not enable )
  {
    m_data.waterCoolerAvailable = false;
    m_data.waterCoolerConnected = false;
  }

  // Update the active profile so that subsequent profile re-applications
  // (e.g. resume from suspend, autosave reload) don't override the runtime state.
  if ( m_service )
  {
    m_service->m_activeProfile.fan.enableWaterCooler = enable;
    m_service->setWaterCoolerScanningEnabled( enable );
  }

  return true;
}

bool UccDBusInterfaceAdaptor::IsWaterCoolerEnabled()
{
  if ( m_service )
    return m_service->m_activeProfile.fan.enableWaterCooler;
  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerFanSpeed( int dutyCyclePercent )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( dutyCyclePercent < 0 || dutyCyclePercent > 100 )
    return false;
  if ( m_service && m_service->m_waterCoolerWorker )
    return m_service->m_waterCoolerWorker->setFanSpeed( dutyCyclePercent );

  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerPumpVoltage( int voltage )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  // V12(1) is reserved and V1bis excluded. Valid: {0, 2, 3, 4}
  if ( voltage != 0 && voltage != 2 && voltage != 3 && voltage != 4 )
    return false;
  if ( m_service && m_service->m_waterCoolerWorker )
    return m_service->m_waterCoolerWorker->setPumpVoltage( voltage );

  return false;
}

bool UccDBusInterfaceAdaptor::SetWaterCoolerLEDColor( int red, int green, int blue, int mode )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    m_service->m_waterCoolerLedMode.store( mode );

    // Temperature mode: internally use Static, daemon auto-sets color from fan speed
    const int hwMode = ( mode == static_cast< int >( ucc::RGBState::Temperature ) )
                             ? static_cast< int >( ucc::RGBState::Static )
                             : mode;

    return m_service->m_waterCoolerWorker->setLEDColor( red, green, blue, hwMode );
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerLED()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    return m_service->m_waterCoolerWorker->turnOffLED();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerFan()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    return m_service->m_waterCoolerWorker->turnOffFan();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::TurnOffWaterCoolerPump()
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( m_service && m_service->m_waterCoolerWorker )
  {
    return m_service->m_waterCoolerWorker->turnOffPump();
  }
  return false;
}

bool UccDBusInterfaceAdaptor::IsWaterCoolerAutoControlEnabled()
{
  if ( m_service )
  {
    return m_service->m_activeProfile.fan.autoControlWC;
  }
  return false;
}

bool UccDBusInterfaceAdaptor::GetWaterCoolerSupported()
{
  return m_data.waterCoolerSupported;
}

bool UccDBusInterfaceAdaptor::GetCTGPAdjustmentSupported()
{
  return m_data.cTGPAdjustmentSupported;
}

// --- Monitoring history D-Bus methods ---

QByteArray UccDBusInterfaceAdaptor::GetMonitorDataSince( qlonglong sinceTimestampMs )
{
  if ( !m_service )
    return QByteArray{};
  const auto raw = m_service->m_metricsStore.querySinceBinary( sinceTimestampMs );
  return QByteArray( reinterpret_cast< const char * >( raw.data() ),
                     static_cast< qsizetype >( raw.size() ) );
}

void UccDBusInterfaceAdaptor::SetMonitorHistoryHorizon( int seconds )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return;
  if ( m_service )
    m_service->m_metricsStore.setHorizon( seconds );
}

int UccDBusInterfaceAdaptor::GetMonitorHistoryHorizon()
{
  return m_service ? m_service->m_metricsStore.horizonSeconds() : 0;
}

int UccDBusInterfaceAdaptor::GetCpuFrequencyMHz()
{
  return m_data.cpuFrequencyMHz.load();
}

// ---------------------------------------------------------------------------
// NVIDIA GPU OC methods
// ---------------------------------------------------------------------------

bool UccDBusInterfaceAdaptor::GetNvidiaOCAvailable()
{
  return m_service && m_service->m_nvidiaOCWorker && m_service->m_nvidiaOCWorker->isAvailable();
}

QString UccDBusInterfaceAdaptor::GetNvidiaOCState( int deviceIndex )
{
  if ( !m_service || !m_service->m_nvidiaOCWorker )
    return QStringLiteral( "{}" );
  return QString::fromStdString(
      m_service->m_nvidiaOCWorker->getOCStateJSON( static_cast< unsigned int >( deviceIndex ) ) );
}

bool UccDBusInterfaceAdaptor::SetNvidiaClockOffset( int deviceIndex, int clockType, int pstate, int offsetMHz )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setClockOffset(
      static_cast< unsigned int >( deviceIndex ),
      static_cast< unsigned int >( clockType ),
      static_cast< unsigned int >( pstate ),
      offsetMHz );
}

bool UccDBusInterfaceAdaptor::SetNvidiaGpuLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setGpuLockedClocks(
      static_cast< unsigned int >( deviceIndex ),
      static_cast< unsigned int >( minMHz ),
      static_cast< unsigned int >( maxMHz ) );
}

bool UccDBusInterfaceAdaptor::SetNvidiaVramLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setVramLockedClocks(
      static_cast< unsigned int >( deviceIndex ),
      static_cast< unsigned int >( minMHz ),
      static_cast< unsigned int >( maxMHz ) );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaGpuLockedClocks( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetGpuLockedClocks( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaVramLockedClocks( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetVramLockedClocks( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaAllClockOffsets( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetAllClockOffsets( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::SetNvidiaGpuPowerLimit( int deviceIndex, double watts )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->setPowerLimit( static_cast< unsigned int >( deviceIndex ), watts );
}

bool UccDBusInterfaceAdaptor::ResetNvidiaGpuPowerLimit( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetPowerLimit( static_cast< unsigned int >( deviceIndex ) );
}

bool UccDBusInterfaceAdaptor::ApplyNvidiaGpuOCProfile( const QString &profileJSON, int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;

  const std::string profileJsonStd = profileJSON.toStdString();
  const bool result = m_service->m_nvidiaOCWorker->applyGpuOCProfile(
      profileJsonStd, static_cast< unsigned int >( deviceIndex ) );

  if ( !result )
    return false;

  // Apply cTGP offset from GPU profile payload (GPU-profile path only)
  if ( m_service->m_profileSettingsWorker && m_service->m_dbusData.nvidiaPowerCTRLAvailable.load() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( profileJsonStd ) );
    if ( doc.isObject() )
    {
      QJsonObject obj = doc.object();
      if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj[ "nvidiaPowerCTRLProfile" ].isObject() )
      {
        QJsonObject nvidiaObj = obj[ "nvidiaPowerCTRLProfile" ].toObject();
        int ctgpOffset = nvidiaObj.value( "cTGPOffset" ).toInt( 0 );
        m_service->m_profileSettingsWorker->applyNVIDIAPowerOffset( ctgpOffset );
      }

      // Update active profile's embedded GPU OC data for readback
      m_service->m_activeProfile.gpuOCProfileData = profileJsonStd;
      m_service->updateDBusActiveProfileData();
    }
  }

  auto extractStr = []( const std::string &json, const std::string &key ) -> std::string {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find( search );
    if ( pos == std::string::npos ) return {};
    pos += search.length();
    size_t end = json.find( '"', pos );
    if ( end == std::string::npos ) return {};
    return json.substr( pos, end - pos );
  };

  const std::string gpuProfileId = extractStr( profileJsonStd, "gpuProfileId" );
  if ( !gpuProfileId.empty() )
  {
    m_service->m_activeProfile.gpuProfileId = gpuProfileId;
    m_service->updateDBusActiveProfileData();

    if ( m_service->m_adaptor )
      emitProfileChanged( m_service->m_activeProfile.id,
                          m_service->m_activeProfile.keyboard.keyboardProfileId,
                          m_service->m_activeProfile.fan.fanProfile,
                          gpuProfileId );
  }

  return true;
}

bool UccDBusInterfaceAdaptor::ResetNvidiaGpuOCAll( int deviceIndex )
{
  if ( !checkAuth( PolkitAuthority::ACTION_CONTROL ) ) return false;
  if ( !m_service || !m_service->m_nvidiaOCWorker ) return false;
  return m_service->m_nvidiaOCWorker->resetAll( static_cast< unsigned int >( deviceIndex ) );
}

// signal emitters
// These may be called from the DaemonWorker thread, but the adaptor lives in
// the main thread.  Use QMetaObject::invokeMethod with a queued connection so
// the actual emit happens in the object's owning thread.

void UccDBusInterfaceAdaptor::emitModeReapplyPendingChanged( bool pending )
{
  QMetaObject::invokeMethod( this, [this, pending]() {
    emit ModeReapplyPendingChanged( pending );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitProfileChanged( const std::string &profileId,
                                                  const std::string &keyboardProfileId,
                                                  const std::string &fanProfileId,
                                                  const std::string &gpuProfileId )
{
  QString id = QString::fromStdString( profileId );
  QString kbId = QString::fromStdString( keyboardProfileId );
  QString fpId = QString::fromStdString( fanProfileId );

  std::string effectiveGpu = gpuProfileId;
  if ( effectiveGpu.empty() && m_service )
    effectiveGpu = m_service->m_activeProfile.gpuProfileId;
  QString gpId = QString::fromStdString( effectiveGpu );

  QMetaObject::invokeMethod( this, [this, id, kbId, fpId, gpId]() {
    emit ProfileChanged( id, kbId, fpId, gpId );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitPowerStateChanged( const std::string &state )
{
  QString s = QString::fromStdString( state );
  QMetaObject::invokeMethod( this, [this, s]() {
    emit PowerStateChanged( s );
  }, Qt::QueuedConnection );
}

void UccDBusInterfaceAdaptor::emitWaterCoolerStatusChanged( const std::string &status )
{
  QString s = QString::fromStdString( status );
  QMetaObject::invokeMethod( this, [this, s]() {
    emit WaterCoolerStatusChanged( s );
  }, Qt::QueuedConnection );
}

// UccDBusService implementation

UccDBusService::UccDBusService()
  : DaemonWorker( std::chrono::milliseconds( 1000 ), false ),
    m_dbusData(),
    m_io(),
    m_dbusObject( nullptr ),
    m_adaptor( nullptr ),
    m_started( false ),
    m_profileManager(),
    m_settingsManager(),
    m_settings(),
    m_activeProfile(),
    m_defaultProfiles(),
    m_customProfiles(),
    m_currentState( ProfileState::AC ),
    m_currentStateProfileId(),
    m_previousWaterCoolerConnected( false ),
    m_waterCoolerWorker( std::make_unique<LCTWaterCoolerWorker>( m_dbusData ) )
{
  // set daemon version
  m_dbusData.uccdVersion = "2.1.21";

  // identify and set device
  auto device = identifyDevice();
  m_deviceId = device;
  if ( device.has_value() )
  {
    m_dbusData.device = std::to_string( static_cast< int >( device.value() ) );
  }
  else
  {
    m_dbusData.device = "";
  }

  // compute device-specific feature flags (aquaris, cTGP)
  computeDeviceCapabilities();

  // detect system hardware info (CPU, GPU, laptop model)
  m_systemInfo = detectSystemInfo( m_deviceId );
  m_dbusData.systemInfoJSON = m_systemInfo.toJSON();

  // Allow the daemon to start on any Linux system. Hardware-specific features
  // still report availability from the driver/sysfs capability checks below.
  m_dbusData.deviceSupported = ucc::isDeviceSupported();

  // detect display session type and initialize display modes
  initializeDisplayModes();

  // check tuxedo wmi availability
  m_dbusData.tuxedoWmiAvailable = m_io.wmiAvailable();

  // set default system JSON values (sentinels for GPU/CPU monitoring data)
  m_dbusData.primeState = "-1";
  m_dbusData.dGpuInfoValuesJSON = "{\"temp\":-1,\"powerDraw\":-1,\"maxPowerLimit\":-1,\"enforcedPowerLimit\":-1,\"coreFrequency\":-1,\"vramFrequency\":-1,\"maxCoreFrequency\":-1,\"computeUtilPct\":-1,\"memoryUtilPct\":-1,\"vramUsedMiB\":-1,\"vramTotalMiB\":-1,\"perfLimitReason\":\"\",\"encoderUtilPct\":-1,\"decoderUtilPct\":-1,\"currentPstate\":-1,\"grClockOffsetMHz\":-999,\"memClockOffsetMHz\":-999,\"coreVoltageMv\":-1}";
  m_dbusData.iGpuInfoValuesJSON = "{\"vendor\":\"unknown\",\"temp\":-1,\"coreFrequency\":-1,\"maxCoreFrequency\":-1,\"powerDraw\":-1}";

  // Keyboard backlight will be detected during worker initialization
  m_dbusData.keyboardBacklightCapabilitiesJSON = "null";
  m_dbusData.keyboardBacklightStatesJSON = "[]";

  // Read all hardware capabilities directly using m_io / sysfs BEFORE any
  // workers or profiles are created.  This populates TDP limits, NVIDIA
  // power limits, charging profiles, and YCbCr420 availability with real
  // hardware values so the D-Bus data is never populated with fake defaults.
  //
  // Create the single shared NvmlWrapper instance first — it is reused by
  // readHardwareCapabilities(), HardwareMonitorWorker, NvidiaOCWorker, and
  // ProfileSettingsWorker so that NVML is initialised exactly once.
  m_nvml = std::make_shared< NvmlWrapper >();
  readHardwareCapabilities();

  // initialize profiles first (safer, doesn't start threads)
  initializeProfiles();

  // Load settings (creates defaults if needed)
  loadSettings();

  // Now build settings JSON with actual stateMap
  m_dbusData.settingsJSON = buildSettingsJSON( m_dbusData.keyboardBacklightStatesJSON,
                                               m_dbusData.currentChargingProfile,
                                               m_settings );

  // Load autosave
  loadAutosave();

  // Initialize display worker (merged backlight + refresh rate)
  m_displayWorker = std::make_unique< DisplayWorker >(
    m_autosaveManager.getAutosavePath(),
    [this]() { return m_activeProfile; },
    [this]() { return m_autosave.displayBrightness; },
    [this]( int32_t brightness ) { m_autosave.displayBrightness = brightness; },
    [this]() -> bool { return m_dbusData.isX11; },
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.displayModes = json;
    },
    [this]( bool isX11 ) { m_dbusData.isX11 = isX11; }
  );

  // initialize cpu worker
  m_cpuWorker = std::make_unique< CpuWorker >(
    [this]() { return m_activeProfile; },
    [this]() { return m_settings.cpuSettingsEnabled; },
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );

  // initialize profile settings worker (replaces ODMPowerLimitWorker, ODMProfileWorker, ChargingWorker, YCbCr420WorkaroundWorker)
  // Quirk: some devices (e.g. IBP Gen10 AMD) have a non-functional ACPI platform_profile —
  // skip it and fall through to the Tuxedo IO API instead (synced from TCC f71acd86, a1b2f6b4).
  const bool skipAcpiPlatformProfile =
    m_deviceId.has_value() and
    ( m_deviceId.value() == UniwillDeviceID::IBPG10AMD or
      m_deviceId.value() == UniwillDeviceID::IBM15A10 );

  m_profileSettingsWorker = std::make_unique< ProfileSettingsWorker >(
    m_io,
    m_nvml,
    [this]() -> UccProfile { return m_activeProfile; },
    [this]( const std::vector< std::string > &profiles ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.odmProfilesAvailable = profiles;
    },
    [this]( const std::string &json ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.odmPowerLimitsJSON = json;
    },
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); },
    m_settings,
    m_dbusData.modeReapplyPending,
    m_dbusData.nvidiaPowerCTRLDefaultPowerLimit,
    m_dbusData.nvidiaPowerCTRLMaxPowerLimit,
    m_dbusData.nvidiaPowerCTRLAvailable,
    m_dbusData.cTGPAdjustmentSupported,
    skipAcpiPlatformProfile
  );

  // initialize hardware monitor worker (merged GPU info + CPU power + Prime)
  // Quirk: IBM15A10 has a display mux — prime-select is only supported when
  // the eDP display is NOT wired to the NVIDIA GPU (synced from TCC PrimeWorker).
  const bool isDisplayMuxDevice =
    m_deviceId.has_value() and m_deviceId.value() == UniwillDeviceID::IBM15A10;

  m_hardwareMonitorWorker = std::make_unique< HardwareMonitorWorker >(
    m_nvml,
    [this]( const std::string &json, double cpuPowerWatts ) {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
        m_dbusData.cpuPowerValuesJSON = json;
      }
      // Push CPU power to history store
      if ( cpuPowerWatts > -1.0 )
        m_metricsStore.push( MetricId::CpuPower, cpuPowerWatts );
    },
    [this]() { return m_dbusData.sensorDataCollectionStatus.load(); },
    [this]( const std::string &primeState ) {
      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
      m_dbusData.primeState = primeState;
    },
    isDisplayMuxDevice
  );

  // webcam monitoring via HardwareMonitorWorker (replaces former WebcamWorker)
  m_hardwareMonitorWorker->setWebcamCallbacks(
    [this]() -> std::pair< bool, bool > {
      bool status = false;
      bool available = m_io.getWebcam( status );
      return { available, status };
    },
    [this]( bool available, bool status ) {
      m_dbusData.webcamSwitchAvailable = available;
      m_dbusData.webcamSwitchStatus = status;
    }
  );

  // CPU frequency monitoring via HardwareMonitorWorker (every cycle ≈ 800ms)
  m_hardwareMonitorWorker->setCpuFrequencyCallback(
    [this]( int frequencyMHz ) {
      m_dbusData.cpuFrequencyMHz = frequencyMHz;
      if ( frequencyMHz > 0 )
        m_metricsStore.push( MetricId::CpuFrequency, static_cast< double >( frequencyMHz ) );
    }
  );

  // initialize fan control worker
  m_fanControlWorker = std::make_unique< FanControlWorker >(
    m_io,
    [this]() { return m_activeProfile; },
    [this]() { return m_settings.fanControlEnabled; },
    [this]( size_t fanIndex, int64_t timestamp, int speed )
    {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );

        if ( fanIndex < m_dbusData.fans.size() )
          m_dbusData.fans[fanIndex].speed.set( timestamp, speed );
      }

      // Push fan duty to history store
      if ( fanIndex == 0 )
        m_metricsStore.push( MetricId::CpuFanDuty, timestamp, speed );
      else if ( fanIndex == 1 )
        m_metricsStore.push( MetricId::GpuFanDuty, timestamp, speed );
    },
    [this]( size_t fanIndex, int64_t timestamp, int temp )
    {
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
        if ( fanIndex < m_dbusData.fans.size() )
          m_dbusData.fans[ fanIndex ].temp.set( timestamp, temp );
      }

      // Push temperature to history store
      if ( fanIndex == 0 )
        m_metricsStore.push( MetricId::CpuTemp, timestamp, temp );
      else if ( fanIndex == 1 )
        m_metricsStore.push( MetricId::GpuTemp, timestamp, temp );

      // Auto-control water cooler fan and pump voltage based on CPU temperature
      if ( m_dbusData.waterCoolerConnected.load() && m_activeProfile.fan.autoControlWC && fanIndex == 0 )
      {
        try
        {
          // Apply asymmetric EWMA to the raw sensor reading so that the
          // water-cooler fan and pump see a smooth temperature signal,
          // matching the filtering the main fan control loop uses.
          if ( m_wcTempFiltered < 0.0 )
            m_wcTempFiltered = static_cast< double >( temp );
          else
          {
            const double alpha = ( temp > m_wcTempFiltered )
                                   ? WC_TEMP_ALPHA_RISING : WC_TEMP_ALPHA_FALLING;
            m_wcTempFiltered += alpha * ( static_cast< double >( temp ) - m_wcTempFiltered );
          }
          const int wcTemp = static_cast< int >( std::round( m_wcTempFiltered ) );

          const std::string &fpName = m_activeProfile.fan.fanProfile;
          FanProfile fp = getDefaultFanProfile( fpName );

          // Overlay water cooler fan table from profile or temporary curves
          if ( m_fanControlWorker && m_fanControlWorker->hasTemporaryCurves() )
          {
            const auto &wcTable = m_fanControlWorker->tempWaterCoolerFanTable();
            if ( !wcTable.empty() )
            {
              fp.tableWaterCoolerFan = wcTable;
            }
          }
          else if ( !m_activeProfile.fan.tableWaterCoolerFan.empty() )
          {
            fp.tableWaterCoolerFan = m_activeProfile.fan.tableWaterCoolerFan;
          }

          // Overlay pump table from temporary curves or from the active profile
          if ( m_fanControlWorker && m_fanControlWorker->hasTemporaryCurves() )
          {
            const auto &pTable = m_fanControlWorker->tempPumpTable();
            if ( !pTable.empty() )
            {
              fp.tablePump = pTable;
            }
          }
          else if ( !m_activeProfile.fan.tablePump.empty() )
          {
            fp.tablePump = m_activeProfile.fan.tablePump;
          }

          const int snappedTemp = ( ( wcTemp + 2 ) / 5 ) * 5;  // round to nearest 5°C
          const int wcFanSpeed = fp.getWaterCoolerFanSpeedForTemp( snappedTemp );
          m_waterCoolerWorker->setFanSpeed( wcFanSpeed );

          // Temperature LED mode: compute gradient color from fan speed
          if ( m_waterCoolerLedMode.load() == static_cast< int32_t >( ucc::RGBState::Temperature ) )
          {
            const float t = static_cast< float >( std::clamp( wcFanSpeed, 0, 100 ) ) / 100.0f;
            const int ledR = static_cast< int >( t * 255.0f );
            const int ledG = 0;
            const int ledB = static_cast< int >( ( 1.0f - t ) * 255.0f );
            m_waterCoolerWorker->setLEDColor( ledR, ledG, ledB,
              static_cast< int >( ucc::RGBState::Static ) );
          }

          // Auto-control pump voltage with hysteresis.
          // Step-up happens immediately at the table threshold; step-down requires
          // the temperature to fall at least PUMP_HYSTERESIS_DEG below the
          // threshold that last triggered an upward transition.
          static constexpr ucc::PumpVoltage pumpIdxToVoltage[] = {
              ucc::PumpVoltage::Off, ucc::PumpVoltage::V7, ucc::PumpVoltage::V8,
              ucc::PumpVoltage::V11, ucc::PumpVoltage::V12 };

          int rawIdx = 0;
          for ( const auto &[t, s] : fp.tablePump )
          {
            if ( wcTemp >= t ) rawIdx = std::min( s, 4 );
            else               break;
          }

          if ( rawIdx > m_pumpHysSpeedIdx )
          {
            // Temperature rising – apply new level and record its table threshold.
            m_pumpHysSpeedIdx = rawIdx;
            m_pumpHysThreshold = 0;
            for ( const auto &[t, s] : fp.tablePump )
              if ( std::min( s, 4 ) == rawIdx ) { m_pumpHysThreshold = t; break; }
          }
          else if ( rawIdx < m_pumpHysSpeedIdx )
          {
            // Temperature falling – only step down once we are past the dead-band.
            if ( wcTemp < m_pumpHysThreshold - PUMP_HYSTERESIS_DEG )
            {
              m_pumpHysSpeedIdx = rawIdx;
              m_pumpHysThreshold = 0;
              for ( const auto &[t, s] : fp.tablePump )
                if ( std::min( s, 4 ) == rawIdx ) { m_pumpHysThreshold = t; break; }
            }
          }

          const ucc::PumpVoltage pumpSpeedValue =
              pumpIdxToVoltage[ std::clamp( m_pumpHysSpeedIdx, 0, 4 ) ];
          m_waterCoolerWorker->setPumpVoltage( static_cast<int>( pumpSpeedValue ) );

          // std::cout << "[Auto WC] Temp: " << temp << "°C, Fan: " << wcFanSpeed
          //           << "%, Pump Voltage: " << static_cast<int>(pumpSpeedValue) << std::endl;
        }
        catch ( ... ) { /* ignore errors in water cooler auto-control */ }
      }
    }
  );

  // Initialize keyboard backlight controller (synchronous — no worker thread)
  {
    std::string capsJSON = m_keyboardBacklightController.init();
    m_dbusData.keyboardBacklightCapabilitiesJSON = capsJSON;

    if ( m_keyboardBacklightController.isAvailable() )
    {
      std::string defaultStates = m_keyboardBacklightController.buildDefaultStatesJSON();
      m_dbusData.keyboardBacklightStatesJSON = defaultStates;

      if ( m_settings.keyboardBacklightControlEnabled )
        m_keyboardBacklightController.applyStatesFromJSON( defaultStates );
    }
  }

  // then setup gpu callback before worker starts processing
  setupGpuDataCallback();

  // fill device-specific defaults BEFORE starting workers
  fillDeviceSpecificDefaults( m_defaultProfiles );
  fillDeviceSpecificDefaults( m_customProfiles );
  serializeProfilesJSON();

  // start worker threads after all callbacks and data are ready
  m_profileSettingsWorker->start();  // synchronous: detects ODM profile type + inits charging state
  m_hardwareMonitorWorker->start();
  m_displayWorker->start();
  m_cpuWorker->start();
  m_fanControlWorker->start();

  // Initialize NVIDIA OC worker (non-threaded, on-demand calls via D-Bus)
  m_nvidiaOCWorker = std::make_unique< NvidiaOCWorker >(
    m_nvml,
    []( const std::string &msg ) { syslog( LOG_INFO, "%s", msg.c_str() ); }
  );

  rebuildBuiltinGpuProfiles();
}

int UccDBusService::readCurrentCTGPOffset() const
{
  static const std::string NVIDIA_CTGP_OFFSET =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";

  if ( !m_dbusData.cTGPAdjustmentSupported )
    return 0;

  try
  {
    if ( auto value = SysfsNode< int >( NVIDIA_CTGP_OFFSET ).read(); value.has_value() )
      return value.value();
    return 0;
  }
  catch ( ... )
  {
    return 0;
  }
}

void UccDBusService::rebuildBuiltinGpuProfiles()
{
  m_builtinGpuProfiles.clear();

  if ( !m_nvidiaOCWorker || !m_nvidiaOCWorker->isAvailable() )
    return;

  const std::string ocStateJson = m_nvidiaOCWorker->getOCStateJSON( 0 );
  QJsonDocument stateDoc = QJsonDocument::fromJson( QByteArray::fromStdString( ocStateJson ) );
  if ( !stateDoc.isObject() )
    return;

  const QJsonObject state = stateDoc.object();
  QJsonObject profile;

  QJsonArray offsets;
  const QJsonArray pstates = state.value( "pstates" ).toArray();
  for ( const QJsonValue &entry : pstates )
  {
    const QJsonObject pstate = entry.toObject();
    QJsonObject offset;
    offset[ "pstate" ] = pstate.value( "pstate" ).toInt();
    offset[ "gpuOffsetMHz" ] = pstate.value( "gpu" ).toObject().value( "currentOffset" ).toInt( 0 );
    offset[ "vramOffsetMHz" ] = pstate.value( "vram" ).toObject().value( "currentOffset" ).toInt( 0 );
    offsets.append( offset );
  }
  profile[ "offsets" ] = offsets;

  QJsonObject gpuLocked;
  const QJsonObject gpuLockedState = state.value( "gpuLockedClocks" ).toObject();
  const QJsonObject gpuRange = state.value( "gpuClockRange" ).toObject();
  const bool gpuLockedEnabled = !gpuLockedState.isEmpty();
  gpuLocked[ "enabled" ] = gpuLockedEnabled;
  gpuLocked[ "min" ] = gpuLockedEnabled ? gpuLockedState.value( "min" ).toInt() : gpuRange.value( "min" ).toInt( 0 );
  gpuLocked[ "max" ] = gpuLockedEnabled ? gpuLockedState.value( "max" ).toInt() : gpuRange.value( "max" ).toInt( 0 );
  profile[ "gpuLockedClocks" ] = gpuLocked;

  QJsonObject vramLocked;
  const QJsonObject vramLockedState = state.value( "vramLockedClocks" ).toObject();
  const QJsonObject vramRange = state.value( "vramClockRange" ).toObject();
  const bool vramLockedEnabled = !vramLockedState.isEmpty();
  vramLocked[ "enabled" ] = vramLockedEnabled;
  vramLocked[ "min" ] = vramLockedEnabled ? vramLockedState.value( "min" ).toInt() : vramRange.value( "min" ).toInt( 0 );
  vramLocked[ "max" ] = vramLockedEnabled ? vramLockedState.value( "max" ).toInt() : vramRange.value( "max" ).toInt( 0 );
  profile[ "vramLockedClocks" ] = vramLocked;

  profile[ "powerLimitW" ] = state.value( "powerLimitW" ).toDouble( 0.0 );

  QJsonObject nvidiaPowerCtrl;
  nvidiaPowerCtrl[ "cTGPOffset" ] = readCurrentCTGPOffset();
  profile[ "nvidiaPowerCTRLProfile" ] = nvidiaPowerCtrl;

  BuiltinGpuProfile builtin;
  builtin.id = BUILTIN_GPU_PROFILE_ID;
  builtin.name = BUILTIN_GPU_PROFILE_NAME;
  builtin.json = QJsonDocument( profile ).toJson( QJsonDocument::Compact ).toStdString();
  m_builtinGpuProfiles.push_back( builtin );
}

void UccDBusService::readHardwareCapabilities()
{
  syslog( LOG_INFO, "[uccd] Reading hardware capabilities directly" );

  // ---- ODM Power Limits (TDP) ----
  // Read from TuxedoIOAPI — identical logic to ProfileSettingsWorker::getTDPInfo()
  {
    int nrTDPs = 0;
    if ( m_io.getNumberTDPs( nrTDPs ) and nrTDPs > 0 )
    {
      std::vector< std::string > descriptors;
      m_io.getTDPDescriptors( descriptors );

      std::ostringstream jsonStream;
      jsonStream << "[";

      for ( int i = 0; i < nrTDPs; ++i )
      {
        uint32_t current = 0, min = 0, max = 0;
        m_io.getTDPMin( i, reinterpret_cast< int & >( min ) );
        m_io.getTDPMax( i, reinterpret_cast< int & >( max ) );
        m_io.getTDP( i, reinterpret_cast< int & >( current ) );

        if ( i > 0 )
          jsonStream << ",";

        jsonStream << "{"
                   << "\"current\":" << current << ","
                   << "\"min\":" << min << ","
                   << "\"max\":" << max
                   << "}";

        syslog( LOG_INFO, "[uccd] TDP[%d]: min=%u, max=%u, current=%u", i, min, max, current );
      }

      jsonStream << "]";
      m_dbusData.odmPowerLimitsJSON = jsonStream.str();
    }
    else
    {
      syslog( LOG_INFO, "[uccd] No TDP hardware available" );
      m_dbusData.odmPowerLimitsJSON = "[]";
    }
  }

  // ---- NVIDIA Power Control ----
  {
    static const std::string NVIDIA_CTGP_OFFSET =
      "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";

    std::error_code ec;
    const bool nvAvailable = std::filesystem::exists( NVIDIA_CTGP_OFFSET, ec )
                          && std::filesystem::is_regular_file( NVIDIA_CTGP_OFFSET, ec );
    m_dbusData.nvidiaPowerCTRLAvailable = nvAvailable;

    if ( nvAvailable )
    {
      if ( !m_nvidiaPowerLimitsInitialized )
      {
        // Query power limits via the shared NVML instance only once per daemon startup.
        // readHardwareCapabilities() is also called after profile apply/reapply.
        if ( m_nvml && m_nvml->isAvailable() && m_nvml->deviceCount() > 0 )
        {
          if ( auto v = m_nvml->getPowerDefaultLimitW( 0 ) )
            m_dbusData.nvidiaPowerCTRLDefaultPowerLimit = static_cast< int32_t >( *v );
          if ( auto v = m_nvml->getPowerMaxLimitW( 0 ) )
            m_dbusData.nvidiaPowerCTRLMaxPowerLimit = static_cast< int32_t >( *v );
        }

        m_nvidiaPowerLimitsInitialized = true;
      }

      syslog( LOG_INFO, "[uccd] NVIDIA power limits — Default: %dW, Max: %dW",
              m_dbusData.nvidiaPowerCTRLDefaultPowerLimit.load(),
              m_dbusData.nvidiaPowerCTRLMaxPowerLimit.load() );
    }
    else
    {
      m_nvidiaPowerLimitsInitialized = false;
      syslog( LOG_INFO, "[uccd] NVIDIA power control not available" );
    }
  }

  // ---- Charging ----
  {
    static const std::string CHARGING_PROFILE_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profile";
    static const std::string CHARGING_PROFILES_AVAILABLE_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profiles_available";
    static const std::string CHARGING_PRIORITY_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prio";
    static const std::string CHARGING_PRIORITIES_AVAILABLE_PATH =
      "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prios_available";

    // Charging profiles
    if ( SysfsNode< std::string >( CHARGING_PROFILE_PATH ).isAvailable() and
         SysfsNode< std::string >( CHARGING_PROFILES_AVAILABLE_PATH ).isAvailable() )
    {
      auto profiles = SysfsNode< std::vector< std::string > >( CHARGING_PROFILES_AVAILABLE_PATH, " " ).read();
      if ( profiles.has_value() and not profiles->empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < profiles->size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << "\"" << ( *profiles )[ i ] << "\"";
        }
        oss << "]";
        m_dbusData.chargingProfilesAvailable = oss.str();

        auto current = SysfsNode< std::string >( CHARGING_PROFILE_PATH ).read();
        if ( current.has_value() )
          m_dbusData.currentChargingProfile = *current;

        syslog( LOG_INFO, "[uccd] Charging profiles: %s, current: %s",
                m_dbusData.chargingProfilesAvailable.c_str(),
                m_dbusData.currentChargingProfile.c_str() );
      }
    }

    // Charging priorities
    if ( SysfsNode< std::string >( CHARGING_PRIORITY_PATH ).isAvailable() and
         SysfsNode< std::string >( CHARGING_PRIORITIES_AVAILABLE_PATH ).isAvailable() )
    {
      auto prios = SysfsNode< std::vector< std::string > >( CHARGING_PRIORITIES_AVAILABLE_PATH, " " ).read();
      if ( prios.has_value() and not prios->empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < prios->size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << "\"" << ( *prios )[ i ] << "\"";
        }
        oss << "]";
        m_dbusData.chargingPrioritiesAvailable = oss.str();

        auto current = SysfsNode< std::string >( CHARGING_PRIORITY_PATH ).read();
        if ( current.has_value() )
          m_dbusData.currentChargingPriority = *current;
      }
    }

    // Charge thresholds
    auto battery = PowerSupplyController::getFirstBattery();
    if ( battery )
    {
      m_dbusData.chargeStartThreshold = battery->getChargeControlStartThreshold();
      m_dbusData.chargeEndThreshold = battery->getChargeControlEndThreshold();

      // Map charge type
      auto chargeType = battery->getChargeType();
      switch ( chargeType )
      {
        case ChargeType::Trickle:      m_dbusData.chargeType = "Trickle"; break;
        case ChargeType::Fast:         m_dbusData.chargeType = "Fast"; break;
        case ChargeType::Standard:     m_dbusData.chargeType = "Standard"; break;
        case ChargeType::Adaptive:     m_dbusData.chargeType = "Adaptive"; break;
        case ChargeType::Custom:       m_dbusData.chargeType = "Custom"; break;
        case ChargeType::LongLife:     m_dbusData.chargeType = "LongLife"; break;
        case ChargeType::Bypass:       m_dbusData.chargeType = "Bypass"; break;
        case ChargeType::NotAvailable: m_dbusData.chargeType = "N/A"; break;
        default:                       m_dbusData.chargeType = "Unknown"; break;
      }

      // Threshold ranges
      auto startThresholds = battery->getChargeControlStartAvailableThresholds();
      auto endThresholds = battery->getChargeControlEndAvailableThresholds();

      if ( !startThresholds.empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < startThresholds.size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << startThresholds[ i ];
        }
        oss << "]";
        m_dbusData.chargeStartAvailableThresholds = oss.str();
      }

      if ( !endThresholds.empty() )
      {
        std::ostringstream oss;
        oss << "[";
        for ( size_t i = 0; i < endThresholds.size(); ++i )
        {
          if ( i > 0 ) oss << ",";
          oss << endThresholds[ i ];
        }
        oss << "]";
        m_dbusData.chargeEndAvailableThresholds = oss.str();
      }
    }
  }

  // ---- YCbCr 4:2:0 ----
  {
    m_dbusData.forceYUV420OutputSwitchAvailable = false;

    for ( const auto &cardEntry : m_settings.ycbcr420Workaround )
    {
      int card = cardEntry.card;
      for ( const auto &portEntry : cardEntry.ports )
      {
        std::string path = "/sys/kernel/debug/dri/" + std::to_string( card ) + "/" +
                           portEntry.port + "/force_yuv420_output";

        std::error_code ec;
        if ( std::filesystem::exists( path, ec ) && std::filesystem::is_regular_file( path, ec ) )
        {
          m_dbusData.forceYUV420OutputSwitchAvailable = true;
          syslog( LOG_INFO, "[uccd] YCbCr 4:2:0 output available at %s", path.c_str() );
          break;
        }
      }
      if ( m_dbusData.forceYUV420OutputSwitchAvailable.load() )
        break;
    }
  }

  // Rebuild settings JSON with real charging data
  m_dbusData.settingsJSON = buildSettingsJSON( m_dbusData.keyboardBacklightStatesJSON,
                                               m_dbusData.currentChargingProfile,
                                               m_settings );
}

void UccDBusService::setupGpuDataCallback()
{
  // Set up callback to update DBus data when GPU info is collected
  m_hardwareMonitorWorker->setGpuDataCallback(
    [this]( const IGpuInfo &iGpuInfo, const DGpuInfo &dGpuInfo )
    {
      // safety check - ensure we're not being called during destruction
      if ( not m_started )
        return;

      std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );

      // Convert GPU structures to JSON and update DBus data
      m_dbusData.iGpuInfoValuesJSON = igpuInfoToJSON( iGpuInfo );
      m_dbusData.dGpuInfoValuesJSON = dgpuInfoToJSON( dGpuInfo );

      // Push GPU metrics to history store (outside dataMutex — store has its own lock)
      const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
        std::chrono::system_clock::now().time_since_epoch() ).count();

      if ( dGpuInfo.m_temp > -1.0 )
        m_metricsStore.push( MetricId::GpuTemp, now, dGpuInfo.m_temp );
      if ( dGpuInfo.m_coreFrequency > -1.0 )
        m_metricsStore.push( MetricId::GpuFrequency, now, dGpuInfo.m_coreFrequency );
      if ( dGpuInfo.m_powerDraw > -1.0 )
        m_metricsStore.push( MetricId::GpuPower, now, dGpuInfo.m_powerDraw );
      if ( dGpuInfo.m_vramFrequency > -1.0 )
        m_metricsStore.push( MetricId::GpuVramFrequency, now, dGpuInfo.m_vramFrequency );
      if ( dGpuInfo.m_coreVoltageMv > -1 )
        m_metricsStore.push( MetricId::GpuCoreVoltage, now,
                             static_cast< double >( dGpuInfo.m_coreVoltageMv ) );

      // Expose dGPU temperature through fan data for UI compatibility
      if ( dGpuInfo.m_temp > -1.0 and m_dbusData.fans.size() > 1 )
      {
        m_dbusData.fans[ 1 ].temp.set(
          static_cast< int64_t >( now ),
          static_cast< int32_t >( std::lround( dGpuInfo.m_temp ) ) );
      }
    }
  );
}

void UccDBusService::updateFanData()
{
  int numberFans = 0;
  bool fansAvailable = m_io.getNumberFans( numberFans ) && numberFans > 0;

  // If getNumberFans fails, try to detect fans by reading temperature from fan 0
  if ( !fansAvailable )
  {
    int temp = -1;
    if ( m_io.getFanTemperature( 0, temp ) && temp >= 0 )
    {
      // We can read from at least fan 0, assume fans are available
      fansAvailable = true;
      numberFans = 2; // Assume CPU and GPU fans
      syslog( LOG_INFO, "UccDBusService: Detected fans by temperature reading (getNumberFans failed)" );
    }
  }

  int minSpeed = 0;
  bool fansOffAvailable = false;
  ( void ) m_io.getFansMinSpeed( minSpeed );
  ( void ) m_io.getFansOffAvailable( fansOffAvailable );

  const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
    std::chrono::system_clock::now().time_since_epoch() ).count();

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.fanHwmonAvailable = fansAvailable;
  m_dbusData.fansMinSpeed = minSpeed;
  m_dbusData.fansOffAvailable = fansOffAvailable;

  if ( not fansAvailable )
    return;

  const int maxFans = std::min( numberFans, static_cast< int >( m_dbusData.fans.size() ) );
  for ( int fanIndex = 0; fanIndex < maxFans; ++fanIndex )
  {
    int speedPercent = -1;
    int tempCelsius = -1;

    if ( m_io.getFanSpeedPercent( fanIndex, speedPercent ) )
    {
      m_dbusData.fans[ static_cast< size_t >( fanIndex ) ].speed.set( static_cast< int64_t >( now ), speedPercent );
    }

    if ( m_io.getFanTemperature( fanIndex, tempCelsius ) )
    {
      m_dbusData.fans[ static_cast< size_t >( fanIndex ) ].temp.set( static_cast< int64_t >( now ), tempCelsius );
    }
  }
}

bool UccDBusService::initDBus()
{
  // Must be called from the main thread (before start()) so that
  // m_dbusObject lives in the main thread's event loop and
  // QDBusConnection::registerObject() can create child QObjects there.
  try
  {
    QDBusConnection bus = QDBusConnection::systemBus();
    if ( !bus.isConnected() )
    {
      syslog( LOG_ERR, "Failed to connect to system D-Bus" );
      return false;
    }

    // Create the D-Bus object in the main thread (with QDBusContext for Polkit)
    m_dbusObject = std::make_unique< UccDBusObject >();

    // Create the adaptor (attaches to m_dbusObject automatically)
    m_adaptor = std::make_unique< UccDBusInterfaceAdaptor >( m_dbusObject.get(), m_dbusData, this );

    // Register the object on the bus
    if ( !bus.registerObject( OBJECT_PATH, m_dbusObject.get() ) )
    {
      syslog( LOG_ERR, "Failed to register D-Bus object at %s: %s",
              OBJECT_PATH, qPrintable( bus.lastError().message() ) );
      m_adaptor.reset();
      m_dbusObject.reset();
      return false;
    }

    // Request the service name
    if ( !bus.registerService( SERVICE_NAME ) )
    {
      syslog( LOG_ERR, "Failed to register D-Bus service name %s: %s",
              SERVICE_NAME, qPrintable( bus.lastError().message() ) );
      bus.unregisterObject( OBJECT_PATH );
      m_adaptor.reset();
      m_dbusObject.reset();
      return false;
    }

    syslog( LOG_INFO, "DBus service registered on %s (Qt D-Bus)", SERVICE_NAME );
    return true;
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_ERR, "DBus service error: %s", e.what() );
    return false;
  }
}

void UccDBusService::shutdown()
{
  syslog( LOG_INFO, "Shutting down workers..." );

  // Phase 1: Signal ALL workers to stop (non-blocking).
  // This must happen before waiting, because some onWork() callbacks
  // use BlockingQueuedConnection to the main thread.  If we stop()+wait
  // sequentially, a worker stuck in BlockingQueuedConnection will deadlock
  // because the main thread is blocked in QThread::wait().
  requestStop();
  if ( m_fanControlWorker ) m_fanControlWorker->requestStop();
  if ( m_cpuWorker ) m_cpuWorker->requestStop();
  if ( m_displayWorker ) m_displayWorker->requestStop();
  if ( m_hardwareMonitorWorker ) m_hardwareMonitorWorker->requestStop();

  // Phase 2: Wait for all threads to finish while keeping the Qt event
  // loop alive.  Workers may have pending BlockingQueuedConnection calls
  // (e.g. LCTWaterCoolerWorker) that need the main thread to dispatch.
  // Pumping events here prevents deadlocks.
  auto waitPumpingEvents = []( QThread *t ) {
    if ( !t || !t->isRunning() )
      return;
    auto *app = QCoreApplication::instance();
    while ( !t->isFinished() )
    {
      if ( app )
        app->processEvents( QEventLoop::AllEvents, 50 );
      else
        QThread::msleep( 10 );
    }
    t->wait();   // join the thread
  };

  // Wait for the main service thread first, then sub-workers
  waitPumpingEvents( this );
  waitPumpingEvents( m_fanControlWorker.get() );
  waitPumpingEvents( m_cpuWorker.get() );
  waitPumpingEvents( m_displayWorker.get() );
  waitPumpingEvents( m_hardwareMonitorWorker.get() );

  // Phase 3: DBus cleanup on the main thread (where the objects live).
  // onExit() runs on the worker thread, so DBus operations there would
  // violate Qt's thread-affinity rules.
  if ( m_started )
  {
    try
    {
      QDBusConnection bus = QDBusConnection::systemBus();
      bus.unregisterService( SERVICE_NAME );
      bus.unregisterObject( OBJECT_PATH );
      syslog( LOG_INFO, "DBus service unregistered" );
    }
    catch ( const std::exception &e )
    {
      syslog( LOG_ERR, "DBus cleanup error: %s", e.what() );
    }
    m_adaptor.reset();
    m_dbusObject.reset();
    m_started = false;
  }

  syslog( LOG_INFO, "All workers stopped" );
}

void UccDBusService::onStart()
{
  m_started = true;
}

void UccDBusService::onWork()
{
  if ( not m_started )
    return;

  // update tuxedo wmi availability (matches typescript implementation)
  m_dbusData.tuxedoWmiAvailable = m_io.wmiAvailable();

  // Periodic NVIDIA cTGP offset validation (every 5 ticks = 5 s)
  if ( m_dbusData.nvidiaPowerCTRLAvailable.load() && m_profileSettingsWorker )
  {
    ++m_nvidiaValidationCounter;
    if ( m_nvidiaValidationCounter >= 5 )
    {
      m_nvidiaValidationCounter = 0;
      m_profileSettingsWorker->validateNVIDIACTGPOffset();
    }
  }

  // Fan data is now updated by FanControlWorker

  // check sensor data collection timeout
  auto now = std::chrono::steady_clock::now();
  if ( m_adaptor )
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    auto elapsed = std::chrono::duration_cast< std::chrono::milliseconds >(
      now - m_adaptor->m_lastDataCollectionAccess ).count();

    if ( elapsed > 10000 )
      m_dbusData.sensorDataCollectionStatus = false;
  }

  // STATE-BASED PROFILE SWITCHING (like TypeScript StateSwitcherWorker)
  // Disabled: uccd no longer saves or monitors settings file
  // UCC handles all profile decisions

  // Monitor power state and emit signals for UCC to handle
  // Skip AC/BAT changes when water cooler is connected (power_wc takes priority)
  if ( m_currentState != ProfileState::WC )
  {
    const ProfileState newState = determineState();
    const std::string stateKey = profileStateToString( newState );

    if ( newState != m_currentState )
    {
      m_currentState = newState;

      // Update m_currentStateProfileId only if the state is mapped
      auto it = m_settings.stateMap.find( stateKey );
      m_currentStateProfileId = ( it != m_settings.stateMap.end() ) ? it->second : std::string();

      std::cout << "[State] Power state changed to " << stateKey << std::endl;

      // Emit signal for UCC to handle profile switching
      m_adaptor->emitPowerStateChanged( stateKey );
    }
  }

  // Check for temp profile requests
  const std::string oldActiveProfileId = m_activeProfile.id;
  const std::string oldActiveProfileName = m_activeProfile.name;

  // Check if a temp profile by ID was requested
  std::string profileId;
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    if ( m_dbusData.tempProfileId.empty() || m_dbusData.tempProfileId == oldActiveProfileId )
    {
      profileId.clear();
    }
    else
    {
      profileId = m_dbusData.tempProfileId;
      m_dbusData.tempProfileId.clear(); // Clear before applying
    }
  }

  if ( !profileId.empty() )
  {
    std::cout << "[Profile] Applying temp profile by ID: " << profileId << std::endl;
    if ( setCurrentProfileById( profileId ) )
    {
      std::cout << "[Profile] Successfully switched to profile ID: " << profileId << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to switch to profile ID: " << profileId << std::endl;
    }

    return; // Process one change per cycle
  }

  // Check if a temp profile by name was requested
  std::string profileName;
  {
    std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
    if ( m_dbusData.tempProfileName.empty() || m_dbusData.tempProfileName == oldActiveProfileName )
    {
      profileName.clear();
    }
    else
    {
      profileName = m_dbusData.tempProfileName;
      m_dbusData.tempProfileName.clear(); // Clear before applying
    }
  }

  if ( !profileName.empty() )
  {
    std::cout << "[Profile] Applying temp profile by name: " << profileName << std::endl;
    if ( setCurrentProfileByName( profileName ) )
    {
      std::cout << "[Profile] Successfully switched to profile: " << profileName << std::endl;
    }
    else
    {
      std::cerr << "[Profile] Failed to switch to profile: " << profileName << std::endl;
    }

    return; // Process one change per cycle
  }

  // emit signal if mode reapply is pending
  if ( m_dbusData.modeReapplyPending and m_adaptor )
    m_adaptor->emitModeReapplyPendingChanged( true );

  // Check water cooler connection state changes and switch power state.
  // Debounce: BLE connections are inherently unstable – the water cooler
  // may briefly disconnect (UART error) and reconnect within seconds.
  // We only act on a state change once it has been stable for a
  // configurable number of seconds (shorter for connect, longer for
  // disconnect so a quick reconnect does not trigger a power-state flip).
  bool wcConnected = m_dbusData.waterCoolerConnected;

  if ( wcConnected != m_previousWaterCoolerConnected )
  {
    // Raw flag differs from the last accepted state.
    if ( !m_wcDebouncePending || m_wcDebouncedTarget != wcConnected )
    {
      // First time we see this new value (or direction changed) – start timer.
      m_wcDebouncePending  = true;
      m_wcDebouncedTarget  = wcConnected;
      m_wcDebounceStart    = std::chrono::steady_clock::now();
    }
    else
    {
      // Still waiting for the same direction – check elapsed time.
      const int requiredSeconds = wcConnected ? WC_CONNECT_DEBOUNCE_S
                                              : WC_DISCONNECT_DEBOUNCE_S;
      auto elapsed = std::chrono::steady_clock::now() - m_wcDebounceStart;
      if ( std::chrono::duration_cast< std::chrono::seconds >( elapsed ).count()
           >= requiredSeconds )
      {
        // Stable long enough – accept the change.
        m_wcDebouncePending = false;
        m_previousWaterCoolerConnected = wcConnected;

        const std::string status = wcConnected ? "connected" : "disconnected";
        syslog( LOG_INFO, "Water cooler status changed to: %s (debounced)", status.c_str() );

        // Emit signal for applications to handle water cooler status changes
        m_adaptor->emitWaterCoolerStatusChanged( status );

        // Switch power state based on water cooler connection and apply the
        // corresponding profile so the system actually transitions.
        if ( wcConnected )
        {
          m_currentState = ProfileState::WC;
          const std::string stateKey = "power_wc";
          std::cout << "[State] Water cooler connected, switching to " << stateKey << std::endl;
          m_adaptor->emitPowerStateChanged( stateKey );
        }
        else
        {
          m_currentState = determineState();
          const std::string stateKey = profileStateToString( m_currentState );
          std::cout << "[State] Water cooler disconnected, reverting to " << stateKey << std::endl;
          m_adaptor->emitPowerStateChanged( stateKey );
        }

        // Reset pump hysteresis so the auto-control loop picks up the correct
        // level for the new profile from a clean state.
        m_pumpHysSpeedIdx = 0;
        m_pumpHysThreshold = 0;
        m_wcTempFiltered = -1.0;  // reset EWMA so next sample seeds immediately

        applyProfileForCurrentState();
      }
    }
  }
  else
  {
    // Raw flag matches accepted state – cancel any pending debounce.
    m_wcDebouncePending = false;
  }
}

void UccDBusService::setWaterCoolerScanningEnabled( bool enable )
{
  // Caller may hold no locks; update dbus data and request worker actions.
  m_dbusData.waterCoolerScanningEnabled = enable;

  if ( not enable )
  {
    m_dbusData.waterCoolerAvailable = false;
    m_dbusData.waterCoolerConnected = false;
  }

  if ( not m_waterCoolerWorker )
    return;

  if ( enable )
  {
    // Only start scanning if not already scanning/connected.
    // startScanning() calls cleanupBleController() which would tear down
    // an active BLE connection, causing pump/fan commands to fail.
    if ( not m_dbusData.waterCoolerAvailable.load() and not m_dbusData.waterCoolerConnected.load() )
      m_waterCoolerWorker->startScanning();
  }
  else
  {
    // Stop discovery and disconnect; stopScanning() now also disconnects the device
    m_waterCoolerWorker->stopScanning();
  }
}

void UccDBusService::onExit()
{
  // Only do thread-safe work here — this runs on the DaemonWorker thread.
  // DBus cleanup happens in shutdown() on the main thread.
  saveAutosave();
}

// profile management implementation

void UccDBusService::loadProfiles()
{
  std::cout << "[ProfileManager] Loading profiles..." << std::endl;

  // identify device for device-specific profiles
  auto device = identifyDevice();

  // load default profiles
  m_defaultProfiles = m_profileManager.getDefaultProfiles( device );
  std::cout << "[ProfileManager] Loaded " << m_defaultProfiles.size() << " default profiles" << std::endl;

  // Fill device-specific defaults (TDP values, etc.) after loading profiles
  fillDeviceSpecificDefaults( m_defaultProfiles );

  // Debug: Verify TDP values were filled
  std::cout << "[loadProfiles] After fillDeviceSpecificDefaults, checking TDP values:" << std::endl;
  for ( size_t i = 0; i < m_defaultProfiles.size() && i < 3; ++i )
  {
    std::cout << "[loadProfiles]   Default profile " << i << " (" << m_defaultProfiles[i].id
              << ") has " << m_defaultProfiles[i].odmPowerLimits.tdpValues.size() << " TDP values";
    if ( !m_defaultProfiles[i].odmPowerLimits.tdpValues.empty() )
    {
      std::cout << ": [";
      for ( size_t j = 0; j < m_defaultProfiles[i].odmPowerLimits.tdpValues.size(); ++j )
      {
        if ( j > 0 ) std::cout << ", ";
        std::cout << m_defaultProfiles[i].odmPowerLimits.tdpValues[j];
      }
      std::cout << "]";
    }
    std::cout << std::endl;
  }
}

void UccDBusService::initializeProfiles()
{
  loadProfiles();

  // Don't set any active profile on startup - let UCC handle this
  // Only refresh if we already have an active profile (from autosave)
  if ( !m_activeProfile.id.empty() )
  {
    // Refresh the active profile from the reloaded profiles
    // in case it was modified
    std::string currentId = m_activeProfile.id;
    if ( !setCurrentProfileById( currentId ) )
    {
      // Profile no longer exists, clear it
      m_activeProfile = UccProfile();
    }
  }

  // update dbus data with profile JSONs
  updateDBusActiveProfileData();

  const int32_t defaultOnlineCores = getDefaultOnlineCores();
  const int32_t defaultScalingMin = getCpuMinFrequency();
  const int32_t defaultScalingMax = getCpuMaxFrequency();

  UccProfile baseCustomProfile = m_profileManager.getDefaultCustomProfiles()[0];

  // serialize all profiles to JSON
  std::ostringstream allProfilesJSON;
  allProfilesJSON << "[";

  auto allProfiles = getAllProfiles();
  for ( size_t i = 0; i < allProfiles.size(); ++i )
  {
    if ( i > 0 )
      allProfilesJSON << ",";

    allProfilesJSON << profileToJSON( allProfiles[ i ],
                      defaultOnlineCores,
                      defaultScalingMin,
                      defaultScalingMax );
  }
  allProfilesJSON << "]";

  std::ostringstream defaultProfilesJSON;
  defaultProfilesJSON << "[";
  for ( size_t i = 0; i < m_defaultProfiles.size(); ++i )
  {
    if ( i > 0 )
      defaultProfilesJSON << ",";

    defaultProfilesJSON << profileToJSON( m_defaultProfiles[ i ],
                        defaultOnlineCores,
                        defaultScalingMin,
                        defaultScalingMax );
  }
  defaultProfilesJSON << "]";

  std::ostringstream customProfilesJSON;
  customProfilesJSON << "[";
  for ( size_t i = 0; i < m_customProfiles.size(); ++i )
  {
    if ( i > 0 )
      customProfilesJSON << ",";

    customProfilesJSON << profileToJSON( m_customProfiles[ i ],
                       defaultOnlineCores,
                       defaultScalingMin,
                       defaultScalingMax );
  }
  customProfilesJSON << "]";

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.profilesJSON = defaultProfilesJSON.str();  // Only default profiles now
  m_dbusData.defaultProfilesJSON = defaultProfilesJSON.str();
  m_dbusData.customProfilesJSON = "[]";  // Empty array since custom profiles are local
  m_dbusData.defaultValuesProfileJSON = profileToJSON( baseCustomProfile,
                                                       defaultOnlineCores,
                                                       defaultScalingMin,
                                                       defaultScalingMax );

  std::cout << "[DBus] Updated profile JSONs:" << std::endl;
  std::cout << "[DBus]   customProfilesJSON: " << m_dbusData.customProfilesJSON.length() << " bytes, "
            << m_customProfiles.size() << " profiles" << std::endl;
  std::cout << "[DBus]   defaultProfilesJSON: " << m_dbusData.defaultProfilesJSON.length() << " bytes, "
            << m_defaultProfiles.size() << " profiles" << std::endl;

}

UccProfile UccDBusService::getCurrentProfile() const
{
  return m_activeProfile;
}

bool UccDBusService::setCurrentProfileByName( const std::string &profileName )
{
  auto allProfiles = getAllProfiles();

  for ( const auto &profile : allProfiles )
  {
    if ( profile.name == profileName )
    {
      const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
      m_activeProfile = profile;
      m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
      snapProfileFrequencies( m_activeProfile );
      updateDBusActiveProfileData();
      return true;
    }
  }

  // fallback to default profile
  const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
  m_activeProfile = getDefaultProfile();
  m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
  snapProfileFrequencies( m_activeProfile );
  updateDBusActiveProfileData();
  return false;
}

bool UccDBusService::setCurrentProfileById( const std::string &id )
{
  auto allProfiles = getAllProfiles();

  for ( const auto &profile : allProfiles )
  {
    if ( profile.id == id )
    {
      std::cout << "[Profile] Switching to profile: " << profile.name << " (ID: " << id << ")" << std::endl;
      // Preserve runtime water cooler enable state across profile switches.
      // The user's explicit EnableWaterCooler() D-Bus call is authoritative.
      const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
      m_activeProfile = profile;
      m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
      snapProfileFrequencies( m_activeProfile );
      updateDBusActiveProfileData();

      // apply fan curves and pump auto-control
      applyFanAndPumpSettings( profile );

      // apply new profile to workers
      if ( m_cpuWorker )
      {
        std::cout << "[Profile] Applying CPU settings from profile" << std::endl;
        m_cpuWorker->reapplyProfile();
      }
      if ( m_profileSettingsWorker )
      {
        std::cout << "[Profile] Applying TDP settings from profile" << std::endl;
        m_profileSettingsWorker->reapplyProfile();

        // Re-read TDP values after apply so D-Bus data reflects new hardware state
        readHardwareCapabilities();

        // Apply charging profile if the profile specifies one
        if ( !profile.chargingProfile.empty() && m_dbusData.chargingProfilesAvailable != "[]" )
        {
          std::cout << "[Profile] Applying charging profile '" << profile.chargingProfile << "'" << std::endl;
          if ( m_profileSettingsWorker->applyChargingProfile( profile.chargingProfile ) )
          {
            std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
            m_dbusData.currentChargingProfile = profile.chargingProfile;
          }
        }

        // Apply charging priority if the profile specifies one
        if ( !profile.chargingPriority.empty() && m_dbusData.chargingPrioritiesAvailable != "[]" )
        {
          std::cout << "[Profile] Applying charging priority '" << profile.chargingPriority << "'" << std::endl;
          if ( m_profileSettingsWorker->applyChargingPriority( profile.chargingPriority ) )
          {
            std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
            m_dbusData.currentChargingPriority = profile.chargingPriority;
          }
        }

        // Apply charge type and thresholds if the profile specifies them
        if ( !profile.chargeType.empty() )
        {
          std::cout << "[Profile] Applying charge type '" << profile.chargeType << "'" << std::endl;
          if ( m_profileSettingsWorker->setChargeType( profile.chargeType ) )
          {
            std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
            m_dbusData.chargeType = profile.chargeType;
          }
        }
        if ( profile.chargeStartThreshold >= 0 )
        {
          std::cout << "[Profile] Applying charge start threshold " << profile.chargeStartThreshold << std::endl;
          if ( m_profileSettingsWorker->setChargeStartThreshold( profile.chargeStartThreshold ) )
            m_dbusData.chargeStartThreshold = profile.chargeStartThreshold;
        }
        if ( profile.chargeEndThreshold >= 0 )
        {
          std::cout << "[Profile] Applying charge end threshold " << profile.chargeEndThreshold << std::endl;
          if ( m_profileSettingsWorker->setChargeEndThreshold( profile.chargeEndThreshold ) )
            m_dbusData.chargeEndThreshold = profile.chargeEndThreshold;
        }
      }

      if ( m_keyboardBacklightController.isAvailable()
           && m_settings.keyboardBacklightControlEnabled
           && !profile.keyboard.keyboardProfileData.empty()
           && profile.keyboard.keyboardProfileData != "{}" )
      {
        bool kbResult = m_keyboardBacklightController.applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );
        std::cout << "[Profile] Keyboard apply result: " << ( kbResult ? "SUCCESS" : "FAILED" ) << std::endl;
      }
      else
      {
        std::cout << "[Profile] Keyboard apply SKIPPED — one or more conditions not met" << std::endl;
      }

      // Emit ProfileChanged signal for DBus clients
      if ( m_adaptor )
        m_adaptor->emitProfileChanged( id,
                                       profile.keyboard.keyboardProfileId,
                                       profile.fan.fanProfile );

      return true;
    }
  }

  // fallback to default profile
  std::cout << "[Profile] Profile ID not found: " << id << ", using default" << std::endl;
  {
    const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
    m_activeProfile = getDefaultProfile();
    m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
  }
  snapProfileFrequencies( m_activeProfile );
  updateDBusActiveProfileData();

  // Emit ProfileChanged signal for DBus clients
  if ( m_adaptor )
  {
    m_adaptor->emitProfileChanged( m_activeProfile.id,
                                   m_activeProfile.keyboard.keyboardProfileId,
                                   m_activeProfile.fan.fanProfile );
  }

  return false;
}

bool UccDBusService::applyProfileJSON( const std::string &profileJSON )
{
  try
  {
    // Parse the profile JSON
    auto profile = m_profileManager.parseProfileJSON( profileJSON );

    std::cout << "[Profile] Applying profile from GUI: " << profile.name << std::endl;

    // Set as active profile, but preserve the runtime water cooler enable state.
    // The user's explicit EnableWaterCooler() D-Bus call is authoritative;
    // the stored profile may have a stale enableWaterCooler value.
    const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
    m_activeProfile = profile;
    m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
    snapProfileFrequencies( m_activeProfile );
    updateDBusActiveProfileData();

    // If the profile explicitly sets sameSpeed, apply it to fan worker immediately
    try
    {
      if ( m_fanControlWorker )
      {
        bool same = m_activeProfile.fan.sameSpeed;
        m_fanControlWorker->setSameSpeed( same );
        syslog( LOG_INFO, "UccDBusService: applied sameSpeed=%d from profile", same ? 1 : 0 );
      }
    }
    catch ( ... ) { /* ignore */ }

    // Try to resolve and apply fan curves: prefer embedded tables, fallback to named fan profile
    try
    {
      std::vector< FanTableEntry > cpuTable;
      std::vector< FanTableEntry > gpuTable;
      std::vector< FanTableEntry > wcFanTable;
      std::vector< FanTableEntry > pumpTable;

      // First, try embedded tables from the profile itself
      if ( profile.fan.hasEmbeddedTables() )
      {
        cpuTable = profile.fan.tableCPU;
        gpuTable = profile.fan.tableGPU;
        wcFanTable = profile.fan.tableWaterCoolerFan;
        pumpTable = profile.fan.tablePump;
        std::cout << "[Profile] Using embedded fan tables from profile" << std::endl;
      }
      else
      {
        // Fallback: resolve from named fan profile preset
        const std::string fpName = profile.fan.fanProfile;
        if ( !fpName.empty() )
        {
          FanProfile fp = getDefaultFanProfile( fpName );
          if ( fp.isValid() )
          {
            cpuTable = fp.tableCPU;
            gpuTable = fp.tableGPU;
            wcFanTable = fp.tableWaterCoolerFan;
            pumpTable = fp.tablePump;
            std::cout << "[Profile] Using fan tables from named profile '" << fpName << "'" << std::endl;
          }
        }
      }

      if ( m_fanControlWorker && !cpuTable.empty() )
      {
        m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, wcFanTable, pumpTable );
        std::cout << "[Profile] Applied fan curves (CPU=" << cpuTable.size()
                  << " GPU=" << gpuTable.size()
                  << " WCFan=" << wcFanTable.size()
                  << " Pump=" << pumpTable.size() << ")" << std::endl;
      }

      // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
      if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
           && !pumpTable.empty() )
      {
        int maxTemp = 0;
        {
          std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
          for ( const auto &fan : m_dbusData.fans )
            maxTemp = std::max( maxTemp, fan.temp.data );
        }
        FanProfile tempFp;
        tempFp.tablePump = pumpTable;
        // Reset hysteresis before a one-shot profile apply so the continuous
        // loop re-initialises to the correct level on the next tick.
        m_pumpHysSpeedIdx = 0;
        m_pumpHysThreshold = 0;
        m_waterCoolerWorker->setPumpVoltage( static_cast<int>( tempFp.getPumpSpeedForTemp( maxTemp ) ) );
        std::cout << "[Profile] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
      }
    }
    catch ( ... ) { /* ignore parse errors and continue applying other profile settings */ }

    // Apply to workers
    if ( m_cpuWorker )
    {
      std::cout << "[Profile] Applying CPU settings from profile" << std::endl;
      m_cpuWorker->reapplyProfile();
    }
    if ( m_profileSettingsWorker )
    {
      std::cout << "[Profile] Applying TDP settings from profile" << std::endl;
      m_profileSettingsWorker->reapplyProfile();

      // Re-read TDP values after apply so D-Bus data reflects new hardware state
      readHardwareCapabilities();

      // Apply charging profile if the profile specifies one
      if ( !profile.chargingProfile.empty() && m_dbusData.chargingProfilesAvailable != "[]" )
      {
        std::cout << "[Profile] Applying charging profile '" << profile.chargingProfile << "'" << std::endl;
        if ( m_profileSettingsWorker->applyChargingProfile( profile.chargingProfile ) )
        {
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.currentChargingProfile = profile.chargingProfile;
        }
      }

      // Apply charging priority if the profile specifies one
      if ( !profile.chargingPriority.empty() && m_dbusData.chargingPrioritiesAvailable != "[]" )
      {
        std::cout << "[Profile] Applying charging priority '" << profile.chargingPriority << "'" << std::endl;
        if ( m_profileSettingsWorker->applyChargingPriority( profile.chargingPriority ) )
        {
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.currentChargingPriority = profile.chargingPriority;
        }
      }

      // Apply charge type and thresholds if the profile specifies them
      if ( !profile.chargeType.empty() )
      {
        std::cout << "[Profile] Applying charge type '" << profile.chargeType << "'" << std::endl;
        if ( m_profileSettingsWorker->setChargeType( profile.chargeType ) )
        {
          std::lock_guard< std::mutex > lk( m_dbusData.dataMutex );
          m_dbusData.chargeType = profile.chargeType;
        }
      }
      if ( profile.chargeStartThreshold >= 0 )
      {
        std::cout << "[Profile] Applying charge start threshold " << profile.chargeStartThreshold << std::endl;
        if ( m_profileSettingsWorker->setChargeStartThreshold( profile.chargeStartThreshold ) )
          m_dbusData.chargeStartThreshold = profile.chargeStartThreshold;
      }
      if ( profile.chargeEndThreshold >= 0 )
      {
        std::cout << "[Profile] Applying charge end threshold " << profile.chargeEndThreshold << std::endl;
        if ( m_profileSettingsWorker->setChargeEndThreshold( profile.chargeEndThreshold ) )
          m_dbusData.chargeEndThreshold = profile.chargeEndThreshold;
      }
    }

    if ( m_keyboardBacklightController.isAvailable()
         && m_settings.keyboardBacklightControlEnabled
         && !profile.keyboard.keyboardProfileData.empty()
         && profile.keyboard.keyboardProfileData != "{}" )
    {
      std::cout << "[Profile] Applying keyboard backlight settings from profile" << std::endl;
      m_keyboardBacklightController.applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );
    }

    // Apply GPU OC and cTGP from the profile
    applyGpuOCFromProfile( profile );

    // Emit ProfileChanged signal for DBus clients
    if ( m_adaptor )
    {
      m_adaptor->emitProfileChanged( profile.id,
                                     profile.keyboard.keyboardProfileId,
                                     profile.fan.fanProfile );
    }

    return true;
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[Profile] Failed to apply profile JSON: " << e.what() << std::endl;
    return false;
  }
}


std::vector< UccProfile > UccDBusService::getAllProfiles() const
{
  std::vector< UccProfile > allProfiles;
  allProfiles.reserve( m_defaultProfiles.size() + m_customProfiles.size() );

  allProfiles.insert( allProfiles.end(), m_defaultProfiles.begin(), m_defaultProfiles.end() );
  allProfiles.insert( allProfiles.end(), m_customProfiles.begin(), m_customProfiles.end() );

  return allProfiles;
}

std::vector< UccProfile > UccDBusService::getDefaultProfiles() const
{
  return m_defaultProfiles;
}

std::vector< UccProfile > UccDBusService::getCustomProfiles() const
{
  return m_customProfiles;
}

UccProfile UccDBusService::getDefaultProfile() const
{
  if ( not m_defaultProfiles.empty() )
    return m_defaultProfiles[0];

  if ( not m_customProfiles.empty() )
    return m_customProfiles[0];

  // ultimate fallback
  return defaultCustomProfile;
}

void UccDBusService::updateDBusActiveProfileData()
{
  const int32_t defaultOnlineCores = getDefaultOnlineCores();
  const int32_t defaultScalingMin = -1;
  const int32_t defaultScalingMax = -1;

  std::string profileJSON = profileToJSON( m_activeProfile,
                                           defaultOnlineCores,
                                           defaultScalingMin,
                                           defaultScalingMax );
  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.activeProfileJSON = profileJSON;
}

void UccDBusService::updateDBusSettingsData()
{
  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.settingsJSON = buildSettingsJSON( m_dbusData.keyboardBacklightStatesJSON,
                                               m_dbusData.currentChargingProfile,
                                               m_settings );
}

bool UccDBusService::addCustomProfile( const UccProfile &profile )
{
  std::cout << "[ProfileManager] Adding profile '" << profile.name << "' to memory" << std::endl;

  // Add to in-memory profiles
  m_customProfiles.push_back( profile );

  // Update DBus data
  updateDBusActiveProfileData();

  std::cout << "[ProfileManager] Profile added successfully" << std::endl;
  return true;
}

bool UccDBusService::deleteCustomProfile( const std::string &profileId )
{
  std::cout << "[ProfileManager] Deleting profile '" << profileId << "' from memory" << std::endl;

  // Remove from in-memory profiles
  if ( auto it = std::remove_if( m_customProfiles.begin(), m_customProfiles.end(),
                           [&profileId]( const UccProfile &p ) { return p.id == profileId; } );
       it != m_customProfiles.end() )
  {
    m_customProfiles.erase( it, m_customProfiles.end() );

    // Update DBus data
    updateDBusActiveProfileData();

    std::cout << "[ProfileManager] Profile deleted successfully" << std::endl;
    return true;
  }

  std::cerr << "[ProfileManager] Profile not found" << std::endl;
  return false;
}

bool UccDBusService::updateCustomProfile( const UccProfile &profile )
{
  std::cout << "[ProfileManager] Updating profile '" << profile.name << "' (ID: " << profile.id << ") in memory" << std::endl;
  std::cout << "[ProfileManager]   Active profile ID: '" << m_activeProfile.id << "'" << std::endl;
  std::cout << "[ProfileManager]   Incoming keyboard data length: " << profile.keyboard.keyboardProfileData.size()
            << " bytes, profileName='" << profile.keyboard.keyboardProfileName << "'" << std::endl;

  // Check if this is a default (hardcoded) profile
  bool isDefaultProfile = false;
  for ( const auto &defaultProf : m_defaultProfiles )
  {
    if ( defaultProf.id == profile.id )
    {
      isDefaultProfile = true;
      break;
    }
  }

  if ( isDefaultProfile )
  {
    std::cout << "[ProfileManager] Cannot update hardcoded default profile '" << profile.id << "'" << std::endl;
    std::cout << "[ProfileManager] Default profiles are read-only." << std::endl;
    std::cerr << "[ProfileManager] ERROR: Attempt to modify read-only default profile rejected!" << std::endl;
    return false;
  }

  // Update in-memory profile
  if ( auto it = std::ranges::find_if( m_customProfiles,
                         [&profile]( const UccProfile &p ) { return p.id == profile.id; } );
       it != m_customProfiles.end() )
  {
    *it = profile;

    // Update DBus data
    updateDBusActiveProfileData();

    // Update active profile if it was the one modified
    if ( m_activeProfile.id == profile.id )
    {
      std::cout << "[ProfileManager] Updated profile is active, reapplying to system" << std::endl;
      const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
      m_activeProfile = profile;
      m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
      snapProfileFrequencies( m_activeProfile );
      // Reapply the profile to actually update the hardware/system settings
      if ( setCurrentProfileById( profile.id ) )
      {
        std::cout << "[ProfileManager] Successfully reapplied updated profile to system" << std::endl;
      }
      else
      {
        std::cerr << "[ProfileManager] Failed to reapply updated profile!" << std::endl;
      }
    }
    else
    {
      std::cout << "[ProfileManager] Updated profile is NOT the active profile — not reapplying" << std::endl;
      std::cout << "[ProfileManager]   active='" << m_activeProfile.id << "' vs saved='" << profile.id << "'" << std::endl;
    }

    std::cout << "[ProfileManager] Profile updated successfully" << std::endl;
    return true;
  }

  std::cerr << "[ProfileManager] Profile not found for update" << std::endl;
  return false;
}

void UccDBusService::initializeDisplayModes()
{
  // Detect session type by reading /proc/<pid>/environ directly (no shell).
  // The old popen()-based approach was vulnerable to environment injection.
  std::string sessionType;
  const uid_t rootUid = 0;

  try
  {
    namespace fs = std::filesystem;
    for ( const auto &entry : fs::directory_iterator( "/proc" ) )
    {
      if ( not sessionType.empty() )
        break;

      const std::string pidName = entry.path().filename().string();
      if ( pidName.empty() or not std::isdigit( static_cast< unsigned char >( pidName[0] ) ) )
        continue;

      // Skip root-owned processes
      std::string loginuidPath = entry.path().string() + "/loginuid";
      std::ifstream loginuidFile( loginuidPath );
      if ( loginuidFile )
      {
        uid_t uid = 0;
        loginuidFile >> uid;
        if ( uid == rootUid or uid == static_cast< uid_t >( -1 ) )
          continue;
      }
      else
      {
        continue;
      }

      std::string environPath = entry.path().string() + "/environ";
      std::ifstream envFile( environPath, std::ios::binary );
      if ( not envFile )
        continue;

      std::string envContent( ( std::istreambuf_iterator< char >( envFile ) ),
                               std::istreambuf_iterator< char >() );

      std::istringstream envStream( envContent );
      std::string envEntry;
      while ( std::getline( envStream, envEntry, '\0' ) )
      {
        if ( envEntry.starts_with( "XDG_SESSION_TYPE=" ) )
        {
          sessionType = envEntry.substr( 17 );
          break;
        }
      }
    }
  }
  catch ( ... )
  {
    // Fall through with empty sessionType
  }

  // trim whitespace
  while ( not sessionType.empty() and
          ( sessionType.back() == '\n' or sessionType.back() == '\r' or
            sessionType.back() == ' ' or sessionType.back() == '\t' ) )
  {
    sessionType.pop_back();
  }

  m_dbusData.isX11 = ( sessionType == "x11" );

  // initialize display modes as empty array - will be populated by display worker if implemented
  // must be valid JSON (empty array, not empty string) for GUI to parse correctly
  m_dbusData.displayModes = "[]";
}

std::optional< UniwillDeviceID > UccDBusService::identifyDevice()
{
  // read dmi information from sysfs
  const std::string dmiBasePath = "/sys/class/dmi/id";
  const std::string productSKU = SysfsNode< std::string >( dmiBasePath + "/product_sku" ).read().value_or( "" );
  const std::string boardName = SysfsNode< std::string >( dmiBasePath + "/board_name" ).read().value_or( "" );

  // get module info from tuxedo_io
  std::string deviceModelId;
  m_io.deviceModelIdStr( deviceModelId );

  // create dmi sku to device map (matches typescript version)
  std::map< std::string, UniwillDeviceID > dmiSKUDeviceMap;
  dmiSKUDeviceMap[ "IBS1706" ] = UniwillDeviceID::IBP17G6;
  dmiSKUDeviceMap[ "IBP1XI08MK1" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP1XI08MK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP14I08MK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP16I08MK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "OMNIA08IMK2" ] = UniwillDeviceID::IBPG8;
  dmiSKUDeviceMap[ "IBP14A10MK1 / IBP15A10MK1" ] = UniwillDeviceID::IBPG10AMD;
  dmiSKUDeviceMap[ "IIBP14A10MK1 / IBP15A10MK1" ] = UniwillDeviceID::IBPG10AMD;
  dmiSKUDeviceMap[ "POLARIS1XA02" ] = UniwillDeviceID::POLARIS1XA02;
  dmiSKUDeviceMap[ "POLARIS1XI02" ] = UniwillDeviceID::POLARIS1XI02;
  dmiSKUDeviceMap[ "POLARIS1XA03" ] = UniwillDeviceID::POLARIS1XA03;
  dmiSKUDeviceMap[ "POLARIS1XI03" ] = UniwillDeviceID::POLARIS1XI03;
  dmiSKUDeviceMap[ "STELLARIS1XA03" ] = UniwillDeviceID::STELLARIS1XA03;
  dmiSKUDeviceMap[ "STEPOL1XA04" ] = UniwillDeviceID::STEPOL1XA04;
  dmiSKUDeviceMap[ "STELLARIS1XI03" ] = UniwillDeviceID::STELLARIS1XI03;
  dmiSKUDeviceMap[ "STELLARIS1XI04" ] = UniwillDeviceID::STELLARIS1XI04;
  dmiSKUDeviceMap[ "PULSE1502" ] = UniwillDeviceID::PULSE1502;
  dmiSKUDeviceMap[ "PULSE1403" ] = UniwillDeviceID::PULSE1403;
  dmiSKUDeviceMap[ "PULSE1404" ] = UniwillDeviceID::PULSE1404;
  dmiSKUDeviceMap[ "STELLARIS1XI05" ] = UniwillDeviceID::STELLARIS1XI05;
  dmiSKUDeviceMap[ "POLARIS1XA05" ] = UniwillDeviceID::POLARIS1XA05;
  dmiSKUDeviceMap[ "STELLARIS1XA05" ] = UniwillDeviceID::STELLARIS1XA05;
  dmiSKUDeviceMap[ "STELLARIS16I06" ] = UniwillDeviceID::STELLARIS16I06;
  dmiSKUDeviceMap[ "STELLARIS17I06" ] = UniwillDeviceID::STELLARIS17I06;
  dmiSKUDeviceMap[ "STELLSL15A06" ] = UniwillDeviceID::STELLSL15A06;
  dmiSKUDeviceMap[ "STELLSL15I06" ] = UniwillDeviceID::STELLSL15I06;
  dmiSKUDeviceMap[ "AURA14GEN3" ] = UniwillDeviceID::AURA14G3;
  dmiSKUDeviceMap[ "AURA15GEN3" ] = UniwillDeviceID::AURA15G3;
  dmiSKUDeviceMap[ "STELLARIS16A07" ] = UniwillDeviceID::STELLARIS16A07;
  dmiSKUDeviceMap[ "STELLARIS16I07" ] = UniwillDeviceID::STELLARIS16I07;
  dmiSKUDeviceMap[ "XNE16A25" ] = UniwillDeviceID::XNE16A25;
  dmiSKUDeviceMap[ "XNE16E25" ] = UniwillDeviceID::XNE16E25;
  dmiSKUDeviceMap[ "GEMINI17I04" ] = UniwillDeviceID::GEMINI17I04;
  dmiSKUDeviceMap[ "GEMINIGEN4I" ] = UniwillDeviceID::GEMINI17I04;
  dmiSKUDeviceMap[ "IBM15A10" ] = UniwillDeviceID::IBM15A10;
  dmiSKUDeviceMap[ "SIRIUS1601" ] = UniwillDeviceID::SIRIUS1601;
  dmiSKUDeviceMap[ "SIRIUS1602" ] = UniwillDeviceID::SIRIUS1602;

  // check for sku match
  if ( auto skuIt = dmiSKUDeviceMap.find( productSKU ); skuIt != dmiSKUDeviceMap.end() )
  {
    return skuIt->second;
  }

  // check uwid (univ wmi interface) device mapping
  std::map< int, UniwillDeviceID > uwidDeviceMap;
  uwidDeviceMap[ 0x13 ] = UniwillDeviceID::IBP14G6_TUX;
  uwidDeviceMap[ 0x12 ] = UniwillDeviceID::IBP14G6_TRX;
  uwidDeviceMap[ 0x14 ] = UniwillDeviceID::IBP14G6_TQF;
  uwidDeviceMap[ 0x17 ] = UniwillDeviceID::IBP14G7_AQF_ARX;

  int modelId = 0;
  try
  {
    modelId = std::stoi( deviceModelId );
  }
  catch ( ... )
  {
    // ignore parse errors
  }

  if ( auto uwidIt = uwidDeviceMap.find( modelId ); uwidIt != uwidDeviceMap.end() )
  {
    return uwidIt->second;
  }

  // no device match found
  return std::nullopt;
}

void UccDBusService::computeDeviceCapabilities()
{
  // Aquaris (LCT water cooler) is supported only on specific devices
  static const std::set< UniwillDeviceID > waterCoolerDevices =
  {
    UniwillDeviceID::STELLARIS1XI04,
    UniwillDeviceID::STEPOL1XA04,
    UniwillDeviceID::STELLARIS1XI05,
    UniwillDeviceID::STELLARIS16I06,
    UniwillDeviceID::STELLARIS17I06,
    UniwillDeviceID::STELLARIS16A07,
    UniwillDeviceID::XNE16A25,
    UniwillDeviceID::XNE16E25,
    UniwillDeviceID::STELLARIS16I07,
  };

  // cTGP adjustment is hidden for the IBP series (undefined behaviour despite nvidia-smi reporting support)
  static const std::set< UniwillDeviceID > cTGPHiddenDevices =
  {
    UniwillDeviceID::IBP14G6_TUX,
    UniwillDeviceID::IBP14G6_TRX,
    UniwillDeviceID::IBP14G6_TQF,
    UniwillDeviceID::IBP14G7_AQF_ARX,
    UniwillDeviceID::IBPG8,
    UniwillDeviceID::IBPG10AMD,
  };

  if ( m_deviceId.has_value() )
  {
    m_dbusData.waterCoolerSupported = waterCoolerDevices.count( m_deviceId.value() ) > 0;
    m_dbusData.cTGPAdjustmentSupported = cTGPHiddenDevices.count( m_deviceId.value() ) == 0;
  }
  else
  {
    // Unknown device: water cooler not available, cTGP defers to hardware detection
    m_dbusData.waterCoolerSupported = false;

    // For unknown devices, check if the hardware file exists (like TCC does)
    std::error_code ec;
    const std::string ctgpPath = "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";
    bool hardwareExists = std::filesystem::exists( ctgpPath, ec ) &&
                         std::filesystem::is_regular_file( ctgpPath, ec );
    m_dbusData.cTGPAdjustmentSupported = hardwareExists;
  }

  syslog( LOG_INFO, "Device capabilities: aquaris=%s, cTGP=%s",
          m_dbusData.waterCoolerSupported.load() ? "supported" : "not supported",
          m_dbusData.cTGPAdjustmentSupported.load() ? "supported" : "hidden" );
}

void UccDBusService::loadSettings()
{
  if ( auto loadedSettings = m_settingsManager.readSettings(); loadedSettings.has_value() )
  {
    m_settings = *loadedSettings;
    std::cout << "[Settings] Loaded existing settings" << std::endl;
  }
  else
  {
    // Fresh install — create default settings with sensible stateMap.
    // Pick the default profile with the highest total TDP so the GUI
    // immediately shows meaningful power-limit values instead of 0 W.
    m_settings = TccSettings();

    std::string bestProfileId;
    int64_t     bestTdpSum = -1;

    for ( const auto &profile : m_defaultProfiles )
    {
      int64_t sum = 0;
      for ( int v : profile.odmPowerLimits.tdpValues )
        sum += v;

      if ( sum > bestTdpSum )
      {
        bestTdpSum = sum;
        bestProfileId = profile.id;
      }
    }

    if ( bestProfileId.empty() && !m_defaultProfiles.empty() )
      bestProfileId = m_defaultProfiles.back().id;

    if ( !bestProfileId.empty() )
    {
      m_settings.stateMap[ "power_ac" ]  = bestProfileId;
      m_settings.stateMap[ "power_bat" ] = bestProfileId;
      m_settings.stateMap[ "power_wc" ]  = bestProfileId;
    }

    std::cout << "[Settings] No settings file found — initialized default stateMap "
              << "(all states=" << bestProfileId << ")" << std::endl;
    updateDBusSettingsData();
  }

  // Load custom profiles from settings BEFORE validating stateMap
  for ( const auto &[profileId, profileJson] : m_settings.profiles )
  {
    try
    {
      auto profile = m_profileManager.parseProfileJSON( profileJson );
      m_customProfiles.push_back( profile );
      std::cout << "[Settings] Loaded profile '" << profile.name << "' (ID: " << profile.id << ") from settings" << std::endl;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[Settings] Failed to parse profile '" << profileId << "' from settings: " << e.what() << std::endl;
    }
  }

  // IMPORTANT: Do NOT resync/clear m_settings.profiles!
  // Reason: m_settings.profiles is the authoritative source from the file.
  // Resyncing can change keys or representation, breaking stateMap lookups.
  // Keep m_settings.profiles exactly as loaded from file.
  // Only modify it when profiles are explicitly added/edited via API.

  // validate and fix state map if needed
  auto allProfiles = getAllProfiles();
  bool settingsChanged = false;

  for ( const auto &stateKey : { "power_ac", "power_bat", "power_wc" } )
  {
    // check if state key exists in map – if not, leave it unassigned
    if ( m_settings.stateMap.find( stateKey ) == m_settings.stateMap.end() )
    {
      std::cout << "[Settings] No profile assigned to state '"
                << stateKey << "', waiting for ucc-gui" << std::endl;
      continue;
    }

    auto &profileId = m_settings.stateMap[stateKey];

    // check if assigned profile exists (either in m_customProfiles OR in m_settings.profiles)
    bool profileExists = false;

    // First check if it exists as a loaded custom profile object
    for ( const auto &profile : m_customProfiles )
    {
      if ( profile.id == profileId )
      {
        profileExists = true;
        break;
      }
    }

    // If not found in objects, check if it exists as a key in m_settings.profiles (JSON map)
    // This is important because a profile might be in the file but not yet parsed into an object
    if ( !profileExists )
    {
      profileExists = ( m_settings.profiles.find( profileId ) != m_settings.profiles.end() );
    }

    // Also check default profiles
    if ( !profileExists )
    {
      for ( const auto &profile : m_defaultProfiles )
      {
        if ( profile.id == profileId )
        {
          profileExists = true;
          break;
        }
      }
    }

    if ( not profileExists )
    {
      std::cout << "[Settings] Profile ID '" << profileId << "' for state '"
                << stateKey << "' not found, removing assignment" << std::endl;
      m_settings.stateMap.erase( stateKey );
      settingsChanged = true;
    }
  }

  if ( settingsChanged )
  {
    if ( m_settingsManager.writeSettings( m_settings ) )
    {
      std::cout << "[Settings] Saved updated settings" << std::endl;
      updateDBusSettingsData();
    }
    else
    {
      std::cerr << "[Settings] Failed to update settings!" << std::endl;
    }
  }

  // Sync ycbcr420Workaround with detected display ports
  if ( syncOutputPortsSetting() )
  {
    if ( m_settingsManager.writeSettings( m_settings ) )
    {
      std::cout << "[Settings] Synced ycbcr420Workaround settings" << std::endl;
      updateDBusSettingsData();
    }
  }
}

void UccDBusService::initializeStartupProfile()
{
  UccProfile resolved = m_profileManager.resolveStartupProfile(
    m_deviceId,
    m_settings.stateMap,
    m_settings.profiles
  );

  if ( resolved.id.empty() )
  {
    syslog( LOG_INFO, "[Startup] No startup profile resolved — no state map entry for current power state" );
    return;
  }

  m_activeProfile = resolved;
  m_currentState = determineState();
  m_currentStateProfileId = resolved.id;

  snapProfileFrequencies( m_activeProfile );
  updateDBusActiveProfileData();

  syslog( LOG_INFO, "[Startup] Applying startup profile: %s (ID: %s)", resolved.name.c_str(), resolved.id.c_str() );
  applyStartupProfile();
}

void UccDBusService::applyStartupProfile()
{
  if ( m_activeProfile.id.empty() )
    return;

  const auto &profile = m_activeProfile;

  applyFanAndPumpSettings( profile );

  if ( m_cpuWorker )
    m_cpuWorker->reapplyProfile();

  if ( m_profileSettingsWorker )
    m_profileSettingsWorker->reapplyProfile();

  if ( m_keyboardBacklightController.isAvailable()
       && m_settings.keyboardBacklightControlEnabled
       && !profile.keyboard.keyboardProfileData.empty()
       && profile.keyboard.keyboardProfileData != "{}" )
    m_keyboardBacklightController.applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );

  applyGpuOCFromProfile( profile );

  if ( m_dbusData.waterCoolerSupported )
    setWaterCoolerScanningEnabled( profile.fan.enableWaterCooler );
}

void UccDBusService::applyFanAndPumpSettings( const UccProfile &profile )
{
  // Apply sameSpeed setting to fan worker
  if ( m_fanControlWorker )
    m_fanControlWorker->setSameSpeed( profile.fan.sameSpeed );

  // Resolve and apply fan curves: prefer embedded tables, fallback to named profile
  try
  {
    std::vector< FanTableEntry > cpuTable;
    std::vector< FanTableEntry > gpuTable;
    std::vector< FanTableEntry > wcFanTable;
    std::vector< FanTableEntry > pumpTable;

    if ( profile.fan.hasEmbeddedTables() )
    {
      cpuTable = profile.fan.tableCPU;
      gpuTable = profile.fan.tableGPU;
      wcFanTable = profile.fan.tableWaterCoolerFan;
      pumpTable = profile.fan.tablePump;
      std::cout << "[FanPump] Using embedded fan tables from profile" << std::endl;
    }
    else
    {
      const std::string &fpName = profile.fan.fanProfile;
      if ( !fpName.empty() )
      {
        FanProfile fp = getDefaultFanProfile( fpName );
        if ( fp.isValid() )
        {
          cpuTable = fp.tableCPU;
          gpuTable = fp.tableGPU;
          wcFanTable = fp.tableWaterCoolerFan;
          pumpTable = fp.tablePump;
          std::cout << "[FanPump] Using fan tables from named profile '" << fpName << "'" << std::endl;
        }
      }
    }

    if ( m_fanControlWorker && !cpuTable.empty() )
    {
      m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, wcFanTable, pumpTable );
      std::cout << "[FanPump] Applied fan curves (CPU=" << cpuTable.size()
                << " GPU=" << gpuTable.size()
                << " WCFan=" << wcFanTable.size()
                << " Pump=" << pumpTable.size() << ")" << std::endl;
    }

    // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
    if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
         && !pumpTable.empty() )
    {
      int maxTemp = 0;
      {
        std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
        for ( const auto &fan : m_dbusData.fans )
          maxTemp = std::max( maxTemp, fan.temp.data );
      }
      FanProfile tempFp;
      tempFp.tablePump = pumpTable;
      // Reset hysteresis before a one-shot profile apply so the continuous
      // loop re-initialises to the correct level on the next tick.
      m_pumpHysSpeedIdx = 0;
      m_pumpHysThreshold = 0;
      m_waterCoolerWorker->setPumpVoltage( static_cast<int>( tempFp.getPumpSpeedForTemp( maxTemp ) ) );
      std::cout << "[FanPump] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
    }
  }
  catch ( const std::exception &e )
  {
    std::cerr << "[FanPump] Failed to apply fan/pump settings: " << e.what() << std::endl;
  }
}

void UccDBusService::applyGpuOCFromProfile( const UccProfile &profile )
{
  // GPU OC / cTGP data lives exclusively inside gpuOCProfileData.
  // Profiles with no GPU profile selected have empty gpuOCProfileData and
  // should not touch GPU state at all.
  if ( profile.gpuOCProfileData.empty() || profile.gpuOCProfileData == "{}" )
    return;

  // Extract and apply cTGP offset from embedded GPU profile data
  if ( m_profileSettingsWorker && m_dbusData.nvidiaPowerCTRLAvailable.load() )
  {
    QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( profile.gpuOCProfileData ) );
    if ( doc.isObject() )
    {
      QJsonObject obj = doc.object();
      if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj[ "nvidiaPowerCTRLProfile" ].isObject() )
      {
        const int ctgpOffset = obj[ "nvidiaPowerCTRLProfile" ].toObject().value( "cTGPOffset" ).toInt( 0 );
        std::cout << "[GpuOC] Applying cTGP offset from profile: " << ctgpOffset << std::endl;
        m_profileSettingsWorker->applyNVIDIAPowerOffset( ctgpOffset );
      }
    }
  }

  // Apply GPU OC settings (clock offsets, locked clocks, power limit)
  if ( m_nvidiaOCWorker && m_nvidiaOCWorker->isAvailable() )
  {
    std::cout << "[GpuOc] Applying embedded GPU OC profile data from profile '"
              << profile.name << "'" << std::endl;
    if ( !m_nvidiaOCWorker->applyGpuOCProfile( profile.gpuOCProfileData, 0 ) )
      std::cerr << "[GpuOC] Failed to apply GPU OC profile data" << std::endl;
  }
}

void UccDBusService::applyProfileForCurrentState()
{
  const std::string stateKey = profileStateToString( m_currentState );
  auto stateMapIt = m_settings.stateMap.find( stateKey );
  if ( stateMapIt == m_settings.stateMap.end() )
  {
    std::cerr << "[State] No profile assigned to state '" << stateKey << "'" << std::endl;
    return;
  }

  const std::string &profileId = stateMapIt->second;
  m_currentStateProfileId = profileId;

  std::cout << "[State] Applying profile for state '" << stateKey << "': " << profileId << std::endl;

  // Lambda to apply all profile settings (fan curves, sameSpeed, CPU, ODM, keyboard, pump auto-control)
  auto applyFullProfile = [this]( const UccProfile &profile )
  {
    // Preserve runtime water cooler enable state across profile re-application.
    // The user's explicit EnableWaterCooler() D-Bus call is authoritative;
    // the stored profile may have a stale enableWaterCooler value.
    const bool preservedWcEnable = m_dbusData.waterCoolerScanningEnabled.load();
    m_activeProfile = profile;
    m_activeProfile.fan.enableWaterCooler = preservedWcEnable;
    snapProfileFrequencies( m_activeProfile );
    updateDBusActiveProfileData();

    // Apply sameSpeed setting to fan worker
    if ( m_fanControlWorker )
    {
      m_fanControlWorker->setSameSpeed( profile.fan.sameSpeed );
    }

    // Resolve and apply fan curves: prefer embedded tables, fallback to named profile
    try
    {
      std::vector< FanTableEntry > cpuTable;
      std::vector< FanTableEntry > gpuTable;
      std::vector< FanTableEntry > wcFanTable;
      std::vector< FanTableEntry > pumpTable;

      if ( profile.fan.hasEmbeddedTables() )
      {
        cpuTable = profile.fan.tableCPU;
        gpuTable = profile.fan.tableGPU;
        wcFanTable = profile.fan.tableWaterCoolerFan;
        pumpTable = profile.fan.tablePump;
        std::cout << "[State] Using embedded fan tables from profile" << std::endl;
      }
      else
      {
        const std::string &fpName = profile.fan.fanProfile;
        if ( !fpName.empty() )
        {
          FanProfile fp = getDefaultFanProfile( fpName );
          if ( fp.isValid() )
          {
            cpuTable = fp.tableCPU;
            gpuTable = fp.tableGPU;
            wcFanTable = fp.tableWaterCoolerFan;
            pumpTable = fp.tablePump;
            std::cout << "[State] Using fan tables from named profile '" << fpName << "'" << std::endl;
          }
        }
      }

      if ( m_fanControlWorker && !cpuTable.empty() )
      {
        m_fanControlWorker->applyTemporaryFanCurves( cpuTable, gpuTable, wcFanTable, pumpTable );
        std::cout << "[State] Applied fan curves (CPU=" << cpuTable.size()
                  << " GPU=" << gpuTable.size()
                  << " WCFan=" << wcFanTable.size()
                  << " Pump=" << pumpTable.size() << ")" << std::endl;
      }

      // Apply pump auto-control if water cooler is connected and autoControlWC is enabled
      if ( profile.fan.autoControlWC && m_waterCoolerWorker && m_dbusData.waterCoolerConnected.load()
           && !pumpTable.empty() )
      {
        int maxTemp = 0;
        {
          std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
          for ( const auto &fan : m_dbusData.fans )
            maxTemp = std::max( maxTemp, fan.temp.data );
        }
        FanProfile tempFp;
        tempFp.tablePump = pumpTable;
        // Reset hysteresis before a one-shot profile apply so the continuous
        // loop re-initialises to the correct level on the next tick.
        m_pumpHysSpeedIdx = 0;
        m_pumpHysThreshold = 0;
        m_waterCoolerWorker->setPumpVoltage( static_cast<int>( tempFp.getPumpSpeedForTemp( maxTemp ) ) );
        std::cout << "[State] Applied pump voltage for temp " << maxTemp << "°C" << std::endl;
      }
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[State] Failed to apply fan curves: " << e.what() << std::endl;
    }

    // Apply CPU/ODM/keyboard workers
    if ( m_cpuWorker )
      m_cpuWorker->reapplyProfile();
    if ( m_profileSettingsWorker )
      m_profileSettingsWorker->reapplyProfile();
    if ( m_keyboardBacklightController.isAvailable()
         && m_settings.keyboardBacklightControlEnabled
         && !profile.keyboard.keyboardProfileData.empty()
         && profile.keyboard.keyboardProfileData != "{}" )
      m_keyboardBacklightController.applyProfileKeyboardStates( profile.keyboard.keyboardProfileData );

    // Apply GPU OC and cTGP from the profile
    applyGpuOCFromProfile( profile );

    // Water cooler scanning state is preserved from the runtime flag
    // (set via EnableWaterCooler D-Bus call). Do NOT re-read enableWaterCooler
    // from the stored profile — it may be stale.

    // Emit ProfileChanged signal for DBus clients
    if ( m_adaptor )
      m_adaptor->emitProfileChanged( profile.id,
                                     profile.keyboard.keyboardProfileId,
                                     profile.fan.fanProfile );
  };

  // Try persistent (custom) profiles first
  if ( auto profileIt = m_settings.profiles.find( profileId );
       profileIt != m_settings.profiles.end() )
  {
    try
    {
      auto profile = m_profileManager.parseProfileJSON( profileIt->second );
      std::cout << "[State] Applied profile from settings: " << profile.name
                << " (ID: " << profile.id << ")" << std::endl;
      applyFullProfile( profile );
      return;
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[State] Failed to parse profile '" << profileId << "': " << e.what() << std::endl;
    }
  }

  // Fall back to built-in profiles
  for ( const auto &profile : getAllProfiles() )
  {
    if ( profile.id == profileId )
    {
      std::cout << "[State] Applied built-in profile: " << profile.name
                << " (ID: " << profile.id << ")" << std::endl;
      applyFullProfile( profile );
      return;
    }
  }

  std::cerr << "[State] WARNING: Profile ID '" << profileId << "' not found for state '" << stateKey << "'" << std::endl;
}

void UccDBusService::serializeProfilesJSON()
{
  std::cout << "[serializeProfilesJSON] Starting profile serialization" << std::endl;
  std::cout << "[serializeProfilesJSON] Default profiles count: " << m_defaultProfiles.size() << std::endl;

  // Debug: Check TDP values before serialization
  for ( size_t i = 0; i < m_defaultProfiles.size() && i < 3; ++i )
  {
    std::cout << "[serializeProfilesJSON]   Profile " << i << " (" << m_defaultProfiles[i].id
              << ") has " << m_defaultProfiles[i].odmPowerLimits.tdpValues.size() << " TDP values" << std::endl;
    if ( !m_defaultProfiles[i].odmPowerLimits.tdpValues.empty() )
    {
      std::cout << "[serializeProfilesJSON]     TDP values: [";
      for ( size_t j = 0; j < m_defaultProfiles[i].odmPowerLimits.tdpValues.size(); ++j )
      {
        if ( j > 0 ) std::cout << ", ";
        std::cout << m_defaultProfiles[i].odmPowerLimits.tdpValues[j];
      }
      std::cout << "]" << std::endl;
    }
  }

  const int32_t defaultOnlineCores = getDefaultOnlineCores();
  const int32_t defaultScalingMin = getCpuMinFrequency();
  const int32_t defaultScalingMax = getCpuMaxFrequency();

  UccProfile defaultProfile = m_profileManager.getDefaultCustomProfiles()[0];

  // serialize all profiles to JSON
  std::ostringstream allProfilesJSON;
  allProfilesJSON << "[";

  auto allProfiles = getAllProfiles();
  for ( size_t i = 0; i < allProfiles.size(); ++i )
  {
    if ( i > 0 )
      allProfilesJSON << ",";

    allProfilesJSON << profileToJSON( allProfiles[ i ],
                      defaultOnlineCores,
                      defaultScalingMin,
                      defaultScalingMax );
  }
  allProfilesJSON << "]";

  std::ostringstream defaultProfilesJSON;
  defaultProfilesJSON << "[";
  for ( size_t i = 0; i < m_defaultProfiles.size(); ++i )
  {
    if ( i > 0 )
      defaultProfilesJSON << ",";

    defaultProfilesJSON << profileToJSON( m_defaultProfiles[ i ],
                        defaultOnlineCores,
                        defaultScalingMin,
                        defaultScalingMax );
  }
  defaultProfilesJSON << "]";

  std::ostringstream customProfilesJSON;
  customProfilesJSON << "[";
  for ( size_t i = 0; i < m_customProfiles.size(); ++i )
  {
    if ( i > 0 )
      customProfilesJSON << ",";

    customProfilesJSON << profileToJSON( m_customProfiles[ i ],
                       defaultOnlineCores,
                       defaultScalingMin,
                       defaultScalingMax );
  }
  customProfilesJSON << "]";

  std::lock_guard< std::mutex > lock( m_dbusData.dataMutex );
  m_dbusData.profilesJSON = defaultProfilesJSON.str();  // Only default profiles now
  m_dbusData.defaultProfilesJSON = defaultProfilesJSON.str();
  m_dbusData.customProfilesJSON = "[]";  // Empty array since custom profiles are local
  m_dbusData.defaultValuesProfileJSON = profileToJSON( defaultProfile,
                                                       defaultOnlineCores,
                                                       defaultScalingMin,
                                                       defaultScalingMax );

  std::cout << "[DBus] Re-serialized profile JSONs" << std::endl;
}

void UccDBusService::fillDeviceSpecificDefaults( std::vector< UccProfile > &profiles )
{
  const int32_t cpuMinFreq = getCpuMinFrequency();
  const int32_t cpuMaxFreq = getCpuMaxFrequency();

  // Get TDP info directly from hardware I/O
  std::vector< TDPInfo > tdpInfo;
  {
    int nrTDPs = 0;
    if ( m_io.getNumberTDPs( nrTDPs ) and nrTDPs > 0 )
    {
      std::vector< std::string > descriptors;
      m_io.getTDPDescriptors( descriptors );

      for ( int i = 0; i < nrTDPs; ++i )
      {
        TDPInfo info;
        info.current = 0;
        info.min = 0;
        info.max = 0;
        info.descriptor = ( i < static_cast< int >( descriptors.size() ) )
                            ? descriptors[ static_cast< size_t >( i ) ]
                            : "";

        m_io.getTDPMin( i, reinterpret_cast< int & >( info.min ) );
        m_io.getTDPMax( i, reinterpret_cast< int & >( info.max ) );
        m_io.getTDP( i, reinterpret_cast< int & >( info.current ) );

        tdpInfo.push_back( info );
      }

      std::cout << "[fillDeviceSpecificDefaults] TDP info available: " << tdpInfo.size() << " entries" << std::endl;
      for ( size_t i = 0; i < tdpInfo.size(); ++i )
      {
        std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "]: min=" << tdpInfo[i].min
                  << ", max=" << tdpInfo[i].max << ", current=" << tdpInfo[i].current << std::endl;
      }
    }
    else
    {
      std::cout << "[fillDeviceSpecificDefaults] No TDP hardware available" << std::endl;
    }
  }

  for ( auto &profile : profiles )
  {
    std::cout << "[fillDeviceSpecificDefaults] Filling profile: " << profile.id
              << ", current TDP values: " << profile.odmPowerLimits.tdpValues.size() << std::endl;

    // Fill CPU frequency defaults
    if ( !profile.cpu.scalingMinFrequency.has_value() || profile.cpu.scalingMinFrequency.value() < cpuMinFreq )
    {
      profile.cpu.scalingMinFrequency = cpuMinFreq;
    }

    if ( !profile.cpu.scalingMaxFrequency.has_value() )
    {
      profile.cpu.scalingMaxFrequency = cpuMaxFreq;
    }
    else if ( profile.cpu.scalingMaxFrequency.value() < profile.cpu.scalingMinFrequency.value() )
    {
      profile.cpu.scalingMaxFrequency = profile.cpu.scalingMinFrequency;
    }
    else if ( profile.cpu.scalingMaxFrequency.value() > cpuMaxFreq )
    {
      profile.cpu.scalingMaxFrequency = cpuMaxFreq;
    }

    // Fill TDP values if missing and hardware TDP info is available
    if ( !tdpInfo.empty() && tdpInfo.size() > profile.odmPowerLimits.tdpValues.size() )
    {
      const size_t nrMissingValues = tdpInfo.size() - profile.odmPowerLimits.tdpValues.size();
      std::cout << "[fillDeviceSpecificDefaults]   Adding " << nrMissingValues << " TDP values" << std::endl;
      // Add missing TDP values with max values from hardware
      for ( size_t i = profile.odmPowerLimits.tdpValues.size(); i < tdpInfo.size(); ++i )
      {
        profile.odmPowerLimits.tdpValues.push_back( static_cast< int >( tdpInfo[i].max ) );
        std::cout << "[fillDeviceSpecificDefaults]     Added TDP[" << i << "] = " << tdpInfo[i].max << std::endl;
      }
    }

    // Clamp existing TDP values to hardware min/max range.
    // Default profiles have hardcoded placeholders (e.g. {5,10,15}) that
    // may lie outside the actual hardware capabilities.
    if ( !tdpInfo.empty() )
    {
      for ( size_t i = 0; i < profile.odmPowerLimits.tdpValues.size() && i < tdpInfo.size(); ++i )
      {
        int &val = profile.odmPowerLimits.tdpValues[ i ];
        const int hwMin = static_cast< int >( tdpInfo[ i ].min );
        const int hwMax = static_cast< int >( tdpInfo[ i ].max );

        if ( val < hwMin )
        {
          std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "] " << val
                    << " below hw min " << hwMin << ", clamping" << std::endl;
          val = hwMin;
        }
        else if ( val > hwMax )
        {
          std::cout << "[fillDeviceSpecificDefaults]   TDP[" << i << "] " << val
                    << " above hw max " << hwMax << ", clamping" << std::endl;
          val = hwMax;
        }
      }
    }

    // Snap CPU frequencies to nearest available hardware frequency
    snapProfileFrequencies( profile );

    std::cout << "[fillDeviceSpecificDefaults]   Final TDP values: " << profile.odmPowerLimits.tdpValues.size() << std::endl;
  }
}

void UccDBusService::snapProfileFrequencies( UccProfile &profile )
{
  if ( m_cpuWorker )
    m_cpuWorker->snapProfileFrequencies( profile );
}

void UccDBusService::loadAutosave()
{
  m_autosave = m_autosaveManager.readAutosave();
  std::cout << "[Autosave] Loaded autosave (displayBrightness: "
            << m_autosave.displayBrightness << "%)" << std::endl;
}

void UccDBusService::saveAutosave()
{
  if ( m_autosaveManager.writeAutosave( m_autosave ) )
  {
    std::cout << "[Autosave] Saved autosave" << std::endl;
  }
  else
  {
    std::cerr << "[Autosave] Failed to save autosave!" << std::endl;
  }
}

std::vector< std::vector< std::string > > UccDBusService::getOutputPorts()
{
  std::vector< std::vector< std::string > > result;

  struct udev *udev_context = udev_new();
  if ( !udev_context )
  {
    std::cerr << "[OutputPorts] Failed to create udev context" << std::endl;
    return result;
  }

  struct udev_enumerate *drm_devices = udev_enumerate_new( udev_context );
  if ( !drm_devices )
  {
    std::cerr << "[OutputPorts] Failed to enumerate devices" << std::endl;
    udev_unref( udev_context );
    return result;
  }

  if ( udev_enumerate_add_match_subsystem( drm_devices, "drm" ) < 0 ||
       udev_enumerate_add_match_sysname( drm_devices, "card*-*-*" ) < 0 ||
       udev_enumerate_scan_devices( drm_devices ) < 0 )
  {
    std::cerr << "[OutputPorts] Failed to scan devices" << std::endl;
    udev_enumerate_unref( drm_devices );
    udev_unref( udev_context );
    return result;
  }

  struct udev_list_entry *drm_devices_iterator = udev_enumerate_get_list_entry( drm_devices );
  if ( !drm_devices_iterator )
  {
    udev_enumerate_unref( drm_devices );
    udev_unref( udev_context );
    return result;
  }

  struct udev_list_entry *drm_devices_entry;
  udev_list_entry_foreach( drm_devices_entry, drm_devices_iterator )
  {
    std::string path = udev_list_entry_get_name( drm_devices_entry );
    std::string name = path.substr( path.rfind( "/" ) + 1 );

    // Extract card number (e.g., "card0" -> 0)
    size_t cardPos = name.find( "card" );
    size_t dashPos = name.find( "-", cardPos );
    if ( cardPos == std::string::npos || dashPos == std::string::npos )
      continue;

    int cardNumber = std::stoi( name.substr( cardPos + 4, dashPos - cardPos - 4 ) );

    // Ensure result vector is large enough
    if ( static_cast< size_t >( cardNumber + 1 ) > result.size() )
    {
      result.resize( static_cast< size_t >( cardNumber + 1 ) );
    }

    // Extract port name (everything after "card0-")
    std::string portName = name.substr( dashPos + 1 );
    result[ static_cast< size_t >( cardNumber ) ].push_back( portName );
  }

  udev_enumerate_unref( drm_devices );
  udev_unref( udev_context );

  return result;
}

bool UccDBusService::syncOutputPortsSetting()
{
  bool settingsChanged = false;

  auto outputPorts = getOutputPorts();

  // Delete additional cards from settings
  if ( m_settings.ycbcr420Workaround.size() > outputPorts.size() )
  {
    m_settings.ycbcr420Workaround.resize( outputPorts.size() );
    settingsChanged = true;
  }

  for ( size_t card = 0; card < outputPorts.size(); ++card )
  {
    // Add card to settings if missing
    if ( m_settings.ycbcr420Workaround.size() <= card )
    {
      YCbCr420Card newCard;
      newCard.card = static_cast< int >( card );
      m_settings.ycbcr420Workaround.push_back( newCard );
      settingsChanged = true;
    }

    // Get reference to card settings
    auto &cardSettings = m_settings.ycbcr420Workaround[card];

    // Delete ports that no longer exist
    std::vector< std::string > portsToRemove;

    for ( const auto &portEntry : cardSettings.ports )
    {
      bool stillAvailable = false;
      for ( const auto &port : outputPorts[card] )
      {
        if ( portEntry.port == port )
        {
          stillAvailable = true;
          break;
        }
      }

      if ( !stillAvailable )
      {
        portsToRemove.push_back( portEntry.port );
      }
    }

    // Remove ports that are no longer available
    for ( const auto &port : portsToRemove )
    {
      cardSettings.ports.erase(
        std::remove_if( cardSettings.ports.begin(), cardSettings.ports.end(),
                       [&port]( const YCbCr420Port &p ) { return p.port == port; } ),
        cardSettings.ports.end()
      );
      settingsChanged = true;
    }

    // Add missing ports to settings
    for ( const auto &port : outputPorts[card] )
    {
      bool found = false;
      for ( const auto &portEntry : cardSettings.ports )
      {
        if ( portEntry.port == port )
        {
          found = true;
          break;
        }
      }

      if ( !found )
      {
        YCbCr420Port newPort;
        newPort.port = port;
        newPort.enabled = false;
        cardSettings.ports.push_back( newPort );
        settingsChanged = true;
      }
    }
  }

  return settingsChanged;
}
