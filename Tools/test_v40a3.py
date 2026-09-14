"""Host regression tests using the actual recovery functions and terminal ring.

Run with Python 3 and g++ on PATH (e.g. `wsl --exec python3 Tools/test_v40a3.py`).
Radio/NVS/UART are fakes; hardware association, DHCP and RF coexistence still
require the bench and home-away-return tests in docs/TEST_V40A3.md.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")


def function(signature):
    start = source.index(signature + " {")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
using std::min;
bool wifiRadioSleeping=false;
uint8_t wifiPowerMode=0;
// LED instrumentation has its own waveform/integration tests; these no-op
// sinks keep the recovery regression focused on unchanged radio policy.
void ledDiagService(){}
void ledDiagSerialRx(){}
void ledDiagUpdateInfraState(){}
void ledDiagInfraReconnected(const char*){}
void ledDiagInfraAttempt(const char*){}
class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& s) : std::string(s) {}
  template<class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
  String(T value) : std::string(std::to_string(value)) {}
};
class Print { public: virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t*, size_t)=0; virtual ~Print()=default; };
class Stream : public Print { public: virtual int available()=0;
  virtual int availableForWrite()=0; virtual int read()=0; virtual int peek()=0;
  virtual void flush()=0; };
struct SerialFake {
  std::string text;
  void begin(unsigned long) {} int available(){return 0;}
  int availableForWrite(){return 128;} int read(){return -1;} int peek(){return -1;}
  void flush(){} size_t write(const uint8_t*,size_t n){return n;}
  template<class T> void print(T v){text+=String(v);}
  template<class T> void println(T v){text+=String(v)+"\n";}
  void println(){text+='\n';}
} Serial;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
uint32_t nowMs=0;
uint32_t millis(){return nowMs;}
const int WL_CONNECTED=3, WIFI_SCAN_RUNNING=-1, WIFI_IF_STA=0;
const int WIFI_REASON_ASSOC_LEAVE=8;
using WiFiEvent_t=int;
const int ARDUINO_EVENT_WIFI_STA_DISCONNECTED=1, ARDUINO_EVENT_WIFI_STA_CONNECTED=2;
struct WiFiEventInfo_t { struct { uint16_t reason; } wifi_sta_disconnected; };
struct Ip { uint32_t value; operator uint32_t() const{return value;}
  String toString() const{return value?"192.0.2.2":"0.0.0.0";}
  operator String() const{return toString();} };
struct WifiFake {
  struct Station { bool associated=false; bool connected(){return associated;} } STA;
  bool ip=false, driverScan=false;
  int status(){return ip?WL_CONNECTED:0;}
  Ip localIP(){return {ip?1u:0u};}
  int scanComplete(){return driverScan?WIFI_SCAN_RUNNING:0;}
  String SSID(int =0){return "home";}
  int RSSI(int){return -47;} int channel(int){return 6;}
  const uint8_t* BSSID(int){static uint8_t b[6]={1,2,3,4,5,6};return b;}
} WiFi;
using esp_err_t=int;
const int ESP_OK=0;
struct wifi_config_t { struct {
  uint8_t ssid[32]={}, password[64]={}; bool bssid_set=false; int channel=0;
} sta; };
int driverCalls=0, disconnectCalls=0, driverError=0;
wifi_config_t lastConfig;
esp_err_t esp_wifi_get_config(int,wifi_config_t*){return driverError;}
esp_err_t esp_wifi_set_config(int,wifi_config_t* config){lastConfig=*config;return driverError;}
esp_err_t esp_wifi_connect(){++driverCalls;return driverError;}
esp_err_t esp_wifi_disconnect(){++disconnectCalls;WiFi.STA.associated=false;return ESP_OK;}
bool wifiScanInProgress=false,bleDiagnosticScanActive=false,csvExportInProgress=false;
bool initialWifiScanPending=false, interaction=false, credentials=true;
uint32_t lastAutoScanMs=0;
unsigned long scanIntervalSeconds=300;
bool userInteractionDeferActive(){return interaction;}
bool loadCredentials(String& s,String& p){s=credentials?"home":"";p="test-secret";return credentials;}
void formatBssid(const uint8_t*,char* target){strcpy(target,"01:02:03:04:05:06");}
void recordDiagnosticEvent(const char* category,const String& detail){Serial.print(category);Serial.println(detail);}
std::vector<uint8_t> persisted;
int persistentWrites=0;
struct Preferences {
  bool begin(const char*,bool){return true;} void end(){}
  size_t getBytesLength(const char*){return persisted.size();}
  size_t getBytes(const char*,void* out,size_t n){memcpy(out,persisted.data(),n);return n;}
  size_t putBytes(const char*,const void* data,size_t n){
    const auto* bytes=static_cast<const uint8_t*>(data);
    persisted.assign(bytes,bytes+n);++persistentWrites;return n;
  }
};
'''

globals_start = source.index("bool nativeReconnectStateInitialized")
globals_end = source.index("void observeInfrastructureEvent(", globals_start)
mirror_start = source.index("class SurveySerialMirror")
mirror_end = source.index("SurveySerialMirror surveySerial;", mirror_start)
pieces = [STUBS, source[globals_start:globals_end], source[mirror_start:mirror_end],
          "void serviceNativeReconnectDiagnostics();"]
for name in ("WIFI_TIMEOUT_MS", "INFRA_NATIVE_GRACE_MS", "INFRA_RECONNECT_ATTEMPT_WINDOW_MS", "INFRA_RECONNECT_BACKOFF_MS"):
    pieces.append(re.search(r"const [^;\n]+\b" + name + r"\b[^;]+;", source)[0])
for signature in (
    "void observeInfrastructureEvent(WiFiEvent_t event, WiFiEventInfo_t info)",
    "bool infrastructureHasIp()",
    "void loadInfrastructureRecoverySummary()",
    "void saveInfrastructureRecoverySummary()",
    "void serviceNativeReconnectDiagnostics()",
    "void considerInfrastructureReconnectAfterScan(int networkCount)",
    "uint32_t infrastructureBackoffRemainingMs()",
    "void finishInfrastructureAttempt(const char* result)",
    "void cancelInfrastructureReconnect(const char* reason)",
    "void serviceInfrastructureReconnect()",
):
    pieces.append(function(signature))

TESTS = r'''
void seen(){considerInfrastructureReconnectAfterScan(1);lastAutoScanMs=nowMs;}
void tick(uint32_t ms){nowMs+=ms;serviceInfrastructureReconnect();}
void testRing(){
  SurveySerialMirror ring;
  std::vector<uint8_t> input(10000);
  for(size_t i=0;i<input.size();++i)input[i]=i%251;
  ring.write(input.data(),input.size());
  uint64_t cursor=0;uint8_t bytes[1024];bool dropped=false,more=false;
  size_t n=ring.snapshot(cursor,bytes,sizeof(bytes),dropped,more);
  assert(dropped && more && n==1024 && cursor==2832);
  assert(std::equal(bytes,bytes+n,input.begin()+1808));
  size_t total=n;
  while(more){n=ring.snapshot(cursor,bytes,sizeof(bytes),dropped,more);assert(!dropped);total+=n;}
  assert(total==8192 && cursor==10000);
  assert(ring.snapshot(cursor,bytes,sizeof(bytes),dropped,more)==0 && !more);
  cursor=999999;ring.snapshot(cursor,bytes,sizeof(bytes),dropped,more);assert(dropped);
}
int main(){
  testRing();
  // No credentials / no evidence / native grace must not issue radio calls.
  credentials=false;seen();tick(30000);assert(driverCalls==0);
  credentials=true;considerInfrastructureReconnectAfterScan(0);tick(1);assert(driverCalls==0);
  WiFi.ip=true;serviceInfrastructureReconnect();WiFi.ip=false;seen();tick(19999);assert(driverCalls==0);
  wifiScanInProgress=true;tick(2);assert(driverCalls==0);
  wifiScanInProgress=false;WiFi.driverScan=true;tick(1);assert(driverCalls==0);
  WiFi.driverScan=false;bleDiagnosticScanActive=true;tick(1);assert(driverCalls==0);
  bleDiagnosticScanActive=false;csvExportInProgress=true;tick(1);assert(driverCalls==0);
  csvExportInProgress=false;interaction=true;tick(1);assert(driverCalls==0);
  interaction=false;lastAutoScanMs=nowMs-300000;tick(1);assert(driverCalls==0);
  lastAutoScanMs=nowMs;tick(1);assert(driverCalls==1 && infrastructureReconnectAttemptActive);
  assert(memcmp(lastConfig.sta.ssid,"home",4)==0 && memcmp(lastConfig.sta.password,"test-secret",11)==0);
  assert(Serial.text.find("test-secret")==std::string::npos);
  tick(14999);assert(infrastructureReconnectAttemptActive);
  tick(1);assert(!infrastructureReconnectAttemptActive && infrastructureAssociationFailures==1);
  assert(infrastructureBackoffRemainingMs()==10000);
  tick(10000);assert(driverCalls==1); // stale evidence cannot retrigger
  seen();tick(1);assert(driverCalls==2);
  // Association without IP has a distinct outcome.
  WiFi.STA.associated=true;tick(1);assert(std::string(infrastructureRecoveryState)=="WAITING_FOR_IP");
  tick(14999);assert(infrastructureIpFailures==1 && std::string(infrastructureLastRecovery.result)=="ip-timeout");
  assert(infrastructureBackoffRemainingMs()==30000);
  // Backoff progression is capped; a new scan cannot bypass it.
  for(uint32_t expected : {60000u,120000u,300000u,300000u}){
    seen();int before=driverCalls;uint32_t remaining=infrastructureBackoffRemainingMs();
    tick(remaining-1);assert(driverCalls==before);
    seen();tick(1);assert(driverCalls==before+1);
    tick(15000);assert(infrastructureBackoffRemainingMs()==expected);
  }
  // Full success clears stale visibility immediately, without another scan.
  tick(300000);seen();tick(1);WiFi.STA.associated=true;WiFi.ip=true;tick(1);
  assert(infrastructureReconnectSuccessCount==1 && !infrastructureVisibleDisconnectedActive);
  assert(!infrastructureReconnectPending && !infrastructureReconnectAttemptActive && infrastructureBackoffStep==0);
  assert(infrastructureLastRecovery.visibleScans>0 && infrastructureLastRecovery.seen);
  // Native success and user success must not be counted as app success.
  uint32_t nativeBefore=nativeReconnectObservedCount;
  WiFi.ip=false;WiFi.STA.associated=false;seen();WiFi.ip=true;tick(1);
  assert(nativeReconnectObservedCount==nativeBefore+1 && infrastructureLastRecovery.nativeRecovered);
  WiFi.ip=false;seen();infrastructureUserRecoveryPending=true;WiFi.ip=true;tick(1);
  assert(nativeReconnectObservedCount==nativeBefore+1 && infrastructureReconnectSuccessCount==1);
  assert(std::string(infrastructureLastRecovery.source)=="USER_REQUEST");
  // Pending evidence is revoked by a completed scan that no longer sees home.
  WiFi.ip=false;seen();considerInfrastructureReconnectAfterScan(0);tick(20001);
  assert(!infrastructureReconnectAttemptActive);
  // Manual scan cancels bounded app work; raw RF disconnect reason survives cancellation.
  seen();tick(1);assert(infrastructureReconnectAttemptActive);
  observeInfrastructureEvent(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,{{201}});tick(1);
  cancelInfrastructureReconnect("manual-scan");
  observeInfrastructureEvent(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,{{WIFI_REASON_ASSOC_LEAVE}});tick(1);
  assert(!infrastructureReconnectAttemptActive && infrastructureLastRecovery.disconnectReason==201);
  // Unsigned elapsed-time handling works across millis rollover.
  WiFi.ip=true;tick(1);nowMs=UINT32_MAX-10000;WiFi.ip=false;seen();tick(20001);
  assert(infrastructureReconnectAttemptActive);
  // Immediate driver errors terminate and back off without trapping the scheduler.
  cancelInfrastructureReconnect("manual-scan");tick(10000);driverError=42;seen();tick(1);
  assert(!infrastructureReconnectAttemptActive && std::string(infrastructureLastRecovery.result)=="start-failed");
  // Persist a bounded summary only when dirty; retain it independently of live counters.
  saveInfrastructureRecoverySummary();assert(persistentWrites==1);
  uint32_t attempts=infrastructureLastRecovery.appAttempts;
  infrastructureLastRecovery={};loadInfrastructureRecoverySummary();
  assert(infrastructurePreviousRecovery.appAttempts==attempts);
  assert(std::string(infrastructurePreviousRecovery.result)=="start-failed");
  saveInfrastructureRecoverySummary();assert(persistentWrites==1);
  persisted[0]=99;loadInfrastructureRecoverySummary();
  assert(std::string(infrastructurePreviousRecovery.result)=="none");
  // Scheduled power cycles must not become reconnect attempts or lost-IP evidence.
  wifiPowerMode=2;wifiRadioSleeping=true;WiFi.ip=false;
  uint32_t oldAttempts=infrastructureReconnectAttemptCount;
  uint32_t oldVisible=infrastructureSavedNetworkSeenDisconnectedScanCount;
  observeInfrastructureEvent(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,{{201}});
  tick(300000);assert(!infrastructureDisconnectActive);
  assert(std::string(infrastructureRecoveryState)=="RADIO_SLEEP");
  wifiRadioSleeping=false;seen();tick(20001);
  assert(!infrastructureReconnectPending && !infrastructureReconnectAttemptActive);
  assert(infrastructureReconnectAttemptCount==oldAttempts);
  assert(infrastructureSavedNetworkSeenDisconnectedScanCount==oldVisible);
  assert(!infrastructureVisibleDisconnectedActive);
  std::cout << "PASS: terminal ring and recovery grace, visibility, radio guards, timeouts, backoff, attribution, cancellation, rollover, driver errors\n";
}
'''

with tempfile.TemporaryDirectory(prefix="surveyor-v40a3-") as temp:
    cpp = Path(temp) / "regression.cpp"
    binary = Path(temp) / "regression"
    cpp.write_text("\n".join(pieces) + TESTS, encoding="utf-8")
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
