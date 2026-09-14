"""Exercise actual power policy/services with fake driver, NVS, HTTP and time.

Run: wsl --exec python3 tools/test_wifi_power.py
Hardware power consumption, DHCP and AP availability require bench validation.
"""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
state = source[source.index("uint8_t wifiPowerMode ="):source.index("const uint8_t STATUS_LED_PIN")]
services = source[source.index("const char* wifiPowerModeName() {"):source.index("void loop() {")]
stubs = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>
#include <iostream>
using esp_err_t = int;
const int ESP_OK=0, ESP_FAIL=-1, WIFI_PS_NONE=0, WIFI_PS_MIN_MODEM=1;
class String : public std::string {
public:
  using std::string::string;
  String(const std::string& s):std::string(s){}
  template<class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
  String(T n):std::string(std::to_string(n)){}
};
uint32_t now=0;
uint32_t millis(){return now;}
void recordDiagnosticEvent(const char*,const String&){}
bool controlledRestartPending=false, apRunning=true, apEnabled=true;
bool infrastructureSavedNetworkConfigured=true, infrastructureDisconnectActive=false;
bool nativeReconnectStateInitialized=false, wifiScanInProgress=false;
bool initialWifiScanPending=false, bleDiagnosticScanActive=false;
bool csvExportInProgress=false, infrastructureUserConnectionActive=false;
bool infrastructureReconnectAttemptActive=false, infrastructureReconnectPending=false;
bool infrastructureVisibleDisconnectedActive=false, nativeReconnectSawDisconnect=false;
bool mdnsStarted=true, webServerStarted=true, hasIp=false;
uint32_t infrastructureVisibleDisconnectedSinceMs=0, scanIntervalSeconds=300;
const char* infrastructureRecoveryState="CONNECTED";
bool infrastructureHasIp(){return hasIp;}
void ledDiagUpdateInfraState(){}
int starts=0,stops=0,connects=0,startResult=0,stopResult=0;
int esp_wifi_start(){++starts;return startResult;}
int esp_wifi_stop(){++stops;return stopResult;}
int esp_wifi_connect(){++connects;return ESP_OK;}
struct Wifi {
  bool reconnect=true,sleepOk=true; int sleep=-1;
  void setAutoReconnect(bool v){reconnect=v;}
  bool setSleep(int v){sleep=v;return sleepOk;}
} WiFi;
struct Server { int begins=0,stops=0; void begin(){++begins;} void stop(){++stops;} } server;
struct Mdns { void end(){} } MDNS;
void startMdnsService(){mdnsStarted=true;}
struct Prefs {
  int saved=-1;
  void begin(const char*,bool){} void end(){}
  void putUChar(const char*,uint8_t m){saved=m;}
  void putUInt(const char*,uint32_t m){saved=m;}
} preferences;
'''
tests = r'''
int main(){
  // Continuous modes never stop Wi-Fi, and select different modem policies.
  serviceWifiPower(); assert(WiFi.sleep==WIFI_PS_NONE && stops==0);
  saveWifiPowerMode(1); serviceWifiPower();
  assert(preferences.saved==1 && WiFi.sleep==WIFI_PS_MIN_MODEM && stops==0);
  now=100; saveWifiPowerMode(2); serviceWifiPower();
  now=60099; serviceWifiPower(); assert(!wifiRadioSleeping);
  now=60100; wifiScanInProgress=true; serviceWifiPower(); assert(stops==0);
  // Completion starts a new 60-second window, including rollover handling.
  wifiScanInProgress=false; noteWifiPowerScanFinished();
  now=120099; serviceWifiPower(); assert(stops==0);
  now=120100; serviceWifiPower();
  assert(wifiRadioSleeping && stops==1 && !apRunning && !WiFi.reconnect);
  assert(!infrastructureDisconnectActive && !infrastructureReconnectAttemptActive);
  serviceWifiPower(); assert(stops==1 && starts==0); // no spontaneous wake
  assert(wakeWifiRadio()); wifiScanInProgress=true; serviceWifiPower();
  assert(starts==1 && apRunning && connects==0 && server.begins==1);
  wifiScanInProgress=false; noteWifiPowerScanFinished(); serviceWifiPower();
  assert(connects==1); serviceWifiPower(); assert(connects==1);
  now+=60000; serviceWifiPower(); assert(wifiRadioSleeping && stops==2);
  // USB/HTTP/config changes restore access without a reboot or another scan.
  saveWifiPowerMode(0); serviceWifiPower();
  assert(!wifiRadioSleeping && starts==2 && WiFi.sleep==WIFI_PS_NONE);
  now=UINT32_MAX-5000; saveWifiPowerMode(2);
  now=54998; serviceWifiPower(); assert(!wifiRadioSleeping);
  now=54999; serviceWifiPower(); assert(wifiRadioSleeping);
  // Failed start stays asleep and retries at a bounded rate.
  startResult=ESP_FAIL; assert(!wakeWifiRadio()); int attempts=starts;
  now+=4999; assert(!wakeWifiRadio() && starts==attempts);
  now++; startResult=ESP_OK; assert(wakeWifiRadio());
  noteWifiPowerScanFinished(); now+=60000;
  // Failed stop restores HTTP and does not claim the radio is off.
  stopResult=ESP_FAIL; serviceWifiPower(); assert(!wifiRadioSleeping);
  int stopAttempts=stops; serviceWifiPower(); assert(stops==stopAttempts);
  now+=5000; stopResult=ESP_OK; serviceWifiPower(); assert(wifiRadioSleeping);
  saveWifiPowerMode(1); serviceWifiPower(); assert(!wifiRadioSleeping);
  // An interval no longer than the access window has no off period.
  scanIntervalSeconds=60; saveWifiPowerMode(2); now+=60000;
  serviceWifiPower(); assert(!wifiRadioSleeping);
  scanIntervalSeconds=300; csvExportInProgress=true;
  serviceWifiPower(); assert(!wifiRadioSleeping);
  csvExportInProgress=false; controlledRestartPending=true;
  serviceWifiPower(); assert(!wifiRadioSleeping);
  controlledRestartPending=false; serviceWifiPower(); assert(wifiRadioSleeping);
  // Driver rejection of modem sleep remains unapplied and is retried.
  WiFi.sleepOk=false; saveWifiPowerMode(1); serviceWifiPower();
  assert(wifiPowerAppliedMode!=1 && wifiPowerLastError==ESP_FAIL);
  now+=5000; WiFi.sleepOk=true; serviceWifiPower(); assert(wifiPowerAppliedMode==1);
  // A changed access window renews its timer and changes the sleep boundary.
  saveWifiPowerMode(2);saveWifiAccessWindow(120);noteWifiPowerScanFinished();
  assert(wifiAccessWindowSeconds==120 && preferences.saved==120);
  now+=119999;serviceWifiPower();assert(!wifiRadioSleeping && wifiAccessWindowActive());
  now++;serviceWifiPower();assert(wifiRadioSleeping && !wifiAccessWindowActive());
  assert(wakeWifiRadio());wifiPowerWindow.arm(millis());
  assert(wifiPowerMode==2 && wifiPowerWindow.remaining(millis())==120000);
  now+=5000;saveWifiAccessWindow(5);now+=4999;serviceWifiPower();assert(!wifiRadioSleeping);
  now++;serviceWifiPower();assert(wifiRadioSleeping);
  std::cout << "Wi-Fi power policy/driver transition tests passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="surveyor-power-") as tmp:
    cpp = Path(tmp) / "power.cpp"
    binary = Path(tmp) / "power"
    cpp.write_text(stubs + state + services + tests)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
