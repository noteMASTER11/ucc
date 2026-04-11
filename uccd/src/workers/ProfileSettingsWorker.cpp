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

#include "workers/ProfileSettingsWorker.hpp"
#include "PowerSupplyController.hpp"
#include <tuxedo_io_lib/tuxedo_io_api.hh>
#include <algorithm>

// =====================================================================
//  Public methods
// =====================================================================

void ProfileSettingsWorker::start()
{
  // Detect which ODM profile mechanism is available (needed by applyODMProfile())
  detectODMProfileType();

  // Hardware capabilities (TDP limits, charging, YCbCr420, NVIDIA power limits)
  // are read directly by UccDBusService::readHardwareCapabilities() before this
  // worker is created.  We only need to initialise charging internal state here
  // so that the getter methods (getCurrentChargingProfile etc.) work.
  initializeChargingSettings();
  initNVIDIAPowerCTRL();
}

std::vector< TDPInfo > ProfileSettingsWorker::getTDPInfo()
{
  std::vector< TDPInfo > tdpInfo;

  int nrTDPs = 0;

  if ( not m_ioApi.getNumberTDPs( nrTDPs ) or nrTDPs <= 0 )
    return tdpInfo;

  std::vector< std::string > descriptors;
  m_ioApi.getTDPDescriptors( descriptors );

  for ( int i = 0; i < nrTDPs; ++i )
  {
    TDPInfo info;
    info.current = 0;
    info.min = 0;
    info.max = 0;
    info.descriptor = ( i < static_cast< int >( descriptors.size() ) )
                        ? descriptors[ static_cast< size_t >( i ) ]
                        : "";

    m_ioApi.getTDPMin( i, reinterpret_cast< int & >( info.min ) );
    m_ioApi.getTDPMax( i, reinterpret_cast< int & >( info.max ) );
    m_ioApi.getTDP( i, reinterpret_cast< int & >( info.current ) );

    tdpInfo.push_back( info );
  }

  return tdpInfo;
}

bool ProfileSettingsWorker::setTDPValues( const std::vector< uint32_t > &values )
{
  bool allSuccess = true;

  for ( size_t i = 0; i < values.size(); ++i )
  {
    if ( not m_ioApi.setTDP( static_cast< int >( i ), static_cast< int >( values[ i ] ) ) )
    {
      allSuccess = false;
    }
  }

  return allSuccess;
}

bool ProfileSettingsWorker::setODMPowerLimits( const std::vector< int > &values )
{
  auto tdpInfo = getTDPInfo();
  if ( tdpInfo.empty() )
  {
    logLine( "ProfileSettingsWorker: No TDP hardware available" );
    m_setOdmPowerLimitsJSON( "[]" );
    return false;
  }

  std::vector< uint32_t > clampedValues;
  const size_t valueCount = std::min( values.size(), tdpInfo.size() );
  clampedValues.reserve( valueCount );

  for ( size_t i = 0; i < valueCount; ++i )
  {
    const int minValue = static_cast< int >( tdpInfo[ i ].min );
    const int maxValue = static_cast< int >( tdpInfo[ i ].max );
    const int clamped = std::clamp( values[ i ], minValue, maxValue );
    clampedValues.push_back( static_cast< uint32_t >( clamped ) );
  }

  if ( clampedValues.empty() )
  {
    publishODMPowerLimitsJSON( tdpInfo );
    return false;
  }

  const bool writeSuccess = setTDPValues( clampedValues );
  tdpInfo = getTDPInfo();
  publishODMPowerLimitsJSON( tdpInfo );

  if ( !writeSuccess )
    logLine( "ProfileSettingsWorker: Failed to write requested TDP values" );

  return writeSuccess;
}

bool ProfileSettingsWorker::applyChargingProfile( const std::string &profileDescriptor ) noexcept
{
  if ( not hasChargingProfile() )
    return false;

  if ( not profileDescriptor.empty() )
    m_currentChargingProfile = profileDescriptor;

  try
  {
    const std::string profileToSet = m_currentChargingProfile;
    const std::string currentProfile =
      SysfsNode< std::string >( CHARGING_PROFILE ).read().value_or( "" );
    const auto profilesAvailable = getChargingProfilesAvailable();

    if ( auto it = std::ranges::find( profilesAvailable, profileToSet );
         not profileToSet.empty() and profileToSet != currentProfile and it != profilesAvailable.end() )
    {
      if ( SysfsNode< std::string >( CHARGING_PROFILE ).write( profileToSet ) )
      {
        syslog( LOG_INFO, "Applied charging profile '%s'", profileToSet.c_str() );
        return true;
      }
    }
  }
  catch ( ... )
  {
    syslog( LOG_WARNING, "Failed applying charging profile" );
  }

  return false;
}

bool ProfileSettingsWorker::applyChargingPriority( const std::string &priorityDescriptor ) noexcept
{
  if ( not hasChargingPriority() )
    return false;

  if ( not priorityDescriptor.empty() )
    m_currentChargingPriority = priorityDescriptor;

  try
  {
    const std::string prioToSet = m_currentChargingPriority;
    const std::string currentPrio = SysfsNode< std::string >( CHARGING_PRIORITY ).read().value_or( "" );
    const auto priosAvailable = getChargingPrioritiesAvailable();

    if ( auto it = std::ranges::find( priosAvailable, prioToSet );
         not prioToSet.empty() and prioToSet != currentPrio and it != priosAvailable.end() )
    {
      if ( SysfsNode< std::string >( CHARGING_PRIORITY ).write( prioToSet ) )
      {
        syslog( LOG_INFO, "Applied charging priority '%s'", prioToSet.c_str() );
        return true;
      }
    }
  }
  catch ( ... )
  {
    syslog( LOG_WARNING, "Failed applying charging priority" );
  }

  return false;
}

bool ProfileSettingsWorker::setChargeStartThreshold( int value ) noexcept
{
  auto battery = PowerSupplyController::getFirstBattery();
  if ( not battery )
    return false;

  if ( battery->setChargeControlStartThreshold( value ) )
  {
    syslog( LOG_INFO, "Set charge start threshold to %d", value );
    return true;
  }

  syslog( LOG_WARNING, "Failed writing start threshold" );
  return false;
}

bool ProfileSettingsWorker::setChargeEndThreshold( int value ) noexcept
{
  auto battery = PowerSupplyController::getFirstBattery();
  if ( not battery )
    return false;

  if ( battery->setChargeControlEndThreshold( value ) )
  {
    syslog( LOG_INFO, "Set charge end threshold to %d", value );
    return true;
  }

  syslog( LOG_WARNING, "Failed writing end threshold" );
  return false;
}

bool ProfileSettingsWorker::setChargeType( const std::string &type ) noexcept
{
  auto battery = PowerSupplyController::getFirstBatteryWithChargeType();
  if ( not battery )
    return false;

  if ( SysfsNode< std::string >( battery->getBasePath() + "/charge_type" ).write( type ) )
  {
    syslog( LOG_INFO, "Set charge type to %s", type.c_str() );
    return true;
  }

  syslog( LOG_WARNING, "Failed writing charge type" );
  return false;
}

void ProfileSettingsWorker::validateNVIDIACTGPOffset()
{
  if ( !m_nvidiaPowerCTRLAvailable )
    return;

  if ( !m_hasAppliedNVIDIAOffset )
    return;

  std::ifstream file( NVIDIA_CTGP_OFFSET );
  if ( file.is_open() )
  {
    int32_t currentValue = 0;
    file >> currentValue;
    file.close();

    int32_t expectedOffset = m_lastAppliedNVIDIAOffset;
    forceNVIDIAPowerControlUnlocked( false );

    if ( currentValue != expectedOffset )
    {
      std::cout << "[NVIDIAPowerCTRL] External change detected (current: " << currentValue
                << ", expected: " << expectedOffset << "), re-applying profile" << std::endl;
      applyNVIDIACTGPOffset( expectedOffset );
    }
  }
}

// =====================================================================
//  Private methods — ODM Profile
// =====================================================================

void ProfileSettingsWorker::detectODMProfileType()
{
  if ( SysfsNode< std::string >( TUXEDO_PLATFORM_PROFILE ).isAvailable() and
       SysfsNode< std::string >( TUXEDO_PLATFORM_PROFILE_CHOICES ).isAvailable() )
  {
    m_odmProfileType = ODMProfileType::TuxedoPlatformProfile;
    syslog( LOG_INFO, "ProfileSettingsWorker: Using TUXEDO platform_profile" );
    return;
  }

  if ( not m_skipAcpiPlatformProfile and
       SysfsNode< std::string >( ACPI_PLATFORM_PROFILE ).isAvailable() and
       SysfsNode< std::string >( ACPI_PLATFORM_PROFILE_CHOICES ).isAvailable() )
  {
    m_odmProfileType = ODMProfileType::AcpiPlatformProfile;
    syslog( LOG_INFO, "ProfileSettingsWorker: Using ACPI platform_profile" );
    return;
  }
  else if ( m_skipAcpiPlatformProfile )
  {
    syslog( LOG_INFO, "ProfileSettingsWorker: Skipping ACPI platform_profile (device quirk)" );
  }

  std::vector< std::string > availableProfiles;
  if ( getAvailableProfilesViaAPI( availableProfiles ) and not availableProfiles.empty() )
  {
    m_odmProfileType = ODMProfileType::TuxedoIOAPI;
    syslog( LOG_INFO, "ProfileSettingsWorker: Using Tuxedo IO API" );
    return;
  }

  m_odmProfileType = ODMProfileType::None;
  syslog( LOG_INFO, "ProfileSettingsWorker: No ODM profile support available" );
}

std::vector< std::string > ProfileSettingsWorker::readPlatformProfileChoices(
  const std::string &path )
{
  std::vector< std::string > profiles;

  std::ifstream file( path );
  if ( not file.is_open() )
    return profiles;

  std::string line;
  if ( std::getline( file, line ) )
  {
    std::istringstream iss( line );
    std::string profile;
    while ( iss >> profile )
    {
      profiles.push_back( profile );
    }
  }

  return profiles;
}

bool ProfileSettingsWorker::getAvailableProfilesViaAPI( std::vector< std::string > &profiles )
{
  if ( m_ioApi.getAvailableODMPerformanceProfiles( profiles ) )
    return true;

  if ( checkNVIDIAAvailability() )
  {
    profiles = { "power_save", "enthusiast", UNIWILL_OVERBOOST_PROFILE };
    syslog( LOG_WARNING,
            "ProfileSettingsWorker: ODM profile list unavailable; forcing Uniwill profile "
            "fallback" );
    return true;
  }

  return false;
}

std::string ProfileSettingsWorker::getDefaultProfileViaAPI()
{
  std::string profileName;
  if ( m_ioApi.getDefaultODMPerformanceProfile( profileName ) )
    return profileName;

  return "";
}

bool ProfileSettingsWorker::setProfileViaAPI( const std::string &profileName )
{
  return !profileName.empty() && m_ioApi.setODMPerformanceProfile( profileName );
}

void ProfileSettingsWorker::applyODMProfile()
{
  const UccProfile profile = m_getActiveProfile();
  const std::string chosenProfileName = profile.odmProfile.name.value_or( "" );

  switch ( m_odmProfileType )
  {
    case ODMProfileType::TuxedoPlatformProfile:
      applyPlatformProfile(
        TUXEDO_PLATFORM_PROFILE, TUXEDO_PLATFORM_PROFILE_CHOICES, chosenProfileName );
      break;

    case ODMProfileType::AcpiPlatformProfile:
      applyPlatformProfile(
        ACPI_PLATFORM_PROFILE, ACPI_PLATFORM_PROFILE_CHOICES, chosenProfileName );
      break;

    case ODMProfileType::TuxedoIOAPI:
      applyProfileViaAPI( chosenProfileName );
      break;

    case ODMProfileType::None:
      m_setOdmProfilesAvailable( {} );
      break;
  }
}

void ProfileSettingsWorker::applyPlatformProfile(
  const std::string &profilePath, const std::string &choicesPath,
  const std::string &chosenProfileName )
{
  std::vector< std::string > availableProfiles = readPlatformProfileChoices( choicesPath );

  m_setOdmProfilesAvailable( availableProfiles );

  if ( chosenProfileName.empty() )
  {
    syslog( LOG_INFO, "ProfileSettingsWorker: No profile name specified in active profile" );
    return;
  }

  if ( auto it = std::ranges::find( availableProfiles, chosenProfileName );
       it == availableProfiles.end() )
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: Profile '%s' not available",
            chosenProfileName.c_str() );
    return;
  }

  SysfsNode< std::string > profileNode( profilePath );
  if ( profileNode.write( chosenProfileName ) )
  {
    syslog( LOG_INFO, "ProfileSettingsWorker: Set ODM profile to '%s'",
            chosenProfileName.c_str() );
  }
  else
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: Failed to set ODM profile to '%s'",
            chosenProfileName.c_str() );
  }
}

void ProfileSettingsWorker::applyProfileViaAPI( const std::string &chosenProfileName )
{
  std::vector< std::string > availableProfiles;

  if ( not getAvailableProfilesViaAPI( availableProfiles ) )
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: Failed to get available profiles via API" );
    m_setOdmProfilesAvailable( {} );
    return;
  }

  m_setOdmProfilesAvailable( availableProfiles );

  std::string profileToApply = chosenProfileName;

  auto it = std::ranges::find( availableProfiles, profileToApply );
  if ( it == availableProfiles.end() )
  {
    profileToApply = getDefaultProfileViaAPI();
    syslog( LOG_INFO,
            "ProfileSettingsWorker: Profile '%s' not available, using default '%s'",
            chosenProfileName.c_str(), profileToApply.c_str() );
  }

  it = std::ranges::find( availableProfiles, profileToApply );
  if ( it == availableProfiles.end() )
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: No valid profile found" );
    return;
  }

  if ( setProfileViaAPI( profileToApply ) )
  {
    syslog( LOG_INFO, "ProfileSettingsWorker: Set ODM profile to '%s'",
            profileToApply.c_str() );
  }
  else
  {
    syslog( LOG_WARNING, "ProfileSettingsWorker: Failed to apply profile '%s'",
            profileToApply.c_str() );
  }
}

// =====================================================================
//  Private methods — ODM Power Limits
// =====================================================================

void ProfileSettingsWorker::logLine( const std::string &message )
{
  if ( m_logFunction )
  {
    m_logFunction( message );
  }
  else
  {
    syslog( LOG_INFO, "%s", message.c_str() );
  }
}

void ProfileSettingsWorker::publishODMPowerLimitsJSON( const std::vector< TDPInfo > &tdpInfo )
{
  std::ostringstream jsonStream;
  jsonStream << "[";

  for ( size_t i = 0; i < tdpInfo.size(); ++i )
  {
    if ( i > 0 )
      jsonStream << ",";

    jsonStream << "{"
               << "\"current\":" << tdpInfo[ i ].current << ","
               << "\"min\":" << tdpInfo[ i ].min << ","
               << "\"max\":" << tdpInfo[ i ].max
               << "}";
  }

  jsonStream << "]";

  m_setOdmPowerLimitsJSON( jsonStream.str() );
}

void ProfileSettingsWorker::applyODMPowerLimits()
{
  logLine( "ProfileSettingsWorker: applyODMPowerLimits() called" );

  const UccProfile profile = m_getActiveProfile();
  const auto &odmPowerLimits = profile.odmPowerLimits;

  auto tdpInfo = getTDPInfo();

  if ( tdpInfo.empty() )
  {
    logLine( "ProfileSettingsWorker: No TDP hardware available" );
    m_setOdmPowerLimitsJSON( "[]" );
    return;
  }

  logLine( "ProfileSettingsWorker: Found " + std::to_string( tdpInfo.size() ) +
           " TDP descriptors" );

  std::vector< uint32_t > newTDPValues;

  if ( not odmPowerLimits.tdpValues.empty() )
  {
    for ( int val : odmPowerLimits.tdpValues )
      newTDPValues.push_back( static_cast< uint32_t >( val ) );
  }

  if ( newTDPValues.empty() )
  {
    for ( const auto &tdp : tdpInfo )
    {
      newTDPValues.push_back( tdp.max );
    }
  }

  for ( size_t i = 0; i < tdpInfo.size() and i < newTDPValues.size(); ++i )
  {
    newTDPValues[ i ] = static_cast< uint32_t >( std::clamp(
      static_cast< int >( newTDPValues[ i ] ),
      static_cast< int >( tdpInfo[ i ].min ),
      static_cast< int >( tdpInfo[ i ].max ) ) );
  }

  std::ostringstream logMessage;
  logMessage << "ProfileSettingsWorker: Set ODM TDPs [";

  for ( size_t i = 0; i < newTDPValues.size(); ++i )
  {
    if ( i > 0 )
      logMessage << ", ";

    logMessage << newTDPValues[ i ] << " W";
  }

  logMessage << "]";
  logLine( logMessage.str() );

  const bool writeSuccess = setTDPValues( newTDPValues );

  if ( writeSuccess )
  {
    tdpInfo = getTDPInfo();
  }
  else
  {
    logLine( "ProfileSettingsWorker: Failed to write TDP values" );
  }

  publishODMPowerLimitsJSON( tdpInfo );
}

// =====================================================================
//  Private methods — Charging
// =====================================================================

void ProfileSettingsWorker::initializeChargingSettings() noexcept
{
  if ( hasChargingProfile() )
  {
    if ( auto currentProfile = SysfsNode< std::string >( CHARGING_PROFILE ).read().value_or( "" );
         not currentProfile.empty() )
    {
      m_currentChargingProfile = currentProfile;
      syslog( LOG_INFO, "Initialized charging profile: %s", m_currentChargingProfile.c_str() );
    }
  }

  if ( hasChargingPriority() )
  {
    if ( auto currentPrio = SysfsNode< std::string >( CHARGING_PRIORITY ).read().value_or( "" );
         not currentPrio.empty() )
    {
      m_currentChargingPriority = currentPrio;
      syslog( LOG_INFO, "Initialized charging priority: %s", m_currentChargingPriority.c_str() );
    }
  }
}

// =====================================================================
//  Private methods — YCbCr 4:2:0
// =====================================================================

void ProfileSettingsWorker::checkYCbCr420Availability()
{
  m_ycbcr420Available = false;

  if ( m_settings.ycbcr420Workaround.empty() )
  {
    return;
  }

  for ( const auto &cardEntry : m_settings.ycbcr420Workaround )
  {
    int card = cardEntry.card;
    for ( const auto &portEntry : cardEntry.ports )
    {
      std::string port = portEntry.port;
      std::string path = "/sys/kernel/debug/dri/" + std::to_string( card ) + "/" + port +
                         "/force_yuv420_output";

      if ( fileExists( path ) )
      {
        m_ycbcr420Available = true;
        return;
      }
    }
  }
}

void ProfileSettingsWorker::applyYCbCr420Workaround()
{
  bool settings_changed = false;

  for ( const auto &cardEntry : m_settings.ycbcr420Workaround )
  {
    int card = cardEntry.card;
    for ( const auto &portEntry : cardEntry.ports )
    {
      std::string port = portEntry.port;
      bool enableYuv = portEntry.enabled;

      // Validate port to prevent path traversal (check for .. and / sequences)
      if ( port.find( ".." ) != std::string::npos || port.find( "/" ) != std::string::npos )
      {
        syslog( LOG_WARNING, "Invalid port name: %s (contains traversal sequences)", port.c_str() );
        continue;
      }

      std::string path = "/sys/kernel/debug/dri/" + std::to_string( card ) + "/" + port +
                         "/force_yuv420_output";

      if ( fileExists( path ) )
      {
        std::ifstream file( path );
        if ( file.is_open() )
        {
          char currentValue;
          file.get( currentValue );
          file.close();

          bool oldValue = ( currentValue == '1' );
          if ( oldValue != enableYuv )
          {
            std::ofstream outFile( path );
            if ( outFile.is_open() )
            {
              outFile << ( enableYuv ? "1" : "0" );
              outFile.close();
              settings_changed = true;
              std::cout << "[YCbCr420] Set " << path << " to " << ( enableYuv ? "1" : "0" )
                        << std::endl;
            }
            else
            {
              std::cerr << "[YCbCr420] Failed to write to " << path << std::endl;
            }
          }
        }
      }
    }
  }

  if ( settings_changed )
  {
    m_modeReapplyPending = true;
    std::cout << "[YCbCr420] Mode reapply pending due to YUV420 changes" << std::endl;
  }
}

// =====================================================================
//  Private methods — NVIDIA Power Control
// =====================================================================

void ProfileSettingsWorker::initNVIDIAPowerCTRL()
{
  m_nvidiaPowerCTRLAvailable = checkNVIDIAAvailability();

  if ( m_nvidiaPowerCTRLAvailable )
  {
    // Always query hardware power limits so the GUI has real values
    queryNVIDIAPowerLimits();
  }
}

bool ProfileSettingsWorker::applyNVIDIAPowerOffset( int32_t offset )
{
  if ( !m_nvidiaPowerCTRLAvailable )
    return false;

  return applyNVIDIACTGPOffset( offset );
}

bool ProfileSettingsWorker::applyNVIDIACTGPOffset( int32_t ctgpOffset )
{
  if ( !m_cTGPAdjustmentSupported )
  {
    std::cout << "[NVIDIAPowerCTRL] cTGP adjustment not supported for this device, skipping" << std::endl;
    return false;
  }

  const bool forcedBeforeWrite = forceNVIDIAPowerControlUnlocked();

  std::ofstream file( NVIDIA_CTGP_OFFSET );
  if ( !file.is_open() )
  {
    std::cerr << "[NVIDIAPowerCTRL] Failed to open " << NVIDIA_CTGP_OFFSET << " for writing"
              << std::endl;
    return false;
  }

  file << ctgpOffset;
  file.flush();

  if ( !file.good() )
  {
    std::cerr << "[NVIDIAPowerCTRL] Failed to write cTGP offset to " << NVIDIA_CTGP_OFFSET
              << " (stream error)" << std::endl;
    return false;
  }

  file.close();

  // Some EC firmware revisions only latch cTGP after the related control bits
  // are refreshed, so repeat the unlock sequence and write the target again.
  const bool forcedAfterWrite = forceNVIDIAPowerControlUnlocked();
  {
    std::ofstream retryFile( NVIDIA_CTGP_OFFSET );
    if ( retryFile.is_open() )
    {
      retryFile << ctgpOffset;
      retryFile.flush();
    }
  }

  // Verify the write by reading back
  std::ifstream verifyFile( NVIDIA_CTGP_OFFSET );
  if ( verifyFile.is_open() )
  {
    int32_t verifiedValue = -1;
    verifyFile >> verifiedValue;
    verifyFile.close();

    // The kernel module may clamp or round the offset to hardware-supported
    // steps, so the readback can legitimately differ from what we wrote.
    // Always track the value the hardware actually accepted so that the
    // periodic validator does not fight the hardware.
    m_lastAppliedNVIDIAOffset = verifiedValue;
    m_hasAppliedNVIDIAOffset = true;

    if ( verifiedValue == ctgpOffset )
    {
      std::cout << "[NVIDIAPowerCTRL] Applied cTGP offset: " << ctgpOffset << std::endl;
    }
    else
    {
      std::cout << "[NVIDIAPowerCTRL] Applied cTGP offset (rounded by hardware): wrote "
                << ctgpOffset << ", hardware accepted " << verifiedValue << std::endl;
    }
    if ( ctgpOffset != 0 && !forcedBeforeWrite && !forcedAfterWrite )
    {
      std::cout << "[NVIDIAPowerCTRL] Extra power-control nodes are unavailable; rebuild the "
                   "patched tuxedo_nb02_nvidia_power_ctrl module to force cTGP/DB enable bits"
                << std::endl;
    }
    return true;
  }

  return false;
}

bool ProfileSettingsWorker::writeNVIDIAPowerControlNodeIfAvailable(
  const std::string &path, int32_t value )
{
  if ( !fileExists( path ) )
    return false;

  std::ofstream file( path );
  if ( !file.is_open() )
  {
    std::cerr << "[NVIDIAPowerCTRL] Failed to open " << path << " for writing" << std::endl;
    return false;
  }

  file << value;
  file.flush();

  if ( !file.good() )
  {
    std::cerr << "[NVIDIAPowerCTRL] Failed to write " << value << " to " << path << std::endl;
    return false;
  }

  return true;
}

bool ProfileSettingsWorker::forceNVIDIAPowerControlUnlocked( bool verbose )
{
  // These nodes are available with the Mechrevo/TUXEDO driver patch. Older
  // modules expose only ctgp_offset, making the calls harmless no-ops.
  const bool ctgpEnabled = writeNVIDIAPowerControlNodeIfAvailable( NVIDIA_CTGP_ENABLE, 1 );
  const bool dbEnabled = writeNVIDIAPowerControlNodeIfAvailable( NVIDIA_DB_ENABLE, 1 );
  const bool tppUnlocked = writeNVIDIAPowerControlNodeIfAvailable(
    NVIDIA_TPP_OFFSET, NVIDIA_TPP_UNLOCK_OFFSET );
  const bool dbOffsetSet = writeNVIDIAPowerControlNodeIfAvailable(
    NVIDIA_DB_OFFSET, NVIDIA_DB_DYNAMIC_BOOST_OFFSET );

  if ( verbose && ( ctgpEnabled || dbEnabled || tppUnlocked || dbOffsetSet ) )
  {
    std::cout << "[NVIDIAPowerCTRL] Forced power control state:"
              << " ctgp_enable=" << ( ctgpEnabled ? "ok" : "n/a" )
              << " db_enable=" << ( dbEnabled ? "ok" : "n/a" )
              << " tpp_offset=" << ( tppUnlocked ? "ok" : "n/a" )
              << " db_offset=" << ( dbOffsetSet ? "ok" : "n/a" )
              << std::endl;
  }

  return ctgpEnabled || dbEnabled || tppUnlocked || dbOffsetSet;
}

void ProfileSettingsWorker::queryNVIDIAPowerLimits()
{
  if ( m_nvml && m_nvml->isAvailable() && m_nvml->deviceCount() > 0 )
  {
    if ( auto v = m_nvml->getPowerDefaultLimitW( 0 ) )
      m_nvidiaPowerCTRLDefaultPowerLimit = static_cast< int32_t >( *v );
    if ( auto v = m_nvml->getPowerMaxLimitW( 0 ) )
      m_nvidiaPowerCTRLMaxPowerLimit = static_cast< int32_t >( *v );
  }

  std::cout << "[NVIDIAPowerCTRL] NVIDIA GPU power limits - Default: "
            << m_nvidiaPowerCTRLDefaultPowerLimit << "W, Max: " << m_nvidiaPowerCTRLMaxPowerLimit
            << "W" << std::endl;
}
