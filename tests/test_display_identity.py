"""The capabilities alias must not replace the laptop's actual DMI identity."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class DisplayIdentity(unittest.TestCase):
    def test_mechrevo_display_name_keeps_real_board_and_other_models(self):
        src=(ROOT/'uccd/src/SystemInfo.cpp').read_text()
        functions=src[src.index('LaptopManufacturer classifyManufacturer('):src.index('//  JSON serialisation helper')]
        cpp='''#include "SystemInfo.hpp"
#include <algorithm>
#include <map>
#include <cassert>
#include <iostream>
std::string readFile(const std::string &path) {
  if(path.ends_with("board_name")) return "YAOSHI Series-X6AR55xY";
  if(path.ends_with("product_name")) return "YAOSHI Series";
  return "";
}
''' + functions + '''
int main() {
  auto manufacturer=classifyManufacturer("MECHREVO", "MECHREVO");
  if(manufacturerToString(manufacturer)!="MECHREVO") {
    std::cerr<<"Manufacturer must preserve MECHREVO\\n";return 1;
  }
  auto name=buildLaptopModel(UniwillDeviceID::STELLARIS16I07,manufacturer,"MECHREVO");
  assert(name=="MECHREVO YAOSHI Series-X6AR55xY");
  assert(buildLaptopModel(UniwillDeviceID::STELLARIS16I07,LaptopManufacturer::TUXEDO,"TUXEDO")
         =="TUXEDO Stellaris 16 Intel Gen7 (2025)");
}
'''
        with tempfile.TemporaryDirectory() as d:
            source=pathlib.Path(d)/'identity.cpp'; binary=pathlib.Path(d)/'identity'
            source.write_text(cpp)
            subprocess.run(['g++','-std=c++20','-I'+str(ROOT/'uccd/inc'),'-I'+str(ROOT/'include'),str(source),'-o',str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__': unittest.main()
