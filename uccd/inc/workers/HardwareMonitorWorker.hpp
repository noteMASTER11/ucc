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

#include "DaemonWorker.hpp"
#include "../NvmlWrapper.hpp"
#include <climits>
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <set>
#include <fstream>
#include <filesystem>
#include <regex>

/**
 * @brief GPU device counts by vendor/type
 */
struct GpuDeviceCounts
{
  int intelIGpuCount = 0;
  int amdIGpuCount = 0;
  int amdDGpuCount = 0;
  int nvidiaCount = 0;
};

/**
 * @brief Data structure for discrete GPU information
 */
struct DGpuInfo
{
  double m_temp = -1.0;
  double m_coreFrequency = -1.0;
  double m_vramFrequency = -1.0;
  double m_maxCoreFrequency = -1.0;
  double m_powerDraw = -1.0;
  double m_maxPowerLimit = -1.0;
  double m_enforcedPowerLimit = -1.0;
  bool m_d0MetricsUsage = false;

  // Extended metrics (NVIDIA only, -1 / INT_MIN when unavailable)
  int m_computeUtilPct = -1;     ///< GPU compute utilization in % (0–100), or -1
  int m_memoryUtilPct  = -1;     ///< GPU memory-controller utilization in % (0–100), or -1
  int m_vramUsedMiB    = -1;     ///< Used VRAM in MiB, or -1
  int m_vramTotalMiB   = -1;     ///< Total VRAM in MiB, or -1
  std::string m_perfLimitReason; ///< Current perf-cap/throttle reason, empty when unavailable
  int m_encoderUtilPct = -1;     ///< NVENC utilization in %, or -1
  int m_decoderUtilPct = -1;     ///< NVDEC utilization in %, or -1
  int m_currentPstate  = -1;     ///< Current P-state index (0–15), or -1 if unknown
  int m_grClockOffsetMHz  = INT_MIN; ///< Graphics-clock offset at current P-state, INT_MIN = unavailable
  int m_memClockOffsetMHz = INT_MIN; ///< Memory-clock offset at current P-state, INT_MIN = unavailable
  int m_coreVoltageMv = -1;      ///< Core voltage in mV, or -1

  void print() const noexcept;
};

/**
 * @brief Data structure for integrated GPU information
 */
struct IGpuInfo
{
  double m_temp = -1.0;
  double m_coreFrequency = -1.0;
  double m_maxCoreFrequency = -1.0;
  double m_powerDraw = -1.0;
  std::string m_vendor = "unknown";

  void print() const noexcept;
};

// Forward declarations for internal implementation classes
class IntelRAPLController;
class PowerController;

/**
 * @brief Detects GPU devices by scanning PCI sysfs entries
 */
class GpuDeviceDetector
{
public:
  explicit GpuDeviceDetector() noexcept = default;

  [[nodiscard]] GpuDeviceCounts detectGpuDevices() const noexcept
  {
    GpuDeviceCounts counts;
    counts.intelIGpuCount = countDevicesMatchingPattern( getIntelIGpuPattern() );
    counts.amdIGpuCount = countDevicesMatchingPattern( getAmdIGpuPattern() );
    counts.amdDGpuCount = countDevicesMatchingPattern( getAmdDGpuPattern() );
    counts.nvidiaCount = countNvidiaDevices();
    return counts;
  }

  /// Expose the Intel iGPU PCI ID regex pattern for DRM path discovery.
  [[nodiscard]] std::string getIntelIGpuPatternPublic() const noexcept { return getIntelIGpuPattern(); }

private:
  [[nodiscard]] std::string getIntelIGpuPattern() const noexcept
  {
    // Comprehensive Intel iGPU device ID list from:
    // https://dgpu-docs.intel.com/devices/hardware-table.html
    // Covers Xe2, Xe-LPG, Xe, Gen11/9/8/7/6/5 GPUs (388 device IDs)
    return "8086:(6420|64B0|7D51|7D67|7D41|7DD5|7D45|7D40|"
           "A780|A781|A788|A789|A78A|A782|A78B|A783|A7A0|A7A1|A7A8|A7AA|"
           "A7AB|A7AC|A7AD|A7A9|A721|A720|4680|4690|4688|468A|468B|4682|"
           "4692|4693|46D3|46D4|46D0|46D1|46D2|4626|4628|462A|46A2|46B3|"
           "46C2|46A3|46B2|46C3|46A0|46B0|46C0|46A6|46AA|46A8|46A1|46B1|"
           "46C1|4C8A|4C8B|4C90|4C9A|4C8C|4C80|4E71|4E61|4E57|4E55|4E51|"
           "4557|4555|4571|4551|4541|9A59|9A78|9A60|9A70|9A68|9A40|9A49|"
           "9AC0|9AC9|9AD9|9AF8|8A70|8A71|8A56|8A58|8A5B|8A5D|8A54|8A5A|"
           "8A5C|8A57|8A59|8A50|8A51|8A52|8A53|3EA5|3EA8|3EA6|3EA7|3EA2|"
           "3E90|3E93|3E99|3E9C|3EA1|9BA5|9BA8|3EA4|9B21|9BA0|9BA2|9BA4|"
           "9BAA|9BAB|9BAC|87CA|3EA3|9B41|9BC0|9BC2|9BC4|9BCA|9BCB|9BCC|"
           "3E91|3E92|3E98|3E9B|9BC5|9BC8|3E96|3E9A|3E94|9BC6|9BE6|9BF6|"
           "3EA9|3EA0|593B|5923|5926|5927|5917|5912|591B|5916|5921|591A|"
           "591D|591E|591C|87C0|5913|5915|5902|5906|590B|590A|5908|590E|"
           "3185|3184|1A85|5A85|0A84|1A84|5A84|192A|1932|193B|193A|193D|"
           "1923|1926|1927|192B|192D|1912|191B|1913|1915|1917|191A|1916|"
           "1921|191D|191E|1902|1906|190B|190A|190E|163D|163A|1632|163E|"
           "163B|1636|1622|1626|162A|162B|162D|162E|1612|1616|161A|161B|"
           "161D|161E|1602|1606|160A|160B|160D|160E|22B0|22B2|22B3|22B1|"
           "0F30|0F31|0F32|0F33|0157|0155|0422|0426|042A|042B|042E|0C22|"
           "0C26|0C2A|0C2B|0C2E|0A22|0A2A|0A2B|0D2A|0D2B|0D2E|0A26|0A2E|"
           "0D22|0D26|0412|0416|0D12|041A|041B|0C12|0C16|0C1A|0C1B|0C1E|"
           "0A12|0A1A|0A1B|0D16|0D1A|0D1B|0D1E|041E|0A16|0A1E|0402|0406|"
           "040A|040B|040E|0C02|0C06|0C0A|0C0B|0C0E|0A02|0A06|0A0A|0A0B|"
           "0A0E|0D02|0D06|0D0A|0D0B|0D0E|0162|0166|016A|0152|0156|015A|"
           "0112|0122|0116|0126|0102|0106|010A|0042|0046)";
  }

  [[nodiscard]] std::string getAmdIGpuPattern() const noexcept
  {
    return "1002:(164E|1506|15DD|15D8|15E7|1636|1638|164C|164D|1681|15BF|"
           "15C8|1304|1305|1306|1307|1309|130A|130B|130C|130D|130E|130F|"
           "1310|1311|1312|1313|1315|1316|1317|1318|131B|131C|131D|13C0|"
           "9830|9831|9832|9833|9834|9835|9836|9837|9838|9839|983a|983b|983c|"
           "983d|983e|983f|9850|9851|9852|9853|9854|9855|9856|9857|9858|"
           "9859|985A|985B|985C|985D|985E|985F|9870|9874|9875|9876|9877|"
           "98E4|13FE|143F|74A0|1435|163f|1900|1901|1114|150E)";
  }

  [[nodiscard]] std::string getAmdDGpuPattern() const noexcept
  {
    return "1002:(7480)";
  }

  [[nodiscard]] int countDevicesMatchingPattern( const std::string &pattern ) const noexcept
  {
    namespace fs = std::filesystem;
    try
    {
      const std::regex idRegex( "PCI_ID=" + pattern );
      int count = 0;

      for ( const auto &entry : fs::directory_iterator( "/sys/bus/pci/devices" ) )
      {
        auto ueventPath = entry.path() / "uevent";
        std::ifstream ueventFile( ueventPath );
        if ( not ueventFile )
          continue;

        bool hasDisplayClass = false;
        bool hasMatchingId   = false;
        std::string line;
        while ( std::getline( ueventFile, line ) )
        {
          if ( line == "PCI_CLASS=30000" )
            hasDisplayClass = true;
          if ( std::regex_search( line, idRegex ) )
            hasMatchingId = true;
          if ( hasDisplayClass and hasMatchingId )
            break;
        }

        if ( hasDisplayClass and hasMatchingId )
          ++count;
      }
      return count;
    }
    catch ( ... ) { return 0; }
  }

  [[nodiscard]] int countNvidiaDevices() const noexcept
  {
    namespace fs = std::filesystem;
    try
    {
      const std::string nvidiaVendorId = "0x10de";
      std::set< std::string > uniqueDevices;

      for ( const auto &entry : fs::directory_iterator( "/sys/bus/pci/devices" ) )
      {
        auto vendorPath = entry.path() / "vendor";
        std::ifstream vendorFile( vendorPath );
        if ( not vendorFile )
          continue;

        std::string vendorValue;
        std::getline( vendorFile, vendorValue );

        // Trim trailing whitespace / newline
        while ( not vendorValue.empty() and
                ( vendorValue.back() == '\n' or vendorValue.back() == '\r' or vendorValue.back() == ' ' ) )
          vendorValue.pop_back();

        if ( vendorValue == nvidiaVendorId )
        {
          // Group by base device (strip PCI function suffix, e.g. ".0")
          std::string devicePath = entry.path().string();
          size_t lastDot = devicePath.rfind( '.' );
          if ( lastDot != std::string::npos )
            devicePath = devicePath.substr( 0, lastDot );
          uniqueDevices.insert( devicePath );
        }
      }
      return static_cast< int >( uniqueDevices.size() );
    }
    catch ( ... ) { return 0; }
  }
};

/**
 * @brief HardwareMonitorWorker - Unified hardware monitoring
 *
 * Merges GPU information collection, CPU power monitoring, and NVIDIA Prime
 * state detection into a single worker thread.
 *
 * GPU monitoring (every cycle, 800ms):
 *   - Intel iGPU via RAPL energy counters and DRM frequency
 *   - AMD iGPU via hwmon sysfs interface
 *   - AMD dGPU via hwmon sysfs interface
 *   - NVIDIA dGPU via nvidia-smi command
 *
 * CPU power monitoring (every 3rd cycle ≈ 2400ms):
 *   - Intel RAPL power data for CPU package
 *   - Power constraints (PL1/PL2/PL4)
 *
 * Prime state monitoring (every 12th cycle ≈ 9600ms):
 *   - NVIDIA Prime GPU switching status
 *   - Requires prime-select utility (Ubuntu/TUXEDO OS)
 */
class HardwareMonitorWorker : public DaemonWorker
{
public:
  /**
   * @brief Callback function type for GPU data updates
   */
  using GpuDataCallback = std::function< void( const IGpuInfo &, const DGpuInfo & ) >;

  /**
   * @brief Callback function type for CPU power data updates
   * @param json JSON string with power data
   * @param cpuPowerWatts Current CPU power draw in watts (or -1.0 if unavailable)
   */
  using CpuPowerCallback = std::function< void( const std::string &json, double cpuPowerWatts ) >;

  /**
   * @brief Callback function type for CPU frequency updates (MHz)
   */
  using CpuFrequencyCallback = std::function< void( int frequencyMHz ) >;

  /**
   * @brief Callback function type for CPU temperature updates (°C)
   */
  using CpuTemperatureCallback = std::function< void( int temperatureCelsius ) >;

  /**
   * @brief Constructor
   * @param cpuPowerUpdateCallback Called with CPU power JSON + raw watts when updated
   * @param getSensorDataCollectionStatus Returns whether sensor data collection is enabled
   * @param setPrimeStateCallback Called with prime state string when updated
   */
  explicit HardwareMonitorWorker(
    std::shared_ptr< NvmlWrapper > nvml,
    CpuPowerCallback cpuPowerUpdateCallback,
    std::function< bool() > getSensorDataCollectionStatus,
    std::function< void( const std::string & ) > setPrimeStateCallback,
    bool isDisplayMuxDevice = false );

  ~HardwareMonitorWorker() override;

  // Prevent copy and move
  HardwareMonitorWorker( const HardwareMonitorWorker & ) = delete;
  HardwareMonitorWorker( HardwareMonitorWorker && ) = delete;
  HardwareMonitorWorker &operator=( const HardwareMonitorWorker & ) = delete;
  HardwareMonitorWorker &operator=( HardwareMonitorWorker && ) = delete;

  /**
   * @brief Callback function type for webcam status updates
   * @param available Whether webcam switch hardware is present
   * @param status    Current webcam on/off state
   */
  using WebcamStatusCallback = std::function< void( bool available, bool status ) >;

  /**
   * @brief Hardware reader that returns (available, status) from the webcam switch
   */
  using WebcamHwReader = std::function< std::pair< bool, bool >() >;

  /**
   * @brief Set callback for GPU data updates
   * @param callback Function called with integrated and discrete GPU info
   */
  void setGpuDataCallback( GpuDataCallback callback ) noexcept;

  /**
   * @brief Set callbacks for webcam monitoring
   *
   * Must be called before start(). The reader queries hardware for the
   * webcam switch state; the callback pushes the result to DBus data.
   *
   * @param reader   Returns (available, status) from hardware
   * @param callback Called with (available, status) on each poll
   */
  void setWebcamCallbacks( WebcamHwReader reader, WebcamStatusCallback callback ) noexcept;

  /**
   * @brief Set callback for CPU frequency updates
   *
   * Called every cycle (~800ms) with the current CPU frequency in MHz.
   * Must be called before start().
   *
   * @param callback Function called with frequency in MHz (or -1 if unavailable)
   */
  void setCpuFrequencyCallback( CpuFrequencyCallback callback ) noexcept;

  /**
   * @brief Set callback for CPU package temperature updates
   *
   * Called every cycle (~800ms) with the current package temperature in °C.
   * The value is -1 if no coretemp package sensor is available.
   *
   * @param callback Function called with temperature in °C
   */
  void setCpuTemperatureCallback( CpuTemperatureCallback callback ) noexcept;

  /**
   * @brief Check if NVIDIA Prime is supported on this system
   * @return true if Prime is supported
   */
  [[nodiscard]] bool isPrimeSupported() const noexcept;

protected:
  void onStart() override;
  void onWork() override;
  void onExit() override;

private:
  // --- GPU state ---
  GpuDeviceDetector m_gpuDetector;
  GpuDeviceCounts m_deviceCounts;
  std::shared_ptr< NvmlWrapper > m_nvml; ///< NVML API wrapper (shared, no-op if libnvidia-ml not present)
  GpuDataCallback m_gpuDataCallback;
  std::optional< std::string > m_amdIGpuHwmonPath;
  std::optional< std::string > m_amdDGpuHwmonPath;
  std::optional< std::string > m_intelIGpuDrmPath;
  int m_hwmonIGpuRetryCount;
  int m_hwmonDGpuRetryCount;

  // GPU RAPL for Intel iGPU power
  std::unique_ptr< IntelRAPLController > m_intelRAPLGpu;
  std::unique_ptr< PowerController > m_intelGpuPowerController;

  // --- CPU power state ---
  std::unique_ptr< IntelRAPLController > m_intelRAPLCpu;
  std::unique_ptr< PowerController > m_cpuPowerController;
  bool m_RAPLConstraint0Status;
  bool m_RAPLConstraint1Status;
  bool m_RAPLConstraint2Status;
  CpuPowerCallback m_cpuPowerUpdateCallback;
  std::function< bool() > m_getSensorDataCollectionStatus;

  // --- CPU frequency callback ---
  CpuFrequencyCallback m_cpuFrequencyCallback;

  // --- CPU temperature callback ---
  CpuTemperatureCallback m_cpuTemperatureCallback;

  // --- Prime state ---
  std::function< void( const std::string & ) > m_setPrimeState;
  bool m_primeSupported;
  bool m_isDisplayMuxDevice;         ///< Device with NVIDIA display mux (e.g. IBM15A10)
  bool m_displayConnectedToNvidia;   ///< Whether eDP display is wired to the NVIDIA GPU

  // --- Webcam state ---
  WebcamHwReader m_webcamHwReader;
  WebcamStatusCallback m_webcamStatusCallback;

  // --- Cycle counters for staggered polling ---
  uint32_t m_cycleCounter;

  // GPU methods
  void initGpu();
  [[nodiscard]] bool checkAmdIGpuHwmonPath() noexcept;
  [[nodiscard]] bool checkAmdDGpuHwmonPath() noexcept;
  [[nodiscard]] std::string getAmdIGpuHwmonPathImpl() const noexcept;
  [[nodiscard]] std::string getAmdDGpuHwmonPathImpl() const noexcept;
  [[nodiscard]] std::string getIntelIGpuDrmPathImpl() const noexcept;
  [[nodiscard]] IGpuInfo getIGpuValues() noexcept;
  [[nodiscard]] IGpuInfo getIntelIGpuValues( const IGpuInfo &base ) const noexcept;
  [[nodiscard]] IGpuInfo getAmdIGpuValues( const IGpuInfo &base ) const noexcept;
  [[nodiscard]] DGpuInfo getDGpuValues() noexcept;
  [[nodiscard]] DGpuInfo getNvidiaDGpuValues() const noexcept;
  [[nodiscard]] DGpuInfo getAmdDGpuValues( const DGpuInfo &base ) const noexcept;
  [[nodiscard]] double parseMaxAmdFreq( const std::string &frequencyString ) const noexcept;

  // CPU power methods
  void initCpuPower();
  void updateCpuPower();
  [[nodiscard]] double getCpuCurrentPower();
  [[nodiscard]] double getCpuMaxPowerLimit();

  // Prime methods
  void initPrime();
  void updatePrimeStatus() noexcept;
  [[nodiscard]] bool checkPrimeSupported() const noexcept;
  [[nodiscard]] std::string checkPrimeStatus() const noexcept;
  [[nodiscard]] std::string transformPrimeStatus( const std::string &status ) const noexcept;
  [[nodiscard]] bool getDisplayConnectedToNvidia() const noexcept;

  // Webcam methods
  void updateWebcamStatus() noexcept;

  // CPU frequency methods
  void updateCpuFrequency() noexcept;

  // CPU temperature methods
  void updateCpuTemperature() noexcept;
  [[nodiscard]] int readCoretempPackageTemperature() const noexcept;
};
