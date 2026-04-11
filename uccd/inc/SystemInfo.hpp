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

#include "profiles/DefaultProfiles.hpp"
#include <string>
#include <optional>

/**
 * @brief Laptop manufacturer / brand
 */
enum class LaptopManufacturer
{
  TUXEDO,
  XMG,
  MECHREVO,
  Uniwill,       // generic Uniwill (not rebranded or unknown brand)
  Unknown
};

/**
 * @brief Human-readable device information
 *
 * Detected once at daemon startup from DMI, PCI, and /proc/cpuinfo data.
 * Exposed to GUI/tray via a single D-Bus JSON blob.
 */
struct SystemInfo
{
  // CPU
  std::string cpuModel;           // e.g. "AMD Ryzen 9 7945HX"

  // GPUs
  std::string iGpuModel;          // e.g. "AMD Radeon 780M" or "" if absent
  std::string dGpuModel;          // e.g. "NVIDIA GeForce RTX 4070" or "" if absent

  // Laptop
  LaptopManufacturer manufacturer = LaptopManufacturer::Unknown;
  std::string manufacturerName;   // human-readable: "TUXEDO", "XMG", "Uniwill", …
  std::string laptopModel;        // human-readable: "TUXEDO Stellaris 16 Intel Gen6 (2024)"
  std::string productSKU;         // raw DMI product_sku
  std::string boardName;          // raw DMI board_name
  std::string boardVendor;        // raw DMI board_vendor
  std::string sysVendor;          // raw DMI sys_vendor

  // Internal device ID (if matched)
  std::optional< UniwillDeviceID > deviceId;

  /**
   * @brief Serialize to JSON string for D-Bus transport
   */
  [[nodiscard]] std::string toJSON() const;
};

/**
 * @brief Detect system hardware information
 *
 * Reads DMI data from /sys/class/dmi/id/, CPU model from /proc/cpuinfo,
 * and GPU models from the PCI sysfs tree.  Maps the detected UniwillDeviceID
 * to a human-readable laptop model name (brand + product line + year).
 *
 * @param deviceId  The already-identified UniwillDeviceID (or nullopt)
 * @return Populated SystemInfo struct
 */
SystemInfo detectSystemInfo( std::optional< UniwillDeviceID > deviceId );
