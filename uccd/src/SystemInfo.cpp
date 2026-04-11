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

#include "SystemInfo.hpp"
#include "SysfsNode.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <syslog.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Trim leading and trailing whitespace (including newlines)
 */
std::string trim( const std::string &s )
{
  const auto start = s.find_first_not_of( " \t\r\n" );
  if ( start == std::string::npos )
    return {};
  return s.substr( start, s.find_last_not_of( " \t\r\n" ) - start + 1 );
}

/**
 * @brief Read a single-line sysfs/proc file and return its trimmed content
 */
std::string readFile( const std::string &path )
{
  std::ifstream file( path );
  if ( !file.is_open() )
    return {};
  std::string line;
  std::getline( file, line );
  return trim( line );
}

// ---------------------------------------------------------------------------
//  CPU detection
// ---------------------------------------------------------------------------

/**
 * @brief Extract the CPU model name from /proc/cpuinfo
 *
 * Reads the first "model name" line and returns it.
 */
std::string detectCpuModel()
{
  std::ifstream cpuinfo( "/proc/cpuinfo" );
  if ( !cpuinfo.is_open() )
    return {};

  std::string line;
  while ( std::getline( cpuinfo, line ) )
  {
    if ( line.rfind( "model name", 0 ) == 0 )
    {
      auto pos = line.find( ':' );
      if ( pos != std::string::npos )
        return trim( line.substr( pos + 1 ) );
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
//  GPU detection via PCI sysfs
// ---------------------------------------------------------------------------

// PCI class codes for display / 3D controllers
static constexpr unsigned PCI_CLASS_DISPLAY_VGA = 0x030000;
static constexpr unsigned PCI_CLASS_DISPLAY_3D  = 0x030200;
static constexpr unsigned PCI_CLASS_MASK        = 0xFFFF00;

struct PciGpu
{
  std::string name;   // from /sys/bus/pci/devices/<addr>/label or decoded vendor+device
  unsigned vendorId = 0;
  unsigned deviceId = 0;
  bool isIntegrated = false;
};

/**
 * @brief Look up a device name from the system pci.ids database
 *
 * The pci.ids file uses a simple text format:
 *   VVVV  Vendor Name          (vendor line, no indent, 4-digit lowercase hex)
 *   \tDDDD  Device Name        (device line, one tab indent, under that vendor)
 *
 * Common locations: /usr/share/hwdata/pci.ids (Fedora/Arch/NixOS),
 *                   /usr/share/misc/pci.ids  (Debian/Ubuntu)
 */
std::string lookupPciIds( unsigned vendor, unsigned device )
{
  static const char *pciIdsPaths[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
    "/usr/share/pci.ids",
    nullptr
  };

  // Format vendor/device as lowercase 4-digit hex for matching
  char vendorHex[5], deviceHex[5];
  std::snprintf( vendorHex, sizeof( vendorHex ), "%04x", vendor );
  std::snprintf( deviceHex, sizeof( deviceHex ), "%04x", device );

  for ( const char **p = pciIdsPaths; *p; ++p )
  {
    std::ifstream file( *p );
    if ( !file.is_open() )
      continue;

    bool inVendor = false;
    std::string line;
    while ( std::getline( file, line ) )
    {
      // Skip comments and empty lines
      if ( line.empty() || line[0] == '#' )
        continue;

      // Vendor line: starts with hex digit at column 0
      if ( line[0] != '\t' )
      {
        // Check if this is our vendor
        if ( line.size() >= 4 && line.compare( 0, 4, vendorHex ) == 0 )
          inVendor = true;
        else if ( inVendor )
          break; // Passed our vendor section, stop searching
        continue;
      }

      // Device line: tab + 4-digit hex + spaces + name
      if ( inVendor && line.size() >= 6 && line[0] == '\t' && line[1] != '\t' )
      {
        if ( line.compare( 1, 4, deviceHex ) == 0 )
        {
          // Found it — extract the name after "DDDD  "
          auto nameStart = line.find_first_not_of( " \t", 5 );
          if ( nameStart != std::string::npos )
            return trim( line.substr( nameStart ) );
        }
      }
    }
  }

  return {};
}

/**
 * @brief Decode a PCI vendor/device pair into a human-readable string
 *
 * Tries multiple sources in order: sysfs label, DRM product_name,
 * NVIDIA /proc, pci.ids database, then generic vendor string fallback.
 */
std::string decodePciName( unsigned vendor, [[maybe_unused]] unsigned device,
                           const std::string &sysfsDir )
{
  // Try kernel label first (some drivers expose a nice name)
  {
    std::string label = readFile( sysfsDir + "/label" );
    if ( !label.empty() )
      return label;
  }

  // Try the DRM card sysfs for a more descriptive name
  // /sys/bus/pci/devices/<addr>/drm/card*/device/product_name   (NVIDIA)
  // The NVIDIA proprietary driver exposes the marketing name in
  // /proc/driver/nvidia/gpus/<addr>/information, but parsing the DRM
  // subsystem is more portable and works with nouveau too.

  // Walk drm subdirectory if present
  const fs::path drmDir = fs::path( sysfsDir ) / "drm";
  if ( fs::exists( drmDir ) )
  {
    try
    {
      for ( const auto &entry : fs::directory_iterator( drmDir ) )
      {
        // card0, card1, …
        const auto cardName = entry.path().filename().string();
        if ( cardName.rfind( "card", 0 ) != 0 )
          continue;

        // Try common locations for GPU product name
        for ( const char *subpath : { "device/product_name", "product_name" } )
        {
          std::string productName = readFile( ( entry.path() / subpath ).string() );
          if ( !productName.empty() )
            return productName;
        }
      }
    }
    catch ( ... )
    {
      // ignore permission or other errors
    }
  }

  // NVIDIA proprietary driver puts a nice name under /proc
  try
  {
    // The address directory name looks like "0000:01:00.0"
    const std::string baseName = fs::path( sysfsDir ).filename().string();
    const std::string nvidiaInfoPath =
      "/proc/driver/nvidia/gpus/" + baseName + "/information";

    std::ifstream nvidiaInfo( nvidiaInfoPath );
    if ( nvidiaInfo.is_open() )
    {
      std::string line;
      while ( std::getline( nvidiaInfo, line ) )
      {
        if ( line.rfind( "Model:", 0 ) == 0 )
        {
          auto pos = line.find( ':' );
          if ( pos != std::string::npos )
            return trim( line.substr( pos + 1 ) );
        }
      }
    }
  }
  catch ( ... ) { }

  // Try the system pci.ids database (same source lspci uses)
  {
    std::string pciName = lookupPciIds( vendor, device );
    if ( !pciName.empty() )
      return pciName;
  }

  // Fallback: vendor-generic strings
  switch ( vendor )
  {
    case 0x10de: return "NVIDIA GPU";
    case 0x1002: return "AMD GPU";
    case 0x8086: return "Intel GPU";
    default:     return "Unknown GPU";
  }
}

/**
 * @brief Scan the PCI bus for display/3D controllers
 *
 * Populates separate integrated and discrete GPU entries.
 */
void detectGpus( std::string &iGpu, std::string &dGpu )
{
  const std::string pciBasePath = "/sys/bus/pci/devices";

  if ( !fs::exists( pciBasePath ) )
    return;

  try
  {
    for ( const auto &entry : fs::directory_iterator( pciBasePath ) )
    {
      const std::string devDir = entry.path().string();
      const std::string classStr = readFile( devDir + "/class" );
      if ( classStr.empty() )
        continue;

      unsigned pciClass = 0;
      try { pciClass = static_cast< unsigned >( std::stoul( classStr, nullptr, 16 ) ); }
      catch ( ... ) { continue; }

      if ( ( pciClass & PCI_CLASS_MASK ) != PCI_CLASS_DISPLAY_VGA &&
           ( pciClass & PCI_CLASS_MASK ) != PCI_CLASS_DISPLAY_3D )
        continue;

      // Read vendor / device IDs
      unsigned vendorId = 0;
      unsigned deviceId = 0;
      try
      {
        vendorId = static_cast< unsigned >( std::stoul( readFile( devDir + "/vendor" ), nullptr, 16 ) );
        deviceId = static_cast< unsigned >( std::stoul( readFile( devDir + "/device" ), nullptr, 16 ) );
      }
      catch ( ... ) { continue; }

      const std::string name = decodePciName( vendorId, deviceId, devDir );

      // Heuristic: Intel and AMD integrated GPUs sit on bus 00.
      // Discrete GPUs (NVIDIA, AMD dGPU) are typically on bus 01+.
      const std::string busAddr = entry.path().filename().string();
      // Format: "DDDD:BB:DD.F" — extract BB
      bool integrated = false;
      if ( busAddr.size() >= 7 )
      {
        std::string busPart = busAddr.substr( 5, 2 ); // BB
        try
        {
          unsigned busNumber = static_cast< unsigned >( std::stoul( busPart, nullptr, 16 ) );
          // Bus 0 with Intel/AMD vendor = integrated (usually)
          integrated = ( busNumber == 0 ) &&
                       ( vendorId == 0x8086 || vendorId == 0x1002 );
        }
        catch ( ... ) { }
      }

      // Also treat VGA-class Intel as integrated regardless of bus
      if ( vendorId == 0x8086 && ( pciClass & PCI_CLASS_MASK ) == PCI_CLASS_DISPLAY_VGA )
        integrated = true;

      if ( integrated )
      {
        if ( iGpu.empty() )
          iGpu = name;
      }
      else
      {
        if ( dGpu.empty() )
          dGpu = name;
      }
    }
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_WARNING, "[SystemInfo] GPU detection error: %s", e.what() );
  }
}

// ---------------------------------------------------------------------------
//  Manufacturer / laptop model detection
// ---------------------------------------------------------------------------

/**
 * @brief Determine the laptop manufacturer from DMI vendor strings
 */
LaptopManufacturer classifyManufacturer( const std::string &sysVendor,
                                         const std::string &boardVendor )
{
  auto containsCI = []( const std::string &haystack, const std::string &needle ) -> bool
  {
    std::string h = haystack;
    std::string n = needle;
    std::transform( h.begin(), h.end(), h.begin(), ::tolower );
    std::transform( n.begin(), n.end(), n.begin(), ::tolower );
    return h.find( n ) != std::string::npos;
  };

  if ( containsCI( sysVendor, "TUXEDO" ) || containsCI( boardVendor, "TUXEDO" ) )
    return LaptopManufacturer::TUXEDO;

  if ( containsCI( sysVendor, "Schenker" ) || containsCI( boardVendor, "Schenker" ) ||
       containsCI( sysVendor, "XMG" ) || containsCI( boardVendor, "XMG" ) )
    return LaptopManufacturer::XMG;

  if ( containsCI( sysVendor, "MECHREVO" ) || containsCI( boardVendor, "MECHREVO" ) )
    return LaptopManufacturer::MECHREVO;

  if ( containsCI( sysVendor, "Uniwill" ) || containsCI( boardVendor, "Uniwill" ) ||
       containsCI( sysVendor, "UNIWILL" ) || containsCI( boardVendor, "UNIWILL" ) )
    return LaptopManufacturer::Uniwill;

  return LaptopManufacturer::Unknown;
}

std::string manufacturerToString( LaptopManufacturer m )
{
  switch ( m )
  {
    case LaptopManufacturer::TUXEDO:  return "TUXEDO";
    case LaptopManufacturer::XMG:     return "XMG";
    case LaptopManufacturer::MECHREVO: return "MECHREVO";
    case LaptopManufacturer::Uniwill: return "Uniwill";
    default:                          return "Unknown";
  }
}

/**
 * @brief Map UniwillDeviceID → human-readable laptop model including year
 *
 * The model strings follow the pattern: "<Brand> <ProductLine> <Size> <CPU vendor> <Generation> (<Year>)"
 */
struct DeviceInfo
{
  const char *modelName;
  const char *year;
  LaptopManufacturer defaultBrand;   // used when DMI vendor is ambiguous
};

// clang-format off
static const std::map< UniwillDeviceID, DeviceInfo > deviceInfoMap =
{
  // InfinityBook Pro
  { UniwillDeviceID::IBP17G6,          { "InfinityBook Pro 17 Gen6",              "2021", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBP14G6_TUX,      { "InfinityBook Pro 14 Gen6",              "2021", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBP14G6_TRX,      { "InfinityBook Pro 14 Gen6",              "2021", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBP14G6_TQF,      { "InfinityBook Pro 14 Gen6",              "2021", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBP14G7_AQF_ARX,  { "InfinityBook Pro 14 Gen7",              "2022", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBPG8,            { "InfinityBook Pro Gen8",                 "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBPG10AMD,        { "InfinityBook Pro Gen10 AMD",            "2025", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::IBM15A10,         { "InfinityBook Metal 15 Gen10 AMD",       "2025", LaptopManufacturer::TUXEDO } },

  // Pulse
  { UniwillDeviceID::PULSE1403,        { "Pulse 14 Gen3",                         "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::PULSE1404,        { "Pulse 14 Gen4",                         "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::PULSE1502,        { "Pulse 15 Gen2",                         "2022", LaptopManufacturer::TUXEDO } },

  // Aura
  { UniwillDeviceID::AURA14G3,         { "Aura 14 Gen3",                          "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::AURA15G3,         { "Aura 15 Gen3",                          "2023", LaptopManufacturer::TUXEDO } },

  // Polaris
  { UniwillDeviceID::POLARIS1XA02,     { "Polaris 15/17 AMD Gen2",                "2022", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::POLARIS1XI02,     { "Polaris 15/17 Intel Gen2",              "2022", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::POLARIS1XA03,     { "Polaris 15/17 AMD Gen3",                "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::POLARIS1XI03,     { "Polaris 15/17 Intel Gen3",              "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::POLARIS1XA05,     { "Polaris 15/17 AMD Gen5",                "2025", LaptopManufacturer::TUXEDO } },

  // Stellaris
  { UniwillDeviceID::STELLARIS1XA03,   { "Stellaris 15/17 AMD Gen3",              "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS1XI03,   { "Stellaris 15/17 Intel Gen3",            "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS1XI04,   { "Stellaris 15/17 Intel Gen4",            "2023", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STEPOL1XA04,      { "Stellaris/Polaris AMD Gen4",            "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS1XI05,   { "Stellaris 15/17 Intel Gen5",            "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS1XA05,   { "Stellaris 15/17 AMD Gen5",              "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS16I06,   { "Stellaris 16 Intel Gen6",               "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS17I06,   { "Stellaris 17 Intel Gen6",               "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLSL15A06,     { "Stellaris Slim 15 AMD Gen6",            "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLSL15I06,     { "Stellaris Slim 15 Intel Gen6",          "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS16A07,   { "Stellaris 16 AMD Gen7",                 "2025", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::STELLARIS16I07,   { "Stellaris 16 Intel Gen7",               "2025", LaptopManufacturer::TUXEDO } },

  // Gemini
  { UniwillDeviceID::GEMINI17I04,      { "Gemini 17 Intel Gen4",                  "2024", LaptopManufacturer::TUXEDO } },

  // Sirius
  { UniwillDeviceID::SIRIUS1601,       { "Sirius 16 Gen1",                        "2024", LaptopManufacturer::TUXEDO } },
  { UniwillDeviceID::SIRIUS1602,       { "Sirius 16 Gen2",                        "2025", LaptopManufacturer::TUXEDO } },

  // XMG models
  { UniwillDeviceID::XNE16E25,         { "NEO 16 Intel E25",                     "2025", LaptopManufacturer::XMG } },
  { UniwillDeviceID::XNE16A25,         { "NEO 16 AMD A25",                       "2025", LaptopManufacturer::XMG } },
};
// clang-format on

/**
 * @brief Build the human-readable laptop model string
 *
 * Uses the deviceInfoMap when the device is identified.
 * Falls back to raw DMI product_name / board_name otherwise.
 */
std::string buildLaptopModel( std::optional< UniwillDeviceID > deviceId,
                              LaptopManufacturer manufacturer,
                              const std::string &sysVendor )
{
  if ( deviceId.has_value() )
  {
    if ( auto it = deviceInfoMap.find( *deviceId ); it != deviceInfoMap.end() )
    {
      const auto &info = it->second;
      // Use detected manufacturer if known, otherwise fall back to the default
      std::string brand;
      if ( manufacturer != LaptopManufacturer::Unknown )
        brand = manufacturerToString( manufacturer );
      else
        brand = manufacturerToString( info.defaultBrand );

      return brand + " " + info.modelName + " (" + info.year + ")";
    }
  }

  // Fallback: use raw DMI product_name
  std::string productName = readFile( "/sys/class/dmi/id/product_name" );
  if ( !productName.empty() )
  {
    if ( manufacturer != LaptopManufacturer::Unknown )
      return manufacturerToString( manufacturer ) + " " + productName;
    if ( !sysVendor.empty() )
      return sysVendor + " " + productName;
    return productName;
  }

  return "Unknown Laptop";
}

// ---------------------------------------------------------------------------
//  JSON serialisation helper
// ---------------------------------------------------------------------------

std::string jsonEscapeValue( const std::string &s )
{
  std::ostringstream oss;
  for ( const char c : s )
  {
    switch ( c )
    {
      case '"':  oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\n': oss << "\\n";  break;
      case '\r': oss << "\\r";  break;
      case '\t': oss << "\\t";  break;
      default:   oss << c;      break;
    }
  }
  return oss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

std::string SystemInfo::toJSON() const
{
  std::ostringstream oss;
  oss << "{"
      << "\"cpuModel\":\"" << jsonEscapeValue( cpuModel ) << "\","
      << "\"iGpuModel\":\"" << jsonEscapeValue( iGpuModel ) << "\","
      << "\"dGpuModel\":\"" << jsonEscapeValue( dGpuModel ) << "\","
      << "\"manufacturer\":\"" << jsonEscapeValue( manufacturerName ) << "\","
      << "\"laptopModel\":\"" << jsonEscapeValue( laptopModel ) << "\","
      << "\"productSKU\":\"" << jsonEscapeValue( productSKU ) << "\","
      << "\"boardName\":\"" << jsonEscapeValue( boardName ) << "\","
      << "\"boardVendor\":\"" << jsonEscapeValue( boardVendor ) << "\","
      << "\"sysVendor\":\"" << jsonEscapeValue( sysVendor ) << "\""
      << "}";
  return oss.str();
}

SystemInfo detectSystemInfo( std::optional< UniwillDeviceID > deviceId )
{
  SystemInfo info;
  info.deviceId = deviceId;

  // DMI data
  const std::string dmiBase = "/sys/class/dmi/id";
  info.productSKU  = readFile( dmiBase + "/product_sku" );
  info.boardName   = readFile( dmiBase + "/board_name" );
  info.boardVendor = readFile( dmiBase + "/board_vendor" );
  info.sysVendor   = readFile( dmiBase + "/sys_vendor" );

  syslog( LOG_INFO, "[SystemInfo] DMI: sku='%s' board='%s' boardVendor='%s' sysVendor='%s'",
          info.productSKU.c_str(), info.boardName.c_str(),
          info.boardVendor.c_str(), info.sysVendor.c_str() );

  // CPU
  info.cpuModel = detectCpuModel();
  syslog( LOG_INFO, "[SystemInfo] CPU: %s", info.cpuModel.c_str() );

  // GPUs
  detectGpus( info.iGpuModel, info.dGpuModel );
  syslog( LOG_INFO, "[SystemInfo] iGPU: %s", info.iGpuModel.empty() ? "(none)" : info.iGpuModel.c_str() );
  syslog( LOG_INFO, "[SystemInfo] dGPU: %s", info.dGpuModel.empty() ? "(none)" : info.dGpuModel.c_str() );

  // Manufacturer
  info.manufacturer = classifyManufacturer( info.sysVendor, info.boardVendor );
  info.manufacturerName = manufacturerToString( info.manufacturer );

  // Laptop model
  info.laptopModel = buildLaptopModel( deviceId, info.manufacturer, info.sysVendor );

  syslog( LOG_INFO, "[SystemInfo] Laptop: %s (manufacturer: %s)",
          info.laptopModel.c_str(), info.manufacturerName.c_str() );

  return info;
}
