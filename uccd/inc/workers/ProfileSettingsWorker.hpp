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

#include "../profiles/UccProfile.hpp"
#include "SysfsNode.hpp"
#include "../TccSettings.hpp"
#include "../NvmlWrapper.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <syslog.h>
#include <vector>

// Forward declaration
class TuxedoIOAPI;

namespace fs = std::filesystem;

/**
 * @brief TDP information structure
 */
struct TDPInfo
{
  uint32_t min;
  uint32_t max;
  uint32_t current;
  std::string descriptor;
};

/**
 * @brief ProfileSettingsWorker - Merged worker for idle profile-related subsystems
 *
 * Replaces the former ODMProfileWorker, ODMPowerLimitWorker, ChargingWorker,
 * and YCbCr420WorkaroundWorker.  None of those needed periodic onWork() activity;
 * they only did real work at start-up and on explicit reapplyProfile() calls.
 * Combining them eliminates four dedicated QThreads.
 *
 * Lives on the main thread -- no DaemonWorker / QThread inheritance.
 */
class ProfileSettingsWorker
{
public:
  ProfileSettingsWorker(
    TuxedoIOAPI &ioApi,
    std::shared_ptr< NvmlWrapper > nvml,
    std::function< UccProfile() > getActiveProfileCallback,
    std::function< void( const std::vector< std::string > & ) > setOdmProfilesAvailableCallback,
    std::function< void( const std::string & ) > setOdmPowerLimitsJSON,
    std::function< void( const std::string & ) > logFunction,
    TccSettings &settings,
    std::atomic< bool > &modeReapplyPending,
    std::atomic< int32_t > &nvidiaPowerCTRLDefaultPowerLimit,
    std::atomic< int32_t > &nvidiaPowerCTRLMaxPowerLimit,
    std::atomic< bool > &nvidiaPowerCTRLAvailable,
    std::atomic< bool > &cTGPAdjustmentSupported,
    bool skipAcpiPlatformProfile = false )
    : m_ioApi( ioApi ),
      m_nvml( std::move( nvml ) ),
      m_getActiveProfile( std::move( getActiveProfileCallback ) ),
      m_setOdmProfilesAvailable( std::move( setOdmProfilesAvailableCallback ) ),
      m_setOdmPowerLimitsJSON( std::move( setOdmPowerLimitsJSON ) ),
      m_logFunction( std::move( logFunction ) ),
      m_skipAcpiPlatformProfile( skipAcpiPlatformProfile ),
      m_settings( settings ),
      m_modeReapplyPending( modeReapplyPending ),
      m_nvidiaPowerCTRLDefaultPowerLimit( nvidiaPowerCTRLDefaultPowerLimit ),
      m_nvidiaPowerCTRLMaxPowerLimit( nvidiaPowerCTRLMaxPowerLimit ),
      m_nvidiaPowerCTRLAvailable( nvidiaPowerCTRLAvailable ),
      m_cTGPAdjustmentSupported( cTGPAdjustmentSupported )
  {
  }

  /**
   * @brief Perform all one-time initialisation formerly done by each worker's onStart().
   *
   * Call this once after construction and after all callbacks are wired up.
   */
  void start();

  // =====================================================================
  //  ODM Power Limit API  (was ODMPowerLimitWorker)
  // =====================================================================

  bool setODMPowerLimits( const std::vector< int > &values );

  void reapplyProfile()
  {
    logLine( "ProfileSettingsWorker: reapplyProfile() called" );
    applyODMPowerLimits();
    applyODMProfile();
  }

  // =====================================================================
  //  Charging API  (was ChargingWorker)
  // =====================================================================

  bool applyChargingProfile( const std::string &profileDescriptor ) noexcept;
  bool applyChargingPriority( const std::string &priorityDescriptor ) noexcept;

  // --- Charge Thresholds ---

  bool setChargeStartThreshold( int value ) noexcept;
  bool setChargeEndThreshold( int value ) noexcept;

  // --- Charge Type ---

  bool setChargeType( const std::string &type ) noexcept;

  // =====================================================================
  //  NVIDIA Power Control API  (was NVIDIAPowerCTRLListener)
  // =====================================================================

  /**
   * @brief Apply cTGP offset explicitly (GPU-profile path only).
   * @return true if successfully applied and verified.
   */
  bool applyNVIDIAPowerOffset( int32_t offset );

  /**
   * @brief Periodic validation — checks if an external process changed the cTGP offset
   *        and re-applies the profile value if needed.
   *
   * Call this from the service's onWork() loop (original interval was 5 000 ms).
   */
  void validateNVIDIACTGPOffset();

private:
  // ----- ODM Profile internals -----

  enum class ODMProfileType
  {
    None,
    TuxedoPlatformProfile,
    AcpiPlatformProfile,
    TuxedoIOAPI
  };

  TuxedoIOAPI &m_ioApi;
  std::shared_ptr< NvmlWrapper > m_nvml;
  std::function< UccProfile() > m_getActiveProfile;
  std::function< void( const std::vector< std::string > & ) > m_setOdmProfilesAvailable;
  std::function< void( const std::string & ) > m_setOdmPowerLimitsJSON;
  std::function< void( const std::string & ) > m_logFunction;
  ODMProfileType m_odmProfileType = ODMProfileType::None;
  bool m_skipAcpiPlatformProfile = false;

  // --- Sysfs path constants ---

  static inline const std::string TUXEDO_PLATFORM_PROFILE =
    "/sys/bus/platform/devices/tuxedo_platform_profile/platform_profile";
  static inline const std::string TUXEDO_PLATFORM_PROFILE_CHOICES =
    "/sys/bus/platform/devices/tuxedo_platform_profile/platform_profile_choices";
  static inline const std::string ACPI_PLATFORM_PROFILE =
    "/sys/firmware/acpi/platform_profile";
  static inline const std::string ACPI_PLATFORM_PROFILE_CHOICES =
    "/sys/firmware/acpi/platform_profile_choices";

  static inline const std::string CHARGING_PROFILE =
    "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profile";
  static inline const std::string CHARGING_PROFILES_AVAILABLE =
    "/sys/devices/platform/tuxedo_keyboard/charging_profile/charging_profiles_available";

  static inline const std::string CHARGING_PRIORITY =
    "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prio";
  static inline const std::string CHARGING_PRIORITIES_AVAILABLE =
    "/sys/devices/platform/tuxedo_keyboard/charging_priority/charging_prios_available";

  static inline const std::string NVIDIA_CTGP_OFFSET =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_offset";
  static inline const std::string NVIDIA_CTGP_ENABLE =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/ctgp_enable";
  static inline const std::string NVIDIA_DB_ENABLE =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/db_enable";
  static inline const std::string NVIDIA_TPP_OFFSET =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/tpp_offset";
  static inline const std::string NVIDIA_DB_OFFSET =
    "/sys/devices/platform/tuxedo_nvidia_power_ctrl/db_offset";
  static constexpr int32_t NVIDIA_TPP_UNLOCK_OFFSET = 255;
  static constexpr int32_t NVIDIA_DB_DYNAMIC_BOOST_OFFSET = 25;
  static inline const std::string UNIWILL_OVERBOOST_PROFILE = "overboost";

  void detectODMProfileType();
  std::vector< std::string > readPlatformProfileChoices( const std::string &path );

  bool getAvailableProfilesViaAPI( std::vector< std::string > &profiles );
  std::string getDefaultProfileViaAPI();
  bool setProfileViaAPI( const std::string &profileName );

  void applyODMProfile();
  void applyPlatformProfile(
    const std::string &profilePath, const std::string &choicesPath,
    const std::string &chosenProfileName );
  void applyProfileViaAPI( const std::string &chosenProfileName );

  // ----- ODM Power Limit internals -----

  std::vector< TDPInfo > getTDPInfo();
  bool setTDPValues( const std::vector< uint32_t > &values );
  void logLine( const std::string &message );
  void publishODMPowerLimitsJSON( const std::vector< TDPInfo > &tdpInfo );
  void applyODMPowerLimits();

  // ----- Charging internals -----

  std::string m_currentChargingProfile;
  std::string m_currentChargingPriority;

  bool hasChargingProfile() const noexcept
  {
    return SysfsNode< std::string >( CHARGING_PROFILE ).isAvailable() and
           SysfsNode< std::string >( CHARGING_PROFILES_AVAILABLE ).isAvailable();
  }

  bool hasChargingPriority() const noexcept
  {
    return SysfsNode< std::string >( CHARGING_PRIORITY ).isAvailable() and
           SysfsNode< std::string >( CHARGING_PRIORITIES_AVAILABLE ).isAvailable();
  }

  std::vector< std::string > getChargingProfilesAvailable() const noexcept
  {
    if ( not hasChargingProfile() )
      return {};

    auto profiles =
      SysfsNode< std::vector< std::string > >( CHARGING_PROFILES_AVAILABLE, " " ).read();
    return profiles.value_or( std::vector< std::string >{} );
  }

  std::vector< std::string > getChargingPrioritiesAvailable() const noexcept
  {
    if ( not hasChargingPriority() )
      return {};

    auto prios = SysfsNode< std::vector< std::string > >( CHARGING_PRIORITIES_AVAILABLE, " " ).read();
    return prios.value_or( std::vector< std::string >{} );
  }

  void initializeChargingSettings() noexcept;

  // ----- YCbCr 4:2:0 internals -----

  TccSettings &m_settings;
  std::atomic< bool > &m_modeReapplyPending;
  bool m_ycbcr420Available = false;

  // ----- NVIDIA Power Control internals -----

  int32_t m_lastAppliedNVIDIAOffset = 0;
  bool m_hasAppliedNVIDIAOffset = false;
  std::atomic< int32_t > &m_nvidiaPowerCTRLDefaultPowerLimit;
  std::atomic< int32_t > &m_nvidiaPowerCTRLMaxPowerLimit;
  std::atomic< bool > &m_nvidiaPowerCTRLAvailable;
  std::atomic< bool > &m_cTGPAdjustmentSupported;

  bool fileExists( const std::string &path ) const
  {
    std::error_code ec;
    return fs::exists( path, ec ) && fs::is_regular_file( path, ec );
  }

  void checkYCbCr420Availability();
  void applyYCbCr420Workaround();

  // ----- NVIDIA Power Control private methods -----

  void initNVIDIAPowerCTRL();
  bool applyNVIDIACTGPOffset( int32_t offset );
  bool writeNVIDIAPowerControlNodeIfAvailable( const std::string &path, int32_t value );
  bool forceNVIDIAPowerControlUnlocked( bool verbose = true );
  void queryNVIDIAPowerLimits();

  bool checkNVIDIAAvailability() const
  {
    std::error_code ec;
    return fs::exists( NVIDIA_CTGP_OFFSET, ec ) && fs::is_regular_file( NVIDIA_CTGP_OFFSET, ec );
  }


  // executeNvidiaSmi removed — replaced by NvmlWrapper direct API calls
};
