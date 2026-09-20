"""Run production discovery methods with only the Bluetooth boundary replaced.

Regression: an adapter absent at boot must become usable on a later retry,
without restarting the worker. No physical adapter is scanned by this test.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, name):
    marker = 'bool LCTWaterCoolerWorker::' + name + '()'
    if marker not in source:
        return ''
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class BluetoothStartup(unittest.TestCase):
    def test_adapter_appearing_after_startup_is_used_on_retry(self):
        source = (ROOT/'uccd/src/workers/LCTWaterCoolerWorker.cpp').read_text()
        harness = r'''
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <syslog.h>
struct Text {
  std::string toStdString() const { return "test adapter"; }
  Text toString() const { return {}; }
};
struct Adapter { Text name() const { return {}; } Text address() const { return {}; } };
struct Adapters : std::vector<Adapter> { bool isEmpty() const { return empty(); } };
struct QBluetoothLocalDevice {
  static inline bool available = false;
  static Adapters allDevices() { Adapters a; if (available) a.push_back({}); return a; }
};
struct QBluetoothDeviceDiscoveryAgent {
  enum Error { NoError };
  bool active = false;
  int starts = 0;
  explicit QBluetoothDeviceDiscoveryAgent(void*) {}
  bool isActive() const { return active; }
  void setLowEnergyDiscoveryTimeout(int) {}
  void start() { ++starts; active = true; }
  void deviceDiscovered() {}
  void finished() {}
  void errorOccurred(Error) {}
};
template<class... T> void connect(T&&...) {}
struct LCTWaterCoolerWorker {
  QBluetoothDeviceDiscoveryAgent* m_deviceDiscoveryAgent = nullptr;
  bool m_isDiscovering = false;
  std::vector<int> m_discoveredDevices;
  bool ensureDiscoveryAgent();
  bool startDiscoveryInternal();
  void onDeviceDiscovered() {}
  void onDiscoveryFinished() {}
  void setAvailableFlag(bool) {}
  void recordConnectionFailure() {}
  ~LCTWaterCoolerWorker() { delete m_deviceDiscoveryAgent; }
};
''' + function(source, 'ensureDiscoveryAgent') + function(source, 'startDiscoveryInternal') + r'''
int main() {
  LCTWaterCoolerWorker worker;
  assert(!worker.startDiscoveryInternal());
  assert(!worker.startDiscoveryInternal());
  QBluetoothLocalDevice::available = true;
  if (!worker.startDiscoveryInternal()) {
    std::cerr << "Discovery must recover when the adapter appears after startup\n";
    return 1;
  }
  auto* agent = worker.m_deviceDiscoveryAgent;
  assert(agent && agent->isActive());
  assert(worker.startDiscoveryInternal());
  assert(worker.m_deviceDiscoveryAgent == agent && agent->starts == 1);
  // An ended scan must be restartable even if the worker flag was left stale.
  agent->active = false;
  assert(worker.startDiscoveryInternal());
  assert(worker.m_deviceDiscoveryAgent == agent && agent->starts == 2);
  LCTWaterCoolerWorker normalStartup;
  assert(normalStartup.startDiscoveryInternal());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory)/'bluetooth_startup.cpp'
            binary = pathlib.Path(directory)/'bluetooth_startup'
            cpp.write_text(harness)
            subprocess.run(['g++', '-std=c++20', str(cpp), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
