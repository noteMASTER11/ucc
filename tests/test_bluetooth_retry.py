"""Exercise real retry handlers with a fake BLE transport and controllable age."""
import pathlib,subprocess,tempfile,unittest,shlex
ROOT=pathlib.Path(__file__).resolve().parents[1]
def method(source,name):
    marker='void LCTWaterCoolerWorker::'+name+'('
    if marker not in source:return ''
    start=source.index(marker);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
class RetryPolicy(unittest.TestCase):
    def test_errors_wait_before_retry_and_discard_stale_fast_target(self):
        source=(ROOT/'uccd/src/workers/LCTWaterCoolerWorker.cpp').read_text()
        constants=source[source.index('static constexpr int TICK_INTERVAL_MS'):source.index('// Static UUID constants')]
        methods='\n'.join(method(source,n) for n in ['recordConnectionFailure','onBleError','onBleDisconnected','handleError','handleDisconnected','handleConnecting','handleReconnecting','requestStartDiscovery','requestConnectToDevice'])
        code=r'''
#include <QString>
#include <iostream>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cassert>
#include <syslog.h>
namespace ucc {enum class LCTDeviceModel {LCT21001};}
struct QLowEnergyController {enum Error {ConnectionError};};
enum class WaterCoolerState {Disconnected,Discovering,Reconnecting,Connecting,Connected,Error};
struct Device {struct Address {QString toString() const{return "00:00:00:00:00:00";}}; Address address() const{return {};}};
class LCTWaterCoolerWorker {
public:
 std::atomic<WaterCoolerState> m_state{WaterCoolerState::Reconnecting};
 std::atomic<bool> m_isConnected{false};std::atomic<int> m_lastPumpVoltage{-1},m_lastFanSpeed{-1};
 ucc::LCTDeviceModel m_connectedModel=ucc::LCTDeviceModel::LCT21001;
 int m_consecutiveFailures=0,m_fastReconnectFailures=0,attempts=0,cleanups=0,purges=0;
 bool m_suspending=false;bool m_hasKnownDevice=true;Device m_lastKnownDeviceInfo;
 struct {bool waterCoolerScanningEnabled=true,waterCoolerConnected=false;} m_dbusData;
 std::chrono::steady_clock::time_point m_lastDiscoveryStart=std::chrono::steady_clock::now();
 int elapsed=0;
 int secondsSinceLastDiscovery() const{return elapsed;}
 void setAvailableFlag(bool){}
 bool connectToKnownDevice(){++attempts;return false;}
 bool startDiscoveryInternal(){++attempts;return false;}
 bool connectToDevice(const QString&){++attempts;return false;}
 void cleanupBleController(){++cleanups;m_isConnected=false;}
 bool purgeBlueZDeviceCache(const QString&){++purges;return true;}
 void recordConnectionFailure();void onBleError(QLowEnergyController::Error);
 void onBleDisconnected();void handleError();void handleDisconnected();void handleConnecting();void handleReconnecting();
 void requestStartDiscovery();void requestConnectToDevice(const QString&);
};
'''+constants+methods+r'''
int main(){
 LCTWaterCoolerWorker w;
 w.onBleError(QLowEnergyController::ConnectionError);
 if(w.m_state!=WaterCoolerState::Error){std::cerr<<"Immediate BLE errors bypass retry backoff\n";return 1;}
 assert(w.m_consecutiveFailures==1 && w.m_fastReconnectFailures==1);
 w.onBleDisconnected();assert(w.m_consecutiveFailures==1);
 w.elapsed=1;w.handleError();assert(w.m_state==WaterCoolerState::Error && w.attempts==0);
 w.elapsed=6;w.handleError();assert(w.m_state==WaterCoolerState::Disconnected);
 for(int i=1;i<3;++i){w.m_state=WaterCoolerState::Reconnecting;w.onBleError(QLowEnergyController::ConnectionError);}
 assert(w.m_fastReconnectFailures>=3);
 w.elapsed=121;w.handleError();w.handleDisconnected();assert(!w.m_hasKnownDevice);
 assert(w.purges==0); // Retry policy must not unpair the device automatically.
 LCTWaterCoolerWorker timeout;timeout.m_state=WaterCoolerState::Connecting;timeout.elapsed=20;timeout.handleConnecting();
 assert(timeout.m_state==WaterCoolerState::Error);
 timeout.m_state=WaterCoolerState::Reconnecting;timeout.handleReconnecting();assert(timeout.m_state==WaterCoolerState::Error);
 LCTWaterCoolerWorker exhausted;exhausted.m_consecutiveFailures=100;exhausted.m_state=WaterCoolerState::Error;
 exhausted.elapsed=119;exhausted.handleError();assert(exhausted.m_state==WaterCoolerState::Error);
 exhausted.elapsed=121;exhausted.handleError();assert(exhausted.m_state==WaterCoolerState::Disconnected);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp=pathlib.Path(d)/'retry.cpp';exe=pathlib.Path(d)/'retry';cpp.write_text(code)
            flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Core'],text=True))
            subprocess.run(['g++','-std=c++20','-fPIC',str(cpp),*flags,'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__':unittest.main()
