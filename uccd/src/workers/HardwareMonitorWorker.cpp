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

#include "workers/HardwareMonitorWorker.hpp"
#include "SysfsNode.hpp"
#include "Utils.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <thread>
#include <chrono>
#include <syslog.h>

// ============================================================================
// DGpuInfo / IGpuInfo print helpers
// ============================================================================

void DGpuInfo::print() const noexcept
{
  std::cout << "DGPU Info: \n"
     << "  Temperature: " << m_temp << " °C\n"
     << "  Core Frequency: " << m_coreFrequency << " MHz\n"
     << "  Max Core Frequency: " << m_maxCoreFrequency << " MHz\n"
     << "  Power Draw: " << m_powerDraw << " W\n"
     << "  Max Power Limit: " << m_maxPowerLimit << " W\n"
     << "  Enforced Power Limit: " << m_enforcedPowerLimit << " W\n"
     << "  D0 Metrics Usage: " << ( m_d0MetricsUsage ? "Yes" : "No" ) << "\n";
}

void IGpuInfo::print() const noexcept
{
  std::cout << "IGPU Info: \n"
     << "  Vendor: " << m_vendor << "\n"
     << "  Temperature: " << m_temp << " °C\n"
     << "  Core Frequency: " << m_coreFrequency << " MHz\n"
     << "  Max Core Frequency: " << m_maxCoreFrequency << " MHz\n"
     << "  Power Draw: " << m_powerDraw << " W\n";
}

// ============================================================================
// IntelRAPLController (internal)
// ============================================================================

class IntelRAPLController
{
public:
  explicit IntelRAPLController( const std::string &basePath ) noexcept
    : m_basePath( basePath )
    , m_energyUJ( 0 )
    , m_enabled( false )
    , m_name( "unknown" )
    , m_constraint0Name( "unknown" ), m_constraint0MaxPower( -1 ), m_constraint0PowerLimit( -1 )
    , m_constraint1Name( "unknown" ), m_constraint1MaxPower( -1 ), m_constraint1PowerLimit( -1 )
    , m_constraint2Name( "unknown" ), m_constraint2MaxPower( -1 ), m_constraint2PowerLimit( -1 )
  {
  }

  [[nodiscard]] bool getIntelRAPLPowerAvailable() const noexcept
  {
    return m_enabled and m_name == "package-0" and m_energyUJ >= 0;
  }

  [[nodiscard]] bool getIntelRAPLConstraint0Available() const noexcept
  {
    return m_constraint0Name == "long_term" and m_constraint0MaxPower >= 0 and m_constraint0PowerLimit >= 0;
  }

  [[nodiscard]] bool getIntelRAPLConstraint1Available() const noexcept
  {
    return m_constraint1Name == "short_term" and m_constraint1MaxPower >= 0 and m_constraint1PowerLimit >= 0;
  }

  [[nodiscard]] bool getIntelRAPLConstraint2Available() const noexcept
  {
    return m_constraint2Name == "peak_power" and m_constraint2MaxPower >= 0 and m_constraint2PowerLimit >= 0;
  }

  [[nodiscard]] bool getIntelRAPLEnergyAvailable() const noexcept
  {
    return m_energyUJ >= 0;
  }

  [[nodiscard]] int64_t getConstraint0MaxPower() const noexcept { return m_constraint0PowerLimit; }
  [[nodiscard]] int64_t getConstraint1MaxPower() const noexcept { return m_constraint1PowerLimit; }
  [[nodiscard]] int64_t getConstraint2MaxPower() const noexcept { return m_constraint2PowerLimit; }

  [[nodiscard]] int64_t getEnergy() const noexcept
  {
    return readIntegerProperty( "energy_uj", -1 );
  }

  void setPowerPL1Limit( std::optional< int64_t > setPowerLimit = std::nullopt ) noexcept
  {
    if ( not getIntelRAPLConstraint0Available() )
      return;

    int64_t maxPower = getConstraint0MaxPower();
    int64_t powerLimit = maxPower;

    if ( setPowerLimit.has_value() )
      powerLimit = std::max( maxPower / 2, std::min( setPowerLimit.value(), maxPower ) );

    if ( writeIntegerProperty( "constraint_0_power_limit_uw", powerLimit ) )
      m_constraint0PowerLimit = powerLimit;

    writeBoolProperty( "enabled", true );
  }

  void updateFromSysfs() noexcept
  {
    m_name = readStringProperty( "name", "unknown" );
    m_enabled = readBoolProperty( "enabled", false );
    m_energyUJ = readIntegerProperty( "energy_uj", -1 );

    m_constraint0Name = readStringProperty( "constraint_0_name", "unknown" );
    m_constraint0MaxPower = readIntegerProperty( "constraint_0_max_power_uw", -1 );
    m_constraint0PowerLimit = readIntegerProperty( "constraint_0_power_limit_uw", -1 );

    m_constraint1Name = readStringProperty( "constraint_1_name", "unknown" );
    m_constraint1MaxPower = readIntegerProperty( "constraint_1_max_power_uw", -1 );
    m_constraint1PowerLimit = readIntegerProperty( "constraint_1_power_limit_uw", -1 );

    m_constraint2Name = readStringProperty( "constraint_2_name", "unknown" );
    m_constraint2MaxPower = readIntegerProperty( "constraint_2_max_power_uw", -1 );
    m_constraint2PowerLimit = readIntegerProperty( "constraint_2_power_limit_uw", -1 );
  }

  [[nodiscard]] const std::string &getBasePath() const noexcept { return m_basePath; }

private:
  [[nodiscard]] bool isPropertyAvailable( const std::string &propertyName ) const noexcept
  {
    try
    {
      std::string filePath = m_basePath + "/" + propertyName;
      return std::filesystem::exists( filePath ) and std::filesystem::is_regular_file( filePath );
    }
    catch ( ... ) { return false; }
  }

  [[nodiscard]] std::string readStringProperty( const std::string &propertyName,
                                               const std::string &defaultValue ) const noexcept
  {
    try
    {
      if ( not isPropertyAvailable( propertyName ) )
        return defaultValue;

      std::ifstream file( m_basePath + "/" + propertyName );
      if ( not file.is_open() )
        return defaultValue;

      std::string value;
      if ( std::getline( file, value ) )
      {
        while ( not value.empty() and ( value.back() == '\n' or value.back() == '\r' or
                                         value.back() == ' ' or value.back() == '\t' ) )
          value.pop_back();
        return value;
      }
      return defaultValue;
    }
    catch ( ... ) { return defaultValue; }
  }

  [[nodiscard]] int64_t readIntegerProperty( const std::string &propertyName,
                                            int64_t defaultValue ) const noexcept
  {
    try
    {
      if ( not isPropertyAvailable( propertyName ) )
        return defaultValue;

      std::ifstream file( m_basePath + "/" + propertyName );
      if ( not file.is_open() )
        return defaultValue;

      int64_t value = defaultValue;
      if ( file >> value )
        return value;
      return defaultValue;
    }
    catch ( ... ) { return defaultValue; }
  }

  [[nodiscard]] bool readBoolProperty( const std::string &propertyName,
                                      bool defaultValue ) const noexcept
  {
    int64_t value = readIntegerProperty( propertyName, defaultValue ? 1 : 0 );
    return value != 0;
  }

  bool writeIntegerProperty( const std::string &propertyName, int64_t value ) noexcept
  {
    try
    {
      std::ofstream file( m_basePath + "/" + propertyName );
      if ( not file.is_open() )
        return false;
      file << value;
      return file.good();
    }
    catch ( ... ) { return false; }
  }

  bool writeBoolProperty( const std::string &propertyName, bool value ) noexcept
  {
    return writeIntegerProperty( propertyName, value ? 1 : 0 );
  }

  std::string m_basePath;
  int64_t m_energyUJ;
  bool m_enabled;
  std::string m_name;
  std::string m_constraint0Name; int64_t m_constraint0MaxPower; int64_t m_constraint0PowerLimit;
  std::string m_constraint1Name; int64_t m_constraint1MaxPower; int64_t m_constraint1PowerLimit;
  std::string m_constraint2Name; int64_t m_constraint2MaxPower; int64_t m_constraint2PowerLimit;
};

// ============================================================================
// PowerController (internal)
// ============================================================================

class PowerController
{
public:
  explicit PowerController( IntelRAPLController &intelRAPL ) noexcept
    : m_intelRAPL( intelRAPL )
    , m_currentEnergy( 0 )
    , m_lastUpdateTime( std::chrono::system_clock::now() )
    , m_raplPowerStatus( intelRAPL.getIntelRAPLEnergyAvailable() )
  {
  }

  [[nodiscard]] double getCurrentPower() noexcept
  {
    if ( not m_raplPowerStatus )
      return -1.0;

    auto now = std::chrono::system_clock::now();
    int64_t currentEnergy = m_intelRAPL.getEnergy();
    int64_t energyIncrement = currentEnergy - m_currentEnergy;

    auto timeDiff = std::chrono::duration_cast< std::chrono::milliseconds >(
        now - m_lastUpdateTime ).count();
    double delaySec = timeDiff > 0 ? static_cast< double >( timeDiff ) / 1000.0 : -1.0;

    double powerDraw = -1.0;
    if ( delaySec > 0 and m_currentEnergy > 0 )
      powerDraw = static_cast< double >( energyIncrement ) / delaySec / 1000000.0;

    m_currentEnergy += energyIncrement;
    m_lastUpdateTime = now;

    return powerDraw;
  }

private:
  IntelRAPLController &m_intelRAPL;
  int64_t m_currentEnergy;
  std::chrono::system_clock::time_point m_lastUpdateTime;
  bool m_raplPowerStatus;
};

// ============================================================================
// GpuDeviceDetector (internal)
// ============================================================================

// GpuDeviceDetector is now defined in HardwareMonitorWorker.hpp

// ============================================================================
// HardwareMonitorWorker
// ============================================================================

HardwareMonitorWorker::HardwareMonitorWorker(
  std::shared_ptr< NvmlWrapper > nvml,
  CpuPowerCallback cpuPowerUpdateCallback,
  std::function< bool() > getSensorDataCollectionStatus,
  std::function< void( const std::string & ) > setPrimeStateCallback,
  bool isDisplayMuxDevice )
  : DaemonWorker( std::chrono::milliseconds( 800 ), false )
  , m_gpuDetector()
  , m_deviceCounts( m_gpuDetector.detectGpuDevices() )
  , m_nvml( std::move( nvml ) )
  , m_gpuDataCallback( nullptr )
  , m_hwmonIGpuRetryCount( 3 )
  , m_hwmonDGpuRetryCount( 3 )
  , m_RAPLConstraint0Status( false )
  , m_RAPLConstraint1Status( false )
  , m_RAPLConstraint2Status( false )
  , m_cpuPowerUpdateCallback( std::move( cpuPowerUpdateCallback ) )
  , m_getSensorDataCollectionStatus( std::move( getSensorDataCollectionStatus ) )
  , m_cpuFrequencyCallback( nullptr )
  , m_cpuTemperatureCallback( nullptr )
  , m_setPrimeState( std::move( setPrimeStateCallback ) )
  , m_primeSupported( false )
  , m_isDisplayMuxDevice( isDisplayMuxDevice )
  , m_displayConnectedToNvidia( false )
  , m_cycleCounter( 0 )
{
}

HardwareMonitorWorker::~HardwareMonitorWorker() = default;

void HardwareMonitorWorker::setGpuDataCallback( GpuDataCallback callback ) noexcept
{
  m_gpuDataCallback = std::move( callback );
}

void HardwareMonitorWorker::setWebcamCallbacks( WebcamHwReader reader, WebcamStatusCallback callback ) noexcept
{
  m_webcamHwReader = std::move( reader );
  m_webcamStatusCallback = std::move( callback );
}

void HardwareMonitorWorker::setCpuFrequencyCallback( CpuFrequencyCallback callback ) noexcept
{
  m_cpuFrequencyCallback = std::move( callback );
}

void HardwareMonitorWorker::setCpuTemperatureCallback( CpuTemperatureCallback callback ) noexcept
{
  m_cpuTemperatureCallback = std::move( callback );
}

bool HardwareMonitorWorker::isPrimeSupported() const noexcept
{
  return m_primeSupported;
}

// ------------ Lifecycle ------------

void HardwareMonitorWorker::onStart()
{
  initGpu();
  initCpuPower();
  initPrime();

  // initial webcam read so DBus data is populated before first poll cycle
  updateWebcamStatus();
}

void HardwareMonitorWorker::onWork()
{
  m_cycleCounter++;

  // --- GPU info: every cycle (800ms) ---
  // retry AMD iGPU path discovery if not found yet
  if ( m_deviceCounts.amdIGpuCount == 1 and not m_amdIGpuHwmonPath.has_value() and m_hwmonIGpuRetryCount > 0 )
    ( void ) checkAmdIGpuHwmonPath();
  if ( m_deviceCounts.amdDGpuCount == 1 and not m_amdDGpuHwmonPath.has_value() and m_hwmonDGpuRetryCount > 0 )
    ( void ) checkAmdDGpuHwmonPath();

  try
  {
    if ( m_gpuDataCallback )
      m_gpuDataCallback( getIGpuValues(), getDGpuValues() );
  }
  catch ( ... ) { /* ignore callback exceptions */ }

  // --- CPU frequency: every cycle (≈ 800ms) ---
  updateCpuFrequency();

  // --- CPU temperature: every cycle (≈ 800ms) ---
  updateCpuTemperature();

  // --- CPU power: every 3rd cycle (≈ 2400ms, close to original 2000ms) ---
  if ( m_cycleCounter % 3 == 0 )
    updateCpuPower();

  // --- Webcam: every 3rd cycle, offset by 1 (≈ 2400ms) ---
  if ( m_cycleCounter % 3 == 1 )
    updateWebcamStatus();

  // --- Prime state: every 12th cycle (≈ 9600ms, close to original 10000ms) ---
  if ( m_cycleCounter % 12 == 0 )
    updatePrimeStatus();
}

void HardwareMonitorWorker::onExit()
{
  m_cpuPowerController.reset();
  m_intelRAPLCpu.reset();
  m_intelGpuPowerController.reset();
  m_intelRAPLGpu.reset();
}

// ============================================================================
// GPU implementation
// ============================================================================

void HardwareMonitorWorker::initGpu()
{
  // Check Intel iGPU availability
  if ( m_deviceCounts.intelIGpuCount == 1 and not m_intelIGpuDrmPath.has_value() )
  {
    if ( std::string intelPath = getIntelIGpuDrmPathImpl(); not intelPath.empty() )
    {
      m_intelIGpuDrmPath = intelPath;
      m_intelRAPLGpu = std::make_unique< IntelRAPLController >(
        "/sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/intel-rapl:0:1/" );
      m_intelGpuPowerController = std::make_unique< PowerController >( *m_intelRAPLGpu );
    }
  }

  // Check AMD iGPU availability
  if ( m_deviceCounts.amdIGpuCount == 1 and m_hwmonIGpuRetryCount > 0 )
    ( void ) checkAmdIGpuHwmonPath();

  // Check AMD dGPU availability
  if ( m_deviceCounts.amdDGpuCount == 1 and m_hwmonDGpuRetryCount > 0 and not m_amdDGpuHwmonPath.has_value() )
    ( void ) checkAmdDGpuHwmonPath();
}

bool HardwareMonitorWorker::checkAmdIGpuHwmonPath() noexcept
{
  if ( m_amdIGpuHwmonPath.has_value() )
    return true;

  if ( m_hwmonIGpuRetryCount > 0 )
  {
    m_hwmonIGpuRetryCount--;
    if ( std::string path = getAmdIGpuHwmonPathImpl(); not path.empty() )
    {
      m_amdIGpuHwmonPath = path;
      return true;
    }
  }
  return false;
}

bool HardwareMonitorWorker::checkAmdDGpuHwmonPath() noexcept
{
  if ( m_amdDGpuHwmonPath.has_value() )
    return true;

  if ( m_hwmonDGpuRetryCount > 0 )
  {
    m_hwmonDGpuRetryCount--;
    if ( std::string path = getAmdDGpuHwmonPathImpl(); not path.empty() )
    {
      m_amdDGpuHwmonPath = path;
      return true;
    }
  }
  return false;
}

std::string HardwareMonitorWorker::getIntelIGpuDrmPathImpl() const noexcept
{
  namespace fs = std::filesystem;
  try
  {
    const std::string intelIGpuPattern = GpuDeviceDetector().getIntelIGpuPatternPublic();
    const std::regex idRegex( "PCI_ID=" + intelIGpuPattern );

    for ( const auto &pciDev : fs::directory_iterator( "/sys/bus/pci/devices" ) )
    {
      const auto drmDir = pciDev.path() / "drm";
      std::error_code ec;
      if ( not fs::is_directory( drmDir, ec ) )
        continue;

      for ( const auto &card : fs::directory_iterator( drmDir ) )
      {
        auto ueventPath = card.path() / "device" / "uevent";
        std::ifstream ueventFile( ueventPath );
        if ( not ueventFile )
          continue;

        std::string line;
        while ( std::getline( ueventFile, line ) )
        {
          if ( std::regex_search( line, idRegex ) )
            return card.path().string();
        }
      }
    }
    return "";
  }
  catch ( ... ) { return ""; }
}

std::string HardwareMonitorWorker::getAmdIGpuHwmonPathImpl() const noexcept
{
  namespace fs = std::filesystem;
  try
  {
    const std::string amdIGpuPattern = "1002:(164E|1506|15DD|15D8|15E7|1636|1638|164C|164D|1681|15BF|"
                                 "15C8|1304|1305|1306|1307|1309|130A|130B|130C|130D|130E|130F|"
                                 "1310|1311|1312|1313|1315|1316|1317|1318|131B|131C|131D|13C0|"
                                 "9830|9831|9832|9833|9834|9835|9836|9837|9838|9839|983a|983b|983c|"
                                 "983d|983e|983f|9850|9851|9852|9853|9854|9855|9856|9857|9858|"
                                 "9859|985A|985B|985C|985D|985E|985F|9870|9874|9875|9876|9877|"
                                 "98E4|13FE|143F|74A0|1435|163f|1900|1901|1114|150E)";
    const std::regex idRegex( "PCI_ID=" + amdIGpuPattern );

    for ( const auto &hwmonEntry : fs::directory_iterator( "/sys/class/hwmon" ) )
    {
      auto ueventPath = hwmonEntry.path() / "device" / "uevent";
      std::ifstream ueventFile( ueventPath );
      if ( not ueventFile )
        continue;

      bool hasAmdGpu = false;
      bool hasMatchingId = false;
      std::string line;
      while ( std::getline( ueventFile, line ) )
      {
        if ( line == "DRIVER=amdgpu" )
          hasAmdGpu = true;
        if ( std::regex_search( line, idRegex ) )
          hasMatchingId = true;
        if ( hasAmdGpu and hasMatchingId )
          return hwmonEntry.path().string();
      }
    }
    return "";
  }
  catch ( ... ) { return ""; }
}

std::string HardwareMonitorWorker::getAmdDGpuHwmonPathImpl() const noexcept
{
  return "";
}

IGpuInfo HardwareMonitorWorker::getIGpuValues() noexcept
{
  IGpuInfo values{};

  if ( m_intelIGpuDrmPath.has_value() )
    values = getIntelIGpuValues( values );
  else if ( m_amdIGpuHwmonPath.has_value() )
    values = getAmdIGpuValues( values );

  return values;
}

IGpuInfo HardwareMonitorWorker::getIntelIGpuValues( const IGpuInfo &base ) const noexcept
{
  IGpuInfo values = base;

  if ( not m_intelIGpuDrmPath.has_value() )
    return values;

  values.m_vendor = "intel";
  const std::string &drmPath = m_intelIGpuDrmPath.value();

  // Current GPU frequency (MHz) — directly readable from DRM sysfs
  if ( auto curFreq = SysfsNode< int64_t >( drmPath + "/gt_act_freq_mhz" ).read(); curFreq and *curFreq > 0 )
    values.m_coreFrequency = static_cast< double >( *curFreq );

  // Max GPU frequency (MHz)
  if ( auto maxFreq = SysfsNode< int64_t >( drmPath + "/gt_RP0_freq_mhz" ).read(); maxFreq and *maxFreq > 0 )
    values.m_maxCoreFrequency = static_cast< double >( *maxFreq );

  // Power draw via Intel RAPL
  if ( m_intelGpuPowerController )
    values.m_powerDraw = m_intelGpuPowerController->getCurrentPower();

  return values;
}

IGpuInfo HardwareMonitorWorker::getAmdIGpuValues( const IGpuInfo &base ) const noexcept
{
  IGpuInfo values = base;

  if ( not m_amdIGpuHwmonPath.has_value() )
    return values;

  std::string hwmonPath = m_amdIGpuHwmonPath.value();
  values.m_vendor = "amd";

  if ( int64_t tempValue = SysfsNode< int64_t >( hwmonPath + "/temp1_input" ).read().value_or( -1 ); tempValue >= 0 )
    values.m_temp = static_cast< double >( tempValue ) / 1000.0;

  if ( int64_t curFreqValue = SysfsNode< int64_t >( hwmonPath + "/freq1_input" ).read().value_or( -1 ); curFreqValue >= 0 )
    values.m_coreFrequency = static_cast< double >( curFreqValue ) / 1000000.0;

  if ( std::string maxFreqString = SysfsNode< std::string >( hwmonPath + "/device/pp_dpm_sclk" ).read().value_or( "" ); not maxFreqString.empty() )
    values.m_maxCoreFrequency = parseMaxAmdFreq( maxFreqString );

  if ( int64_t powerValue = SysfsNode< int64_t >( hwmonPath + "/power1_input" ).read().value_or( -1 ); powerValue >= 0 )
    values.m_powerDraw = static_cast< double >( powerValue ) / 1000.0;

  return values;
}

DGpuInfo HardwareMonitorWorker::getDGpuValues() noexcept
{
  DGpuInfo values{};
  bool metricsUsage = true;

  if ( m_deviceCounts.nvidiaCount == 1 and m_nvml->isAvailable() and metricsUsage )
  {
    values = getNvidiaDGpuValues();
  }
  else if ( m_deviceCounts.amdDGpuCount == 1 and metricsUsage )
  {
    if ( m_amdDGpuHwmonPath.has_value() or checkAmdDGpuHwmonPath() )
      values = getAmdDGpuValues( values );
  }

  values.m_d0MetricsUsage = metricsUsage;
  return values;
}

DGpuInfo HardwareMonitorWorker::getNvidiaDGpuValues() const noexcept
{
  if ( !m_nvml->isAvailable() )
    return DGpuInfo{};

  DGpuInfo values{};

  if ( auto v = m_nvml->getTemperatureDegC( 0 ) )
    values.m_temp = static_cast< double >( *v );

  if ( auto v = m_nvml->getPowerDrawW( 0 ) )
    values.m_powerDraw = *v;

  if ( auto v = m_nvml->getPowerMaxLimitW( 0 ) )
    values.m_maxPowerLimit = *v;

  if ( auto v = m_nvml->getEnforcedPowerLimitW( 0 ) )
    values.m_enforcedPowerLimit = *v;

  if ( auto v = m_nvml->getGpuClockMHz( 0 ) )
    values.m_coreFrequency = static_cast< double >( *v );

  if ( auto v = m_nvml->getMemClockMHz( 0 ) )
    values.m_vramFrequency = static_cast< double >( *v );

  if ( auto v = m_nvml->getMaxGpuClockMHz( 0 ) )
    values.m_maxCoreFrequency = static_cast< double >( *v );

  if ( auto v = m_nvml->getComputeUtilPct( 0 ) )
    values.m_computeUtilPct = static_cast< int >( *v );

  if ( auto v = m_nvml->getMemoryUtilPct( 0 ) )
    values.m_memoryUtilPct = static_cast< int >( *v );

  if ( auto v = m_nvml->getVramUsedMiB( 0 ) )
    values.m_vramUsedMiB = static_cast< int >( *v );

  if ( auto v = m_nvml->getVramTotalMiB( 0 ) )
    values.m_vramTotalMiB = static_cast< int >( *v );

  if ( auto v = m_nvml->getPerfLimitReason( 0 ) )
    values.m_perfLimitReason = *v;

  if ( auto v = m_nvml->getEncoderUtilPct( 0 ) )
    values.m_encoderUtilPct = static_cast< int >( *v );

  if ( auto v = m_nvml->getDecoderUtilPct( 0 ) )
    values.m_decoderUtilPct = static_cast< int >( *v );

  if ( auto v = m_nvml->getCurrentPstate( 0 ) )
    values.m_currentPstate = static_cast< int >( *v );

  if ( auto v = m_nvml->getGrClockOffsetMHz( 0 ) )
    values.m_grClockOffsetMHz = *v;

  if ( auto v = m_nvml->getMemClockOffsetMHz( 0 ) )
    values.m_memClockOffsetMHz = *v;

  if ( auto v = m_nvml->getCoreVoltageMv( 0 ) )
    values.m_coreVoltageMv = static_cast< int >( *v );

  return values;
}

DGpuInfo HardwareMonitorWorker::getAmdDGpuValues( const DGpuInfo &base ) const noexcept
{
  DGpuInfo values = base;

  if ( not m_amdDGpuHwmonPath.has_value() )
    return values;

  try
  {
    const std::string &basePath = m_amdDGpuHwmonPath.value();

    int64_t tempMilli = SysfsNode< int64_t >( basePath + "/temp1_input" ).read().value_or( -1000 );
    if ( tempMilli > -1000 )
      values.m_temp = static_cast< double >( tempMilli ) / 1000.0;

    int64_t freqHz = SysfsNode< int64_t >( basePath + "/freq1_input" ).read().value_or( -1 );
    if ( freqHz > 0 )
      values.m_coreFrequency = static_cast< double >( freqHz ) / 1000000.0;

    int64_t powerMicro = SysfsNode< int64_t >( basePath + "/power1_average" ).read().value_or( -1 );
    if ( powerMicro > 0 )
      values.m_powerDraw = static_cast< double >( powerMicro ) / 1000000.0;
  }
  catch ( ... ) { }

  return values;
}

double HardwareMonitorWorker::parseMaxAmdFreq( const std::string &frequencyString ) const noexcept
{
  std::regex mhzRegex( R"(\d+Mhz)" );
  std::smatch match;
  double maxFreq = -1.0;

  std::string::const_iterator searchStart( frequencyString.cbegin() );
  while ( std::regex_search( searchStart, frequencyString.cend(), match, mhzRegex ) )
  {
    try
    {
      std::string numStr = match.str();
      numStr = numStr.substr( 0, numStr.length() - 3 );
      double freq = std::stod( numStr );
      if ( freq > maxFreq )
        maxFreq = freq;
    }
    catch ( ... ) { }
    searchStart = match.suffix().first;
  }
  return maxFreq;
}

// ============================================================================
// CPU power implementation
// ============================================================================

void HardwareMonitorWorker::initCpuPower()
{
  m_intelRAPLCpu = std::make_unique< IntelRAPLController >(
    "/sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/" );
  m_intelRAPLCpu->updateFromSysfs();
  m_cpuPowerController = std::make_unique< PowerController >( *m_intelRAPLCpu );

  m_RAPLConstraint0Status = m_intelRAPLCpu->getIntelRAPLConstraint0Available();
  m_RAPLConstraint1Status = m_intelRAPLCpu->getIntelRAPLConstraint1Available();
  m_RAPLConstraint2Status = m_intelRAPLCpu->getIntelRAPLConstraint2Available();

  // Run first update immediately
  updateCpuPower();
}

void HardwareMonitorWorker::updateCpuPower()
{
  std::ostringstream jsonStream;
  jsonStream << "{";
  double rawPower = -1.0;

  if ( m_getSensorDataCollectionStatus() )
  {
    rawPower = getCpuCurrentPower();
    jsonStream << "\"powerDraw\":" << rawPower;

    double maxPowerLimit = getCpuMaxPowerLimit();
    if ( maxPowerLimit > 0 )
      jsonStream << ",\"maxPowerLimit\":" << maxPowerLimit;
  }
  else
  {
    jsonStream << "\"powerDraw\":-1";
  }

  jsonStream << "}";
  m_cpuPowerUpdateCallback( jsonStream.str(), rawPower );
}

double HardwareMonitorWorker::getCpuCurrentPower()
{
  if ( not m_cpuPowerController )
    return -1.0;
  return m_cpuPowerController->getCurrentPower();
}

double HardwareMonitorWorker::getCpuMaxPowerLimit()
{
  if ( not m_RAPLConstraint0Status and not m_RAPLConstraint1Status and not m_RAPLConstraint2Status )
    return -1.0;

  double maxPowerLimit = -1.0;

  if ( m_RAPLConstraint0Status )
  {
    int64_t c0 = m_intelRAPLCpu->getConstraint0MaxPower();
    if ( c0 > 0 )
      maxPowerLimit = static_cast< double >( c0 );
  }

  if ( m_RAPLConstraint1Status )
  {
    int64_t c1 = m_intelRAPLCpu->getConstraint1MaxPower();
    if ( c1 > 0 and static_cast< double >( c1 ) > maxPowerLimit )
      maxPowerLimit = static_cast< double >( c1 );
  }

  if ( m_RAPLConstraint2Status )
  {
    int64_t c2 = m_intelRAPLCpu->getConstraint2MaxPower();
    if ( c2 > 0 and static_cast< double >( c2 ) > maxPowerLimit )
      maxPowerLimit = static_cast< double >( c2 );
  }

  // Convert from micro watts to watts
  return maxPowerLimit / 1000000.0;
}

// ============================================================================
// Prime implementation
// ============================================================================

void HardwareMonitorWorker::initPrime()
{
  // Wait for requires_offloading file to be updated after boot
  std::this_thread::sleep_for( std::chrono::milliseconds( 2000 ) );

  if ( m_isDisplayMuxDevice )
  {
    m_displayConnectedToNvidia = getDisplayConnectedToNvidia();
    syslog( LOG_INFO, "HardwareMonitorWorker: Display mux device — display connected to NVIDIA: %s",
            m_displayConnectedToNvidia ? "yes" : "no" );
  }

  updatePrimeStatus();
}

void HardwareMonitorWorker::updatePrimeStatus() noexcept
{
  const bool primeSupported = checkPrimeSupported();

  if ( primeSupported )
  {
    m_setPrimeState( checkPrimeStatus() );
    m_primeSupported = primeSupported;
  }
  else
  {
    m_setPrimeState( "-1" );
    m_primeSupported = false;
  }
}

bool HardwareMonitorWorker::checkPrimeSupported() const noexcept
{
  // On display-mux devices (e.g. IBM15A10), prime-select is not supported
  // when the eDP display is physically wired to the NVIDIA GPU.
  if ( m_isDisplayMuxDevice )
    return not m_displayConnectedToNvidia;

  const bool offloadingStatus = std::filesystem::exists(
    "/var/lib/ubuntu-drivers-common/requires_offloading" );

  if ( not offloadingStatus )
    return false;

  try
  {
    // Use executeProcess() instead of popen("which prime-select") for safety
    std::string result = ucc::executeProcess( "which", { "prime-select" } );

    while ( not result.empty() and ( result.back() == '\n' or result.back() == ' ' ) )
      result.pop_back();

    return not result.empty();
  }
  catch ( ... ) { return false; }
}

std::string HardwareMonitorWorker::checkPrimeStatus() const noexcept
{
  try
  {
    // Use executeProcess() instead of popen("prime-select query") for safety
    std::string result = ucc::executeProcess( "prime-select", { "query" } );

    while ( not result.empty() and ( result.back() == '\n' or result.back() == ' ' or result.back() == '\r' ) )
      result.pop_back();

    return transformPrimeStatus( result );
  }
  catch ( ... ) { return "off"; }
}

// ============================================================================
// Webcam implementation
// ============================================================================

void HardwareMonitorWorker::updateWebcamStatus() noexcept
{
  if ( not m_webcamHwReader or not m_webcamStatusCallback )
    return;

  try
  {
    auto [available, status] = m_webcamHwReader();
    m_webcamStatusCallback( available, status );
  }
  catch ( ... ) { /* ignore */ }
}

std::string HardwareMonitorWorker::transformPrimeStatus( const std::string &status ) const noexcept
{
  if ( status == "nvidia" )
    return "dGPU";
  else if ( status == "intel" )
    return "iGPU";
  else if ( status == "on-demand" )
    return "on-demand";
  else
    return "off";
}

bool HardwareMonitorWorker::getDisplayConnectedToNvidia() const noexcept
{
  namespace fs = std::filesystem;

  try
  {
    const std::string drmPath = "/sys/class/drm/";
    std::error_code ec;

    if ( not fs::exists( drmPath, ec ) )
      return false;

    for ( const auto &entry : fs::directory_iterator( drmPath, ec ) )
    {
      const std::string name = entry.path().filename().string();

      // Look for eDP connector entries (e.g. card0-eDP-1, card1-eDP-2)
      if ( name.find( "eDP" ) == std::string::npos )
        continue;

      const std::string vendorPath = entry.path().string() + "/device/device/vendor";
      const std::string statusPath = entry.path().string() + "/status";

      std::ifstream vendorFile( vendorPath );
      std::ifstream statusFile( statusPath );

      if ( not vendorFile.is_open() or not statusFile.is_open() )
        continue;

      std::string vendorId, status;
      std::getline( vendorFile, vendorId );
      std::getline( statusFile, status );

      // Trim whitespace
      while ( not vendorId.empty() and ( vendorId.back() == '\n' or vendorId.back() == ' ' ) )
        vendorId.pop_back();
      while ( not status.empty() and ( status.back() == '\n' or status.back() == ' ' ) )
        status.pop_back();

      // 0x10de = NVIDIA vendor ID
      if ( vendorId == "0x10de" and status == "connected" )
      {
        syslog( LOG_INFO, "HardwareMonitorWorker: eDP display '%s' connected to NVIDIA GPU",
                name.c_str() );
        return true;
      }
    }
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_WARNING, "HardwareMonitorWorker: getDisplayConnectedToNvidia failed: %s", e.what() );
  }

  return false;
}

// ============================================================================
// CPU frequency
// ============================================================================

void HardwareMonitorWorker::updateCpuFrequency() noexcept
{
  if ( !m_cpuFrequencyCallback )
    return;

  try
  {
    std::ifstream ifs( "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq" );
    if ( ifs.is_open() )
    {
      int freqKHz = -1;
      ifs >> freqKHz;
      if ( freqKHz > 0 )
      {
        m_cpuFrequencyCallback( freqKHz / 1000 );
        return;
      }
    }
  }
  catch ( ... ) { /* ignore */ }

  m_cpuFrequencyCallback( -1 );
}

void HardwareMonitorWorker::updateCpuTemperature() noexcept
{
  if ( !m_cpuTemperatureCallback )
    return;

  m_cpuTemperatureCallback( readCoretempPackageTemperature() );
}

int HardwareMonitorWorker::readCoretempPackageTemperature() const noexcept
{
  namespace fs = std::filesystem;

  try
  {
    for ( const auto &entry : fs::directory_iterator( "/sys/class/hwmon" ) )
    {
      const auto hwmonPath = entry.path();
      std::ifstream nameFile( hwmonPath / "name" );
      std::string name;
      if ( !std::getline( nameFile, name ) || name != "coretemp" )
        continue;

      for ( const auto &sensorEntry : fs::directory_iterator( hwmonPath ) )
      {
        const std::string filename = sensorEntry.path().filename().string();
        if ( filename.rfind( "temp", 0 ) != 0 ||
             filename.find( "_label" ) == std::string::npos )
          continue;

        std::ifstream labelFile( sensorEntry.path() );
        std::string label;
        if ( !std::getline( labelFile, label ) || label != "Package id 0" )
          continue;

        std::string inputFilename = filename;
        const size_t labelPos = inputFilename.find( "_label" );
        inputFilename.replace( labelPos, std::string( "_label" ).length(), "_input" );

        std::ifstream inputFile( hwmonPath / inputFilename );
        int millidegrees = -1;
        if ( inputFile >> millidegrees )
          return ( millidegrees + 500 ) / 1000;
      }

      std::ifstream fallbackInput( hwmonPath / "temp1_input" );
      int millidegrees = -1;
      if ( fallbackInput >> millidegrees )
        return ( millidegrees + 500 ) / 1000;
    }
  }
  catch ( ... ) { }

  return -1;
}
