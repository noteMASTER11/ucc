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

#include "SysfsNode.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <ranges>

namespace fs = std::filesystem;

/**
 * @brief Keyboard backlight capabilities
 */
struct KeyboardBacklightCapabilities
{
  int zones = 0;
  int maxBrightness = 0;
  int maxRed = 0;
  int maxGreen = 0;
  int maxBlue = 0;
};

/**
 * @brief Single zone keyboard backlight state
 */
struct KeyboardBacklightState
{
  int brightness = 0;
  int red = 0;
  int green = 0;
  int blue = 0;
};

/**
 * @brief Synchronous controller for keyboard backlight hardware.
 *
 * Replaces the former KeyboardBacklightListener polling thread with a
 * direct, non-threaded implementation.  All public methods are meant to
 * be called from the main / D-Bus thread.
 *
 * Supports:
 * - White-only backlights
 * - RGB zone backlights (1-3 zones)
 * - Per-key RGB backlights
 */
class KeyboardBacklightController
{
public:
  KeyboardBacklightController() = default;
  ~KeyboardBacklightController() = default;

  // non-copyable, non-movable
  KeyboardBacklightController( const KeyboardBacklightController & ) = delete;
  KeyboardBacklightController &operator=( const KeyboardBacklightController & ) = delete;

  /**
   * @brief Detect keyboard backlight hardware and publish initial state.
   *
   * Call once during daemon startup.  Returns the capabilities JSON string
   * (or "null" when no backlight is found).
   */
  std::string init()
  {
    detectKeyboardBacklight();

    if ( m_capabilities.zones > 0 )
    {
      std::cout << "[KeyboardBacklight] Detected " << m_capabilities.zones
                << " zone(s), max brightness: " << m_capabilities.maxBrightness << std::endl;
      return capabilitiesToJSON();
    }

    std::cout << "[KeyboardBacklight] No keyboard backlight detected" << std::endl;
    return "null";
  }

  /**
   * @brief Build initial default states (max brightness, full colour).
   * @return JSON array string, e.g. "[{\"brightness\":255,...}, ...]"
   */
  std::string buildDefaultStatesJSON() const
  {
    if ( m_capabilities.zones == 0 )
      return "[]";

    std::vector< KeyboardBacklightState > defaults;
    for ( int i = 0; i < m_capabilities.zones; ++i )
    {
      KeyboardBacklightState s;
      s.brightness = m_capabilities.maxBrightness;
      s.red = m_capabilities.maxRed > 0 ? m_capabilities.maxRed : 0;
      s.green = m_capabilities.maxGreen > 0 ? m_capabilities.maxGreen : 0;
      s.blue = m_capabilities.maxBlue > 0 ? m_capabilities.maxBlue : 0;
      defaults.push_back( s );
    }
    return statesToJSON( defaults );
  }

  /**
   * @brief Apply a flat JSON states array directly to hardware.
   * @param statesJSON  A JSON array of state objects, e.g. "[{\"brightness\":128,\"red\":255,...}, ...]"
   * @return true on success
   *
   * This is the main entry point called from SetKeyboardBacklightStatesJSON
   * and profile-apply paths.
   */
  bool applyStatesFromJSON( const std::string &statesJSON )
  {
    if ( m_capabilities.zones == 0 )
      return false;

    if ( statesJSON.empty() || statesJSON == "[]" )
      return false;

    try
    {
      std::vector< KeyboardBacklightState > newStates;

      size_t pos = 0;
      while ( ( pos = statesJSON.find( '{', pos ) ) != std::string::npos )
      {
        size_t end = statesJSON.find( '}', pos );
        if ( end == std::string::npos )
          break;

        std::string stateObj = statesJSON.substr( pos, end - pos + 1 );
        KeyboardBacklightState kbs;

        kbs.brightness = std::clamp( extractInt( stateObj, "brightness" ), 0, m_capabilities.maxBrightness );
        kbs.red   = std::clamp( extractInt( stateObj, "red" ),   0, 255 );
        kbs.green = std::clamp( extractInt( stateObj, "green" ), 0, 255 );
        kbs.blue  = std::clamp( extractInt( stateObj, "blue" ),  0, 255 );

        newStates.push_back( kbs );
        pos = end + 1;
      }

      if ( !newStates.empty() )
      {
        applyStates( newStates );
        m_currentStates = newStates;
        return true;
      }
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[KeyboardBacklight] Error parsing states JSON: " << e.what() << std::endl;
    }
    return false;
  }

  /**
   * @brief Apply keyboard backlight states from a profile's keyboard data.
   * @param keyboardDataJSON  JSON object string: {"brightness":N,"states":[...]}
   * @return true on success
   */
  bool applyProfileKeyboardStates( const std::string &keyboardDataJSON )
  {
    if ( m_capabilities.zones == 0 )
      return false;

    try
    {
      std::string statesJSON = extractStatesArray( keyboardDataJSON );
      if ( !statesJSON.empty() )
        return applyStatesFromJSON( statesJSON );
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[KeyboardBacklight] Failed to apply profile keyboard states: " << e.what() << std::endl;
    }
    return false;
  }

  /** @brief Current states serialised as a JSON array */
  std::string currentStatesJSON() const
  {
    return statesToJSON( m_currentStates );
  }

  /** @brief True when hardware was detected */
  bool isAvailable() const { return m_capabilities.zones > 0; }

  const KeyboardBacklightCapabilities &capabilities() const { return m_capabilities; }

private:
  KeyboardBacklightCapabilities m_capabilities;
  std::vector< KeyboardBacklightState > m_currentStates;
  std::vector< std::string > m_ledPaths;

  // Common LED paths
  static constexpr const char *LEDS_WHITE_ONLY = "/sys/devices/platform/tuxedo_keyboard/leds/white:kbd_backlight";
  static constexpr const char *LEDS_WHITE_ONLY_NB05 = "/sys/bus/platform/devices/tuxedo_nb05_kbd_backlight/leds/white:kbd_backlight";
  static constexpr const char *LEDS_RGB_BASE = "/sys/devices/platform/tuxedo_keyboard/leds/rgb:kbd_backlight";

  // ---- hardware detection ----

  void detectKeyboardBacklight()
  {
    m_capabilities = KeyboardBacklightCapabilities();

    if ( checkWhiteBacklight( LEDS_WHITE_ONLY ) )
    {
      std::cout << "[KeyboardBacklight] Detected white-only keyboard backlight" << std::endl;
      return;
    }

    if ( checkWhiteBacklight( LEDS_WHITE_ONLY_NB05 ) )
    {
      std::cout << "[KeyboardBacklight] Detected white-only keyboard backlight (NB05)" << std::endl;
      return;
    }

    detectRGBBacklight();
  }

  bool checkWhiteBacklight( const std::string &basePath )
  {
    std::string maxBrightnessPath = basePath + "/max_brightness";
    std::error_code ec;

    if ( !fs::exists( maxBrightnessPath, ec ) )
      return false;

    SysfsNode< int > maxBrightness( maxBrightnessPath );
    auto value = maxBrightness.read();
    if ( !value.has_value() )
      return false;

    m_capabilities.zones = 1;
    m_capabilities.maxBrightness = value.value();
    m_ledPaths.push_back( basePath );
    return true;
  }

  void detectRGBBacklight()
  {
    findPerKeyRGBLEDs();

    if ( m_ledPaths.size() > 3 )
    {
      std::cout << "[KeyboardBacklight] Detected per-key RGB keyboard with "
                << m_ledPaths.size() << " zones" << std::endl;

      SysfsNode< int > maxBrightness( m_ledPaths[0] + "/max_brightness" );
      auto value = maxBrightness.read();

      m_capabilities.zones = static_cast< int >( m_ledPaths.size() );
      m_capabilities.maxBrightness = value.value_or( 255 );
      m_capabilities.maxRed = 0xFF;
      m_capabilities.maxGreen = 0xFF;
      m_capabilities.maxBlue = 0xFF;
      return;
    }

    std::vector< std::string > rgbPaths = {
      LEDS_RGB_BASE,
      std::string( LEDS_RGB_BASE ) + "_1",
      std::string( LEDS_RGB_BASE ) + "_2"
    };

    std::error_code ec;
    for ( const auto &path : rgbPaths )
    {
      if ( fs::exists( path + "/max_brightness", ec ) )
        m_ledPaths.push_back( path );
    }

    if ( !m_ledPaths.empty() )
    {
      SysfsNode< int > maxBrightness( m_ledPaths[0] + "/max_brightness" );
      auto value = maxBrightness.read();

      m_capabilities.zones = static_cast< int >( m_ledPaths.size() );
      m_capabilities.maxBrightness = value.value_or( 255 );
      m_capabilities.maxRed = 0xFF;
      m_capabilities.maxGreen = 0xFF;
      m_capabilities.maxBlue = 0xFF;

      std::cout << "[KeyboardBacklight] Detected " << m_capabilities.zones
                << " zone RGB keyboard backlight" << std::endl;
    }
  }

  void findPerKeyRGBLEDs()
  {
    std::vector< std::string > searchPaths = {
      "/sys/bus/hid/drivers/tuxedo-keyboard-ite",
      "/sys/bus/hid/drivers/ite_829x",
      "/sys/bus/hid/drivers/ite_8291",
      "/sys/bus/platform/drivers/tuxedo_nb04_kbd_backlight"
    };

    for ( const auto &driverPath : searchPaths )
    {
      std::error_code ec;
      if ( !fs::exists( driverPath, ec ) )
        continue;

      for ( const auto &entry : fs::directory_iterator( driverPath, ec ) )
      {
        if ( !entry.is_symlink( ec ) )
          continue;

        auto ledsPath = entry.path() / "leds";
        if ( !fs::exists( ledsPath, ec ) )
          continue;

        std::vector< std::pair< std::string, int > > foundLEDs;
        for ( const auto &ledEntry : fs::directory_iterator( ledsPath, ec ) )
        {
          std::string name = ledEntry.path().filename().string();
          if ( name.find( "rgb:kbd_backlight" ) == std::string::npos )
            continue;

          int zoneNum = 0;
          if ( name != "rgb:kbd_backlight" )
          {
            size_t underscorePos = name.find_last_of( '_' );
            if ( underscorePos != std::string::npos )
            {
              try { zoneNum = std::stoi( name.substr( underscorePos + 1 ) ); }
              catch ( ... ) { zoneNum = 0; }
            }
          }

          foundLEDs.push_back( { ledEntry.path().string(), zoneNum } );
        }

        std::ranges::sort( foundLEDs, []( const auto &a, const auto &b ) { return a.second < b.second; } );

        for ( const auto &led : foundLEDs )
          m_ledPaths.push_back( led.first );

        if ( !m_ledPaths.empty() )
          return;
      }
    }
  }

  // ---- JSON serialisation helpers ----

  std::string capabilitiesToJSON() const
  {
    if ( m_capabilities.zones == 0 )
      return "null";

    std::ostringstream oss;
    oss << "{\"modes\":[0]";
    oss << ",\"zones\":" << m_capabilities.zones;
    oss << ",\"maxBrightness\":" << m_capabilities.maxBrightness;

    if ( m_capabilities.maxRed > 0 )
    {
      oss << ",\"maxRed\":" << m_capabilities.maxRed;
      oss << ",\"maxGreen\":" << m_capabilities.maxGreen;
      oss << ",\"maxBlue\":" << m_capabilities.maxBlue;
    }

    oss << "}";
    return oss.str();
  }

  std::string statesToJSON( const std::vector< KeyboardBacklightState > &states ) const
  {
    std::ostringstream oss;
    oss << "[";
    for ( size_t i = 0; i < states.size(); ++i )
    {
      if ( i > 0 )
        oss << ",";
      oss << "{"
          << "\"brightness\":" << states[i].brightness << ","
          << "\"red\":" << states[i].red << ","
          << "\"green\":" << states[i].green << ","
          << "\"blue\":" << states[i].blue
          << "}";
    }
    oss << "]";
    return oss.str();
  }

  /**
   * @brief Extract the "states" array from a JSON object string.
   * @param json  JSON object: {"states":[...], ...}
   * @return The serialised [...] array, or empty string.
   */
  std::string extractStatesArray( const std::string &json ) const
  {
    std::string search = "\"states\":";
    size_t pos = json.find( search );
    if ( pos == std::string::npos )
      return "";

    pos += search.length();

    size_t arrayStart = json.find( '[', pos );
    if ( arrayStart == std::string::npos )
      return "";

    int depth = 0;
    size_t arrayEnd = arrayStart;
    for ( size_t i = arrayStart; i < json.length(); ++i )
    {
      if ( json[i] == '[' ) ++depth;
      else if ( json[i] == ']' ) --depth;
      if ( depth == 0 )
      {
        arrayEnd = i;
        break;
      }
    }

    if ( arrayEnd > arrayStart )
      return json.substr( arrayStart, arrayEnd - arrayStart + 1 );

    return "";
  }

  int extractInt( const std::string &json, const std::string &key ) const
  {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find( search );
    if ( pos == std::string::npos )
      return 0;

    pos += search.length();
    size_t endPos = json.find_first_of( ",}", pos );
    if ( endPos == std::string::npos )
      return 0;

    try { return std::stoi( json.substr( pos, endPos - pos ) ); }
    catch ( ... ) { return 0; }
  }

  // ---- hardware sysfs writes ----

  void applyStates( const std::vector< KeyboardBacklightState > &states )
  {
    if ( states.empty() || m_ledPaths.empty() )
      return;

    for ( size_t i = 0; i < m_ledPaths.size() && i < states.size(); ++i )
      setBrightness( i, states[i].brightness );

    if ( m_capabilities.maxRed > 0 )
    {
      setBufferInput( true );

      for ( size_t i = 0; i < m_ledPaths.size() && i < states.size(); ++i )
        setMultiIntensity( i, states[i].red, states[i].green, states[i].blue );

      setBufferInput( false );
    }
  }

  void setBrightness( size_t zoneIndex, int brightness )
  {
    if ( zoneIndex >= m_ledPaths.size() )
      return;

    SysfsNode< int > brightnessNode( m_ledPaths[zoneIndex] + "/brightness" );
    if ( !brightnessNode.write( brightness ) )
    {
      std::cerr << "[KeyboardBacklight] Failed to set brightness for zone "
                << zoneIndex << " to " << brightness << std::endl;
    }
  }

  void setMultiIntensity( size_t zoneIndex, int red, int green, int blue )
  {
    if ( zoneIndex >= m_ledPaths.size() )
      return;

    std::string multiIntensityPath = m_ledPaths[zoneIndex] + "/multi_intensity";
    std::error_code ec;

    if ( !fs::exists( multiIntensityPath, ec ) )
      return;

    std::string value = std::to_string( red ) + " " +
                       std::to_string( green ) + " " +
                       std::to_string( blue );

    std::ofstream file( multiIntensityPath );
    if ( !file.is_open() )
    {
      std::cerr << "[KeyboardBacklight] Failed to open " << multiIntensityPath << std::endl;
      return;
    }

    file << value << '\n';
    if ( !file.good() )
    {
      std::cerr << "[KeyboardBacklight] Failed to write multi_intensity for zone "
                << zoneIndex << std::endl;
    }
  }

  void setBufferInput( bool bufferOn )
  {
    if ( m_ledPaths.empty() )
      return;

    std::string bufferPath = m_ledPaths[0] + "/device/controls/buffer_input";
    std::error_code ec;

    if ( !fs::exists( bufferPath, ec ) )
      return;

    std::ofstream file( bufferPath );
    if ( !file.is_open() )
    {
      std::cerr << "[KeyboardBacklight] Failed to open " << bufferPath << std::endl;
      return;
    }

    file << ( bufferOn ? "1" : "0" ) << '\n';
    if ( !file.good() )
    {
      std::cerr << "[KeyboardBacklight] Failed to write buffer_input" << std::endl;
    }
  }
};
