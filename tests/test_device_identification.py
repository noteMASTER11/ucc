"""Exercise the production device classifier with a fake read-only sysfs boundary.

Rejecting a changed vendor/board/version/SKU prevents granting another laptop
the Mechrevo power and water-cooler capabilities. No daemon or EC is opened.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DeviceIdentification(unittest.TestCase):
    def test_exact_match_and_near_misses(self):
        source = (ROOT / 'uccd/src/UccDBusService.cpp').read_text()
        start = source.index('std::optional< UniwillDeviceID > UccDBusService::identifyDevice()')
        end = source.index('\nvoid UccDBusService::computeDeviceCapabilities()', start)
        utils = (ROOT / 'include/Utils.hpp').read_text()
        support = utils[utils.index('inline bool isDeviceSupported()'):]
        support = support[:support.index('\n}') + 2]
        harness = r'''
#include "profiles/DefaultProfiles.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <optional>
#include <string>
static std::map<std::string, std::string> dmi;
template<class T> struct TestSysfsNode {
  std::string path;
  explicit TestSysfsNode(std::string p): path(p) {}
  std::optional<T> read() const {
    auto it = dmi.find(path.substr(path.rfind('/') + 1));
    return it == dmi.end() ? std::nullopt : std::optional<T>(it->second);
  }
};
struct FakeIO { void deviceModelIdStr(std::string &s) { s = "255"; } };
struct UccDBusService {
  FakeIO m_io;
  std::optional<UniwillDeviceID> identifyDevice();
};
using ucc::kSupportedDeviceSKUs;
''' + support.replace('SysfsNode<', 'TestSysfsNode<').replace('isDeviceSupported()', 'testIsDeviceSupported()') + source[start:end].replace('SysfsNode<', 'TestSysfsNode<') + r'''
int main() {
  UccDBusService service;
  const std::map<std::string,std::string> exact = {
    {"sys_vendor","MECHREVO"}, {"product_name","YAOSHI Series"},
    {"board_name","YAOSHI Series-X6AR55xY"}, {"board_version","Standard"},
    {"product_sku","0001"}
  };
  dmi = exact;
  if (!testIsDeviceSupported()) { std::cerr << "Exact Mechrevo must pass GUI/daemon whitelist\n"; return 1; }
  if (service.identifyDevice() != UniwillDeviceID::STELLARIS16I07) {
    std::cerr << "Exact Mechrevo must receive Stellaris 16 Intel Gen7 capabilities\n";
    return 1;
  }
  for (const auto &field : {"sys_vendor","board_name","board_version","product_sku"}) {
    dmi = exact; dmi[field] += "-different";
    assert(!service.identifyDevice().has_value());
    assert(!testIsDeviceSupported());
    dmi = exact; dmi.erase(field);
    assert(!service.identifyDevice().has_value());
    assert(!testIsDeviceSupported());
  }
  dmi.clear(); dmi["product_sku"] = "XNE16A25";
  assert(service.identifyDevice() == UniwillDeviceID::XNE16A25);
  dmi.clear(); dmi["product_sku"] = "STELLARIS16I07";
  assert(service.identifyDevice() == UniwillDeviceID::STELLARIS16I07);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory) / 'identify.cpp'
            binary = pathlib.Path(directory) / 'identify'
            cpp.write_text(harness)
            subprocess.run(['g++', '-std=c++20', '-I'+str(ROOT/'uccd/inc'),
                            '-I'+str(ROOT/'include'), str(cpp), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
