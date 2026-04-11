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

#include "UccdClient.hpp"
#include <QDBusMessage>
#include <QDBusError>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QThread>
#include <QFile>
#include <QVariantMap>

namespace ucc
{

UccdClient::UccdClient( QObject *parent )
  : QObject( parent )
{
  // Initial connection attempt (uccd may not be running yet — that's fine)
  connectToDaemon();

  // Watch for uccd appearing or disappearing on the system bus
  m_serviceWatcher = new QDBusServiceWatcher(
    QLatin1String( DBUS_SERVICE ),
    QDBusConnection::systemBus(),
    QDBusServiceWatcher::WatchForOwnerChange,
    this );

  connect( m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered,
           this, &UccdClient::onServiceRegistered );
  connect( m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered,
           this, &UccdClient::onServiceUnregistered );

  emit connectionStatusChanged( m_connected );
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void UccdClient::subscribeDbusSignals()
{
  // Disconnect first so we never accumulate duplicate connections across
  // multiple reconnect cycles.
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "ProfileChanged", this,
                  SLOT( onProfileChangedSignal( QString, QString, QString, QString ) ) );
  QDBusConnection::systemBus().disconnect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
                  "PowerStateChanged", this,
                  SLOT( onPowerStateChangedSignal( QString ) ) );

  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "ProfileChanged", this,
               SLOT( onProfileChangedSignal( QString, QString, QString, QString ) ) );
  QDBusConnection::systemBus().connect( DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE,
               "PowerStateChanged", this,
               SLOT( onPowerStateChangedSignal( QString ) ) );
}

void UccdClient::connectToDaemon()
{
  // Check if the service actually has an owner on the bus.  We must NOT
  // just create a QDBusInterface, because with a D-Bus activation .service
  // file the bus would auto-start uccd during the introspection call that
  // QDBusInterface performs in its constructor.
  if ( auto *busIface = QDBusConnection::systemBus().interface();
       !busIface || !busIface->isServiceRegistered( QLatin1String( DBUS_SERVICE ) ) )
  {
    m_connected = false;
    m_interface.reset();  // no interface while disconnected
    qWarning() << "[UccdClient] uccd D-Bus service not registered on the system bus";
    return;
  }

  // The service is running — safe to introspect without triggering activation.
  // Do NOT pass a parent to QDBusInterface — unique_ptr owns its lifetime.
  m_interface = std::make_unique< QDBusInterface >(
    DBUS_SERVICE,
    DBUS_PATH,
    DBUS_INTERFACE,
    QDBusConnection::systemBus() );

  m_connected = m_interface->isValid();

  if ( m_connected )
  {
    subscribeDbusSignals();
  }
  else
  {
    qWarning() << "[UccdClient] uccd D-Bus interface not valid:"
               << m_interface->lastError().message();
  }
}

// ---------------------------------------------------------------------------
// D-Bus service watcher slots
// ---------------------------------------------------------------------------

void UccdClient::onServiceRegistered( const QString &service )
{
  Q_UNUSED( service )
  qInfo() << "[UccdClient] uccd appeared on the system bus — reconnecting";
  connectToDaemon();
  emit connectionStatusChanged( m_connected );
}

void UccdClient::onServiceUnregistered( const QString &service )
{
  Q_UNUSED( service )
  qWarning() << "[UccdClient] uccd disappeared from the system bus";
  m_connected = false;
  emit connectionStatusChanged( false );
}

bool UccdClient::isConnected() const
{
  return m_connected && m_interface && m_interface->isValid();
}

// Signal handlers
void UccdClient::onProfileChangedSignal( const QString &profileId,
                                         const QString &keyboardProfileId,
                                         const QString &fanProfileId,
                                         const QString &gpuProfileId )
{
  emit profileChanged( profileId, keyboardProfileId, fanProfileId, gpuProfileId );
}

void UccdClient::onPowerStateChangedSignal( const QString &state )
{
  emit powerStateChanged( state );
}

// Template implementations
template< typename T >
std::optional< T > UccdClient::callMethod( const QString &method ) const
{
  if ( !isConnected() )
  {
    return std::nullopt;
  }

  QDBusReply< T > reply = m_interface->call( method );
  if ( reply.isValid() )
  {
    return reply.value();
  }
  else
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.error().message();
    return std::nullopt;
  }
}

template< typename T, typename... Args >
std::optional< T > UccdClient::callMethod( const QString &method, const Args &...args ) const
{
  if ( !isConnected() )
  {
    return std::nullopt;
  }

  QDBusReply< T > reply = m_interface->call( method, args... );
  if ( reply.isValid() )
  {
    return reply.value();
  }
  else
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.error().message();
    return std::nullopt;
  }
}

bool UccdClient::callVoidMethod( const QString &method ) const
{
  if ( !isConnected() )
  {
    return false;
  }

  QDBusMessage reply = m_interface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage )
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.errorMessage();
    return false;
  }
  return true;
}

template< typename... Args >
bool UccdClient::callVoidMethod( const QString &method, const Args &...args ) const
{
  if ( !isConnected() )
  {
    return false;
  }

  QDBusMessage reply = m_interface->call( method, args... );
  if ( reply.type() == QDBusMessage::ErrorMessage )
  {
    qWarning() << "DBus call failed:" << method << "-" << reply.errorMessage();
    return false;
  }
  return true;
}

// System Information
std::optional< std::string > UccdClient::getSystemInfoJSON()
{
  if ( auto result = callMethod< QString >( "GetSystemInfoJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< bool > UccdClient::isDeviceSupported()
{
  return callMethod< bool >( "IsDeviceSupported" );
}

// Profile Management
std::optional< std::string > UccdClient::getDefaultProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetDefaultProfilesJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getCpuFrequencyLimitsJSON()
{
  if ( auto result = callMethod< QString >( "GetCpuFrequencyLimitsJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getDefaultValuesProfileJSON()
{
  if ( auto result = callMethod< QString >( "GetDefaultValuesProfileJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getCustomProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetCustomProfilesJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getActiveProfileJSON()
{
  if ( auto result = callMethod< QString >( "GetActiveProfileJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getSettingsJSON()
{
  if ( auto result = callMethod< QString >( "GetSettingsJSON" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getPowerState()
{
  if ( auto result = callMethod< QString >( "GetPowerState" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

bool UccdClient::setStateMap( const std::string &state, const std::string &profileId )
{
  const QString qState = QString::fromStdString( state );
  const QString qProfileId = QString::fromStdString( profileId );
  return callVoidMethod( "SetStateMap", qState, qProfileId );
}

bool UccdClient::setBatchStateMap( const std::map< std::string, std::string > &entries )
{
  QJsonObject obj;
  for ( const auto &[state, profileId] : entries )
    obj[QString::fromStdString( state )] = QString::fromStdString( profileId );
  QString json = QJsonDocument( obj ).toJson( QJsonDocument::Compact );
  return callVoidMethod( "SetBatchStateMap", json );
}

bool UccdClient::setActiveProfile( const std::string &profileId )
{
  const QString id = QString::fromStdString( profileId );
  return callVoidMethod( "SetActiveProfile", id );
}

bool UccdClient::applyProfile( const std::string &profileJSON )
{
  return callVoidMethod( "ApplyProfile", QString::fromStdString( profileJSON ) );
}

bool UccdClient::saveCustomProfile( [[maybe_unused]] [[maybe_unused]] const std::string &profileJSON )
{
  return callVoidMethod( "SaveCustomProfile", QString::fromStdString( profileJSON ) );
}

bool UccdClient::deleteCustomProfile( [[maybe_unused]] const std::string &profileId )
{
  return callVoidMethod( "DeleteCustomProfile", QString::fromStdString( profileId ) );
}

std::optional< std::string > UccdClient::getFanProfile( const std::string &fanProfileId )
{
  if ( auto result = callMethod< QString >( "GetFanProfile", QString::fromStdString( fanProfileId ) ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getFanProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetFanProfileNames" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getGpuProfile( const std::string &gpuProfileId )
{
  if ( auto result = callMethod< QString >( "GetGpuProfile", QString::fromStdString( gpuProfileId ) ) )
  {
    if ( const std::string json = result->toStdString(); !json.empty() && json != "{}" )
      return json;
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getGpuProfilesJSON()
{
  if ( auto result = callMethod< QString >( "GetGpuProfileNames" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

std::optional< bool > UccdClient::setFanProfile( const std::string &fanProfileId, const std::string &json )
{ return callMethod< bool >( "SetFanProfile", QString::fromStdString( fanProfileId ), QString::fromStdString( json ) ); }

bool UccdClient::setDisplayBrightness( int brightness )
{ return callVoidMethod( "SetDisplayBrightness", brightness ); }

std::optional< int > UccdClient::getDisplayBrightness()
{
  return callMethod< int >( "GetDisplayBrightness" );
}

bool UccdClient::setWebcamEnabled( bool enabled )
{
  return callVoidMethod( "SetWebcam", enabled );
}

std::optional< bool > UccdClient::getWebcamEnabled()
{
  return callMethod< bool >( "GetWebcamSWStatus" );
}

// GPU Info
std::optional< std::string > UccdClient::getGpuInfo()
{
  if ( auto result = callMethod< QString >( "GetGpuInfo" ) )
  {
    return result->toStdString();
  }
  return std::nullopt;
}

// Device Capability Queries
std::optional< bool > UccdClient::getWaterCoolerSupported()
{
  return callMethod< bool >( "GetWaterCoolerSupported" );
}

std::optional< bool > UccdClient::getCTGPAdjustmentSupported()
{
  return callMethod< bool >( "GetCTGPAdjustmentSupported" );
}

// Fn Lock
bool UccdClient::setFnLock( bool enabled )
{
  return callVoidMethod( "SetFnLockStatus", enabled );
}

std::optional< bool > UccdClient::getFnLock()
{
  return callMethod< bool >( "GetFnLockStatus" );
}

// Stub implementations for remaining methods
// TODO: Implement these based on actual uccd DBus interface

bool UccdClient::setYCbCr420Workaround( [[maybe_unused]] bool enabled )
{
  // TODO: Implement when DBus method is available
  return false;
}

std::optional< bool > UccdClient::getYCbCr420Workaround()
{
  return std::nullopt;
}

bool UccdClient::setDisplayRefreshRate( const std::string &display, int refreshRate )
{
  const QString qDisplay = QString::fromStdString( display );
  return callVoidMethod( "SetDisplayRefreshRate", qDisplay, refreshRate );
}

bool UccdClient::setCpuScalingGovernor( [[maybe_unused]] const std::string &governor )
{
  return false;
}

std::optional< std::string > UccdClient::getCpuScalingGovernor()
{
  return std::nullopt;
}

std::optional< std::vector< std::string > > UccdClient::getAvailableCpuGovernors()
{
  auto jsonStr = callMethod< QString >( "GetAvailableGovernors" );
  if ( !jsonStr )
    return std::nullopt;

  QJsonDocument doc = QJsonDocument::fromJson( jsonStr->toUtf8() );
  if ( !doc.isArray() )
    return std::nullopt;

  QJsonArray array = doc.array();
  std::vector< std::string > governors;
  for ( const QJsonValue &value : array )
  {
    if ( value.isString() )
    {
      governors.push_back( value.toString().toStdString() );
    }
  }
  return governors;
}

bool UccdClient::setCpuFrequency( [[maybe_unused]] int minFreq, [[maybe_unused]] int maxFreq )
{
  return false;
}

bool UccdClient::setEnergyPerformancePreference( [[maybe_unused]] const std::string &preference )
{
  return false;
}

std::optional< std::vector< std::string > > UccdClient::getAvailableEPPs()
{
  auto jsonStr = callMethod< QString >( "GetAvailableEPPs" );
  if ( !jsonStr )
    return std::nullopt;

  QJsonDocument doc = QJsonDocument::fromJson( jsonStr->toUtf8() );
  if ( !doc.isArray() )
    return std::nullopt;

  QJsonArray array = doc.array();
  std::vector< std::string > epps;
  for ( const QJsonValue &value : array )
  {
    if ( value.isString() )
    {
      epps.push_back( value.toString().toStdString() );
    }
  }
  return epps;
}

std::optional< int > UccdClient::getCpuCoreCount()
{
  return callMethod< int >( "GetCpuCoreCount" );
}

bool UccdClient::setFanProfile( [[maybe_unused]] [[maybe_unused]] const std::string &profileJSON )
{
  return false;
}

bool UccdClient::setFanProfileCPU( const std::string &pointsJSON )
{
  const QString js = QString::fromStdString( pointsJSON );
  return callMethod< bool, QString >( "SetFanProfileCPU", js ).value_or( false );
}

bool UccdClient::setFanProfileDGPU( const std::string &pointsJSON )
{
  const QString js = QString::fromStdString( pointsJSON );
  return callMethod< bool, QString >( "SetFanProfileDGPU", js ).value_or( false );
}

bool UccdClient::enableWaterCooler( bool enable )
{
  return callMethod< bool, bool >( "EnableWaterCooler", enable ).value_or( false );
}

std::optional< bool > UccdClient::isWaterCoolerEnabled()
{
  return callMethod< bool >( "IsWaterCoolerEnabled" );
}

bool UccdClient::applyFanProfiles( const std::string &fanProfilesJSON )
{
  const QString js = QString::fromStdString( fanProfilesJSON );
  return callMethod< bool, QString >( "ApplyFanProfiles", js ).value_or( false );
}

bool UccdClient::revertFanProfiles()
{
  return callMethod< bool >( "RevertFanProfiles" ).value_or( false );
}

std::optional< std::string > UccdClient::getCurrentFanSpeed()
{
  return std::nullopt;
}

std::optional< std::string > UccdClient::getFanTemperatures()
{
  return std::nullopt;
}

bool UccdClient::setODMPowerLimits( const std::vector< int > &limits )
{
  QJsonArray array;
  for ( int limit : limits )
    array.append( limit );

  const QString json = QString::fromUtf8( QJsonDocument( array ).toJson( QJsonDocument::Compact ) );
  return callMethod< bool, QString >( "SetODMPowerLimitsJSON", json ).value_or( false );
}

std::optional< std::vector< int > > UccdClient::getODMPowerLimits()
{
  auto jsonStr = callMethod< QString >( "ODMPowerLimitsJSON" );
  if ( !jsonStr )
    return std::nullopt;

  QJsonDocument doc = QJsonDocument::fromJson( jsonStr->toUtf8() );
  if ( !doc.isArray() )
    return std::nullopt;

  QJsonArray array = doc.array();
  std::vector< int > limits;
  for ( const QJsonValue &value : array )
  {
    if ( value.isObject() )
    {
      QJsonObject obj = value.toObject();
      if ( obj.contains( "max" ) && obj["max"].isDouble() )
      {
        limits.push_back( obj["max"].toInt() );
      }
    }
  }
  return limits;
}

bool UccdClient::setChargingProfile( const std::string &profileDescriptor )
{
  return callVoidMethod( "SetChargingProfile", QString::fromStdString( profileDescriptor ) );
}

std::optional< std::string > UccdClient::getChargingProfilesAvailable()
{
  if ( auto result = callMethod< QString >( "GetChargingProfilesAvailable" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentChargingProfile()
{
  if ( auto result = callMethod< QString >( "GetCurrentChargingProfile" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getChargingPrioritiesAvailable()
{
  if ( auto result = callMethod< QString >( "GetChargingPrioritiesAvailable" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getCurrentChargingPriority()
{
  if ( auto result = callMethod< QString >( "GetCurrentChargingPriority" ) )
    return result->toStdString();

  return std::nullopt;
}

bool UccdClient::setChargingPriority( const std::string &priorityDescriptor )
{
  return callVoidMethod( "SetChargingPriority", QString::fromStdString( priorityDescriptor ) );
}

std::optional< std::string > UccdClient::getChargeStartAvailableThresholds()
{
  if ( auto result = callMethod< QString >( "GetChargeStartAvailableThresholds" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< std::string > UccdClient::getChargeEndAvailableThresholds()
{
  if ( auto result = callMethod< QString >( "GetChargeEndAvailableThresholds" ) )
    return result->toStdString();

  return std::nullopt;
}

std::optional< int > UccdClient::getChargeStartThreshold()
{
  return callMethod< int >( "GetChargeStartThreshold" );
}

std::optional< int > UccdClient::getChargeEndThreshold()
{
  return callMethod< int >( "GetChargeEndThreshold" );
}

bool UccdClient::setChargeStartThreshold( int value )
{
  return callVoidMethod( "SetChargeStartThreshold", value );
}

bool UccdClient::setChargeEndThreshold( int value )
{
  return callVoidMethod( "SetChargeEndThreshold", value );
}

std::optional< std::string > UccdClient::getChargeType()
{
  if ( auto result = callMethod< QString >( "GetChargeType" ) )
    return result->toStdString();

  return std::nullopt;
}

bool UccdClient::setChargeType( const std::string &type )
{
  return callVoidMethod( "SetChargeType", QString::fromStdString( type ) );
}

bool UccdClient::setNVIDIAPowerOffset( int offset )
{
  return callVoidMethod( "SetNVIDIAPowerOffset", offset );
}

std::optional< int > UccdClient::getNVIDIAPowerOffset()
{
  if ( auto offset = callMethod< int >( "GetNVIDIAPowerOffset" ) )
    return offset;

  // Fallback for older daemons that do not expose the live cTGP getter.
  if ( auto json = getActiveProfileJSON() )
  {
    if ( QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *json ).toUtf8() ); doc.isObject() )
    {
      QJsonObject obj = doc.object();
      // cTGP offset lives inside the embedded GPU OC profile data
      if ( obj.contains( "gpuOCProfileData" ) && obj["gpuOCProfileData"].isObject() )
      {
        QJsonObject gpuObj = obj["gpuOCProfileData"].toObject();
        if ( gpuObj.contains( "nvidiaPowerCTRLProfile" ) && gpuObj["nvidiaPowerCTRLProfile"].isObject() )
        {
          if ( QJsonObject nvidiaObj = gpuObj["nvidiaPowerCTRLProfile"].toObject(); nvidiaObj.contains( "cTGPOffset" ) )
            return nvidiaObj["cTGPOffset"].toInt();
        }
      }
    }
  }
  return std::nullopt;
}

std::optional< int > UccdClient::getNVIDIAPowerCTRLMaxPowerLimit()
{
  return callMethod< int >( "GetNVIDIAPowerCTRLMaxPowerLimit" );
}

std::optional< int > UccdClient::getNVIDIAPowerCTRLDefaultPowerLimit()
{
  return callMethod< int >( "GetNVIDIAPowerCTRLDefaultPowerLimit" );
}

std::optional< bool > UccdClient::getNVIDIAPowerCTRLAvailable()
{
  return callMethod< bool >( "GetNVIDIAPowerCTRLAvailable" );
}

bool UccdClient::setPrimeProfile( [[maybe_unused]] const std::string &profile )
{
  // Prime profile switching is not supported as a standalone D-Bus call.
  // Prime state is detected automatically by the daemon's HardwareMonitorWorker.
  return false;
}

std::optional< std::string > UccdClient::getPrimeProfile()
{
  if ( auto result = callMethod< QString >( "GetPrimeState" ) )
    return result->toStdString();
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// NVIDIA GPU OC Control
// ---------------------------------------------------------------------------

std::optional< bool > UccdClient::getNvidiaOCAvailable()
{
  return callMethod< bool >( "GetNvidiaOCAvailable" );
}

std::optional< std::string > UccdClient::getNvidiaOCState( int deviceIndex )
{
  if ( auto result = callMethod< QString, int >( "GetNvidiaOCState", deviceIndex ) )
    return result->toStdString();
  return std::nullopt;
}

bool UccdClient::setNvidiaClockOffset( int deviceIndex, int clockType, int pstate, int offsetMHz )
{
  return callMethod< bool, int, int, int, int >( "SetNvidiaClockOffset",
      deviceIndex, clockType, pstate, offsetMHz ).value_or( false );
}

bool UccdClient::setNvidiaGpuLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  return callMethod< bool, int, int, int >( "SetNvidiaGpuLockedClocks",
      deviceIndex, minMHz, maxMHz ).value_or( false );
}

bool UccdClient::setNvidiaVramLockedClocks( int deviceIndex, int minMHz, int maxMHz )
{
  return callMethod< bool, int, int, int >( "SetNvidiaVramLockedClocks",
      deviceIndex, minMHz, maxMHz ).value_or( false );
}

bool UccdClient::resetNvidiaGpuLockedClocks( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaGpuLockedClocks", deviceIndex ).value_or( false );
}

bool UccdClient::resetNvidiaVramLockedClocks( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaVramLockedClocks", deviceIndex ).value_or( false );
}

bool UccdClient::resetNvidiaAllClockOffsets( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaAllClockOffsets", deviceIndex ).value_or( false );
}

bool UccdClient::setNvidiaGpuPowerLimit( int deviceIndex, double watts )
{
  return callMethod< bool, int, double >( "SetNvidiaGpuPowerLimit",
      deviceIndex, watts ).value_or( false );
}

bool UccdClient::resetNvidiaGpuPowerLimit( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaGpuPowerLimit", deviceIndex ).value_or( false );
}

bool UccdClient::applyNvidiaGpuOCProfile( const std::string &profileJSON, int deviceIndex )
{
  return callMethod< bool, QString, int >( "ApplyNvidiaGpuOCProfile",
      QString::fromStdString( profileJSON ), deviceIndex ).value_or( false );
}

bool UccdClient::resetNvidiaGpuOCAll( int deviceIndex )
{
  return callMethod< bool, int >( "ResetNvidiaGpuOCAll", deviceIndex ).value_or( false );
}

bool UccdClient::setKeyboardBacklight( const std::string &config )
{
  return callMethod< bool, QString >( "SetKeyboardBacklightStatesJSON", QString::fromStdString( config ) ).value_or( false );
}

std::optional< std::string > UccdClient::getKeyboardBacklightInfo()
{
  if ( auto caps = callMethod< QString >( "GetKeyboardBacklightCapabilitiesJSON" ); caps )
  {
    return caps->toStdString();
  }
  return std::nullopt;
}

std::optional< std::string > UccdClient::getKeyboardBacklightStates()
{
  if ( auto states = callMethod< QString >( "GetKeyboardBacklightStatesJSON" ); states )
  {
    return states->toStdString();
  }
  return std::nullopt;
}

bool UccdClient::setODMPerformanceProfile( [[maybe_unused]] const std::string &profile )
{
  return false;
}

std::optional< std::string > UccdClient::getODMPerformanceProfile()
{
  return std::nullopt;
}

std::optional< std::vector< std::string > > UccdClient::getAvailableODMProfiles()
{
  return std::nullopt;
}

namespace
{
std::optional< int > readFanDataValue( QDBusInterface *iface, const QString &method, const QString &key )
{
  if ( !iface )
  {
    return std::nullopt;
  }

  QDBusMessage reply = iface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
  {
    return std::nullopt;
  }

  // The adaptor returns QVariantMap (D-Bus a{sv}).
  // Each value in the outer map is a variant wrapping another a{sv}.
  // Demarshall the outer map first.
  QVariantMap outerMap = qdbus_cast< QVariantMap >( reply.arguments().at( 0 ).value< QDBusArgument >() );
  if ( !outerMap.contains( key ) )
    return std::nullopt;

  // The inner value is a variant containing a QDBusArgument for the nested a{sv}.
  QVariant innerVariant = outerMap.value( key );
  QVariantMap innerMap;
  if ( innerVariant.canConvert< QDBusArgument >() )
    innerMap = qdbus_cast< QVariantMap >( innerVariant.value< QDBusArgument >() );
  else
    innerMap = innerVariant.toMap();

  if ( !innerMap.contains( "data" ) )
    return std::nullopt;

  int value = innerMap.value( "data" ).toInt();

  // Treat entries with a zero timestamp as missing data
  if ( innerMap.contains( "timestamp" ) )
  {
    qint64 ts = innerMap.value( "timestamp" ).toLongLong();
    if ( ts == 0 )
    {
      // Silently treat as missing data - this is normal during startup
      // before fan monitoring worker has collected initial readings
      return std::nullopt;
    }
  }

  if ( value >= 0 )
    return value;

  return std::nullopt;
}

std::optional< int > readJsonInt( QDBusInterface *iface, const QString &method, const QString &key )
{
  if ( !iface )
  {
    return std::nullopt;
  }

  QDBusMessage reply = iface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
  {
    return std::nullopt;
  }

  const QString json = reply.arguments().at( 0 ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
  {
    return std::nullopt;
  }

  QJsonObject obj = doc.object();
  if ( !obj.contains( key ) )
  {
    return std::nullopt;
  }

  int val = obj[ key ].toInt();
  return ( val >= 0 ) ? std::optional< int >( val ) : std::nullopt;
}

std::optional< double > readJsonDouble( QDBusInterface *iface, const QString &method, const QString &key )
{
  if ( !iface )
  {
    return std::nullopt;
  }

  QDBusMessage reply = iface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
  {
    return std::nullopt;
  }

  const QString json = reply.arguments().at( 0 ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
  {
    return std::nullopt;
  }

  QJsonObject obj = doc.object();
  if ( !obj.contains( key ) )
  {
    return std::nullopt;
  }

  double val = obj[ key ].toDouble();
  return ( val >= 0.0 ) ? std::optional< double >( val ) : std::nullopt;
}

std::optional< std::string > readJsonString( QDBusInterface *iface, const QString &method, const QString &key )
{
  if ( !iface )
  {
    return std::nullopt;
  }

  QDBusMessage reply = iface->call( method );
  if ( reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty() )
  {
    return std::nullopt;
  }

  const QString json = reply.arguments().at( 0 ).toString();
  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( doc.isNull() || !doc.isObject() )
  {
    return std::nullopt;
  }

  QJsonObject obj = doc.object();
  if ( !obj.contains( key ) || !obj[ key ].isString() )
  {
    return std::nullopt;
  }

  return obj[ key ].toString().toStdString();
}
} // namespace

// System Monitoring implementations
std::optional< int > UccdClient::getCpuTemperature()
{
  return readFanDataValue( m_interface.get(), "GetFanDataCPU", "temp" );
}

std::optional< int > UccdClient::getGpuTemperature()
{
  if ( auto temp = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "temp" ) )
    return temp;

  return readJsonInt( m_interface.get(), "GetIGpuInfoValuesJSON", "temp" );
}

std::optional< int > UccdClient::getIGpuTemperature()
{
  return readJsonInt( m_interface.get(), "GetIGpuInfoValuesJSON", "temp" );
}

std::optional< int > UccdClient::getCpuFrequency()
{
  // Read from daemon (sysfs reading moved to HardwareMonitorWorker)
  return callMethod< int >( "GetCpuFrequencyMHz" );
}

std::optional< int > UccdClient::getGpuFrequency()
{
  if ( auto freq = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "coreFrequency" ) )
  {
    return freq;
  }
  return readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "coreFreq" );
}

std::optional< int > UccdClient::getIGpuFrequency()
{
  return readJsonInt( m_interface.get(), "GetIGpuInfoValuesJSON", "coreFrequency" );
}

std::optional< double > UccdClient::getCpuPower()
{
  return readJsonDouble( m_interface.get(), "GetCpuPowerValuesJSON", "powerDraw" );
}

std::optional< double > UccdClient::getGpuPower()
{
  if ( auto power = readJsonDouble( m_interface.get(), "GetDGpuInfoValuesJSON", "powerDraw" ) )
  {
    return power;
  }
  return readJsonDouble( m_interface.get(), "GetIGpuInfoValuesJSON", "powerDraw" );
}

std::optional< double > UccdClient::getIGpuPower()
{
  return readJsonDouble( m_interface.get(), "GetIGpuInfoValuesJSON", "powerDraw" );
}

// ---- Extended discrete GPU metrics ----

std::optional< int > UccdClient::getDGpuComputeUtilPct()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "computeUtilPct" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuMemoryUtilPct()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "memoryUtilPct" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuVramUsedMiB()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "vramUsedMiB" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuVramTotalMiB()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "vramTotalMiB" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< std::string > UccdClient::getDGpuPerfLimitReason()
{
  auto v = readJsonString( m_interface.get(), "GetDGpuInfoValuesJSON", "perfLimitReason" );
  return ( v && !v->empty() ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuEncoderUtilPct()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "encoderUtilPct" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuDecoderUtilPct()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "decoderUtilPct" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuCurrentPstate()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "currentPstate" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuGrClockOffsetMHz()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "grClockOffsetMHz" );
  return ( v && *v != -999 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuMemClockOffsetMHz()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "memClockOffsetMHz" );
  return ( v && *v != -999 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuVramFrequencyMHz()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "vramFrequency" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getDGpuCoreVoltageMv()
{
  auto v = readJsonInt( m_interface.get(), "GetDGpuInfoValuesJSON", "coreVoltageMv" );
  return ( v && *v >= 0 ) ? v : std::nullopt;
}

std::optional< int > UccdClient::getFanSpeedRPM()
{
  if ( auto percentage = readFanDataValue( m_interface.get(), "GetFanDataCPU", "speed" ) )
  {
    return ( *percentage ) * 60;
  }
  return std::nullopt;
}

std::optional< int > UccdClient::getGpuFanSpeedRPM()
{
  auto gpu1 = readFanDataValue( m_interface.get(), "GetFanDataGPU1", "speed" );
  auto gpu2 = readFanDataValue( m_interface.get(), "GetFanDataGPU2", "speed" );

  if ( gpu1 && gpu2 )
  {
    return static_cast< int >( ( *gpu1 + *gpu2 ) / 2 ) * 60;
  }
  if ( gpu1 )
  {
    return ( *gpu1 ) * 60;
  }
  if ( gpu2 )
  {
    return ( *gpu2 ) * 60;
  }
  return std::nullopt;
}

// Return raw fan speed percentage (0-100) as reported by uccd
std::optional< int > UccdClient::getFanSpeedPercent()
{
  return readFanDataValue( m_interface.get(), "GetFanDataCPU", "speed" );
}

std::optional< int > UccdClient::getGpuFanSpeedPercent()
{
  auto gpu1 = readFanDataValue( m_interface.get(), "GetFanDataGPU1", "speed" );
  auto gpu2 = readFanDataValue( m_interface.get(), "GetFanDataGPU2", "speed" );

  if ( gpu1 && gpu2 )
  {
    return static_cast< int >( ( *gpu1 + *gpu2 ) / 2 );
  }
  if ( gpu1 )
  {
    return *gpu1;
  }
  if ( gpu2 )
  {
    return *gpu2;
  }
  return std::nullopt;
}

// Water cooler control
bool UccdClient::setWaterCoolerFanSpeed( int dutyCyclePercent )
{
  return callMethod< bool, int >( "SetWaterCoolerFanSpeed", dutyCyclePercent ).value_or( false );
}

bool UccdClient::setWaterCoolerPumpVoltage( int voltageCode )
{
  return callMethod< bool, int >( "SetWaterCoolerPumpVoltage", voltageCode ).value_or( false );
}

bool UccdClient::setWaterCoolerLEDColor( int r, int g, int b, int mode )
{
  return callMethod< bool, int, int, int, int >( "SetWaterCoolerLEDColor", r, g, b, mode ).value_or( false );
}

bool UccdClient::turnOffWaterCoolerLED()
{
  return callMethod< bool >( "TurnOffWaterCoolerLED" ).value_or( false );
}

// Water cooler readings
std::optional< int > UccdClient::getWaterCoolerFanSpeed()
{
  return callMethod< int >( "GetWaterCoolerFanSpeed" );
}

std::optional< int > UccdClient::getWaterCoolerPumpLevel()
{
  return callMethod< int >( "GetWaterCoolerPumpLevel" );
}

// --- Monitoring history ---

std::optional< QByteArray > UccdClient::getMonitorDataSince( qint64 sinceTimestampMs )
{
  return callMethod< QByteArray >( "GetMonitorDataSince", static_cast< qlonglong >( sinceTimestampMs ) );
}

bool UccdClient::setMonitorHistoryHorizon( int seconds )
{
  return callVoidMethod( "SetMonitorHistoryHorizon", seconds );
}

std::optional< int > UccdClient::getMonitorHistoryHorizon()
{
  return callMethod< int >( "GetMonitorHistoryHorizon" );
}

void UccdClient::subscribeProfileChanged( [[maybe_unused]] ProfileChangedCallback callback )
{
  // Already handled via Qt signal connection
}

void UccdClient::subscribePowerStateChanged( [[maybe_unused]] PowerStateChangedCallback callback )
{
  // Already handled via Qt signal connection
}

} // namespace ucc
