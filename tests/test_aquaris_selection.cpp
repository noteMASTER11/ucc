#include "AquarisSelection.hpp"
#include <cassert>
int main() {
  const std::string name = "CoolingSystem LCT22002";
  const std::string mac = "AA:BB:CC:DD:EE:FF";
  assert(selectAquaris({{mac,name}}, mac,name) == 0);
  assert(selectAquaris({{"11",name},{mac,name}}, mac,name) == 1);
  assert(selectAquaris({{"11",name}}, mac,name) == 0); // one rotated BLE address
  assert(selectAquaris({{"11",name},{"22",name}}, mac,name) == -1);
  assert(selectAquaris({{mac,"unrelated"}}, mac,name) == -1);
  assert(selectAquaris({{mac,name}}, "",name) == -1);
  assert(selectAquaris({{mac,name}}, mac,"") == -1);
  assert(selectAquaris({}, mac,name) == -1);
}
