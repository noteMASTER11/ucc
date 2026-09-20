// SPDX-License-Identifier: GPL-3.0-or-later
#include <iostream>
#include <string>
#include "tuxedo_io_lib/tuxedo_io_api.hh"
#include "SysfsNode.hpp"

int main(int argc, char** argv) {
  if (argc != 2 || (std::string(argv[1]) != "--audit" && std::string(argv[1]) != "--fan-auto")) {
    std::cerr << "Usage: ucc-hardware --audit | --fan-auto\n";
    return 2;
  }
  const auto field = [](const char* name) {
    return SysfsNode<std::string>(std::string("/sys/class/dmi/id/")+name).read().value_or("");
  };
  if (field("sys_vendor")!="MECHREVO" || field("board_name")!="YAOSHI Series-X6AR55xY" ||
      field("board_version")!="Standard" || field("product_sku")!="0001") return 3;
  TuxedoIOAPI io;
  if (!io.wmiAvailable()) return 4;
  if (std::string(argv[1]) == "--fan-auto") {
    const bool ok = io.setFansAuto();
    std::cout << "Firmware fan control restore: " << (ok ? "OK" : "FAILED") << '\n';
    return ok ? 0 : 1;
  }
  bool ok = true;
  for (int fan = 0; fan != 2; ++fan) {
    int speed=-1, temp=-1;
    ok = io.getFanSpeedPercent(fan,speed) && ok;
    ok = io.getFanTemperature(fan,temp) && ok;
    std::cout << "fan=" << fan << " EC-duty=" << speed << " temp=" << temp << '\n';
  }
  return ok ? 0 : 1;
}
