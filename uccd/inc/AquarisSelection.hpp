#pragma once
#include <string>
#include <vector>

struct AquarisCandidate { std::string address; std::string name; };
inline int selectAquaris(const std::vector<AquarisCandidate>& devices,
                         const std::string& address, const std::string& name) {
  if (address.empty() || name.empty()) return -1;
  int candidate = -1;
  int matches = 0;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (devices[i].name != name) continue;
    if (devices[i].address == address) return static_cast<int>(i);
    candidate = static_cast<int>(i);
    ++matches;
  }
  return matches == 1 ? candidate : -1;
}
