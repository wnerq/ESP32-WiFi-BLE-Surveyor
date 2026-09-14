"""Compile the actual LED manager with simulated time/UART/GPIO.

Run: wsl --exec python3 Tools/test_led_diag.py
No ESP32 is accessed. These tests verify logic and waveform timing under regular
servicing, not electrical output or on-device loop-gap performance.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
enum = re.search(r"enum LedDiagEvent : uint8_t \{.*?\};", source, re.S)[0]
mirror = source[source.index("class SurveySerialMirror"):source.index("void recordDiagnosticEvent(const char*")]
manager = source[source.index("struct LedDiagPattern {"):source.index("// Serial helpers")]
restart = source[source.index("bool controlledRestartPending ="):source.index("// Purpose: Performs a controlled System-page restart")]

STUBS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
using std::min;
uint32_t nowMs=0, microCalls=0;
uint32_t millis(){return nowMs;}
uint32_t micros(){return nowMs*1000+(++microCalls);}
bool statusLedEnabled=true;
bool wifiRadioSleeping=false;
uint8_t wifiPowerMode=0;
const bool STATUS_LED_AVAILABLE=true, STATUS_LED_ACTIVE_HIGH=true;
const uint8_t STATUS_LED_PIN=2;
const int HIGH=1,LOW=0,OUTPUT=1,WIFI_MODE_STA=1;
bool pinOn=false;
struct Edge { uint32_t ms; bool on; };
std::vector<Edge> edges;
void pinMode(int pin,int mode){assert(pin==2 && mode==OUTPUT);}
void digitalWrite(int pin,int on){assert(pin==2);pinOn=on;edges.push_back({nowMs,bool(on)});}
struct WifiFake { int mode=1; int getMode(){return mode;} } WiFi;
bool hasIp=false;
bool infrastructureHasIp(){return hasIp;}
struct EspFake { int restarts=0; void restart(){++restarts;} } ESP;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
class Print {
 public:
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t*,size_t)=0;
  virtual ~Print()=default;
  void print(const char* s){write(reinterpret_cast<const uint8_t*>(s),strlen(s));}
  template<class T,std::enable_if_t<std::is_arithmetic_v<T>,int> =0>
  void print(T value){auto s=std::to_string(value);print(s.c_str());}
  template<class T> void println(T value){print(value);print("\n");}
};
class Stream:public Print {
 public: virtual int read()=0;virtual int available()=0;virtual int peek()=0;
  virtual int availableForWrite()=0;virtual void flush()=0;
};
struct Uart {
  std::deque<uint8_t> rx;std::string tx;
  void begin(unsigned long){} int available(){return rx.size();}
  int read(){if(rx.empty())return -1;int v=rx.front();rx.pop_front();return v;}
  int peek(){return rx.empty()?-1:rx.front();}
  int availableForWrite(){return 128;} void flush(){}
  size_t write(const uint8_t* b,size_t n){tx.append(reinterpret_cast<const char*>(b),n);return n;}
} physicalSerial;
#define Serial physicalSerial
'''

TESTS = r'''
void advance(uint32_t duration){
  assert(duration%5==0);
  for(uint32_t i=0;i<duration;i+=5){nowMs+=5;ledDiagService();}
}
void reset(bool boot=true){
  diagnosticLed=DiagnosticLedManager();ledDiagInfraConfigured=false;
  nowMs=0;microCalls=0;statusLedEnabled=true;hasIp=false;WiFi.mode=1;
  controlledRestartPending=false;ESP.restarts=0;
  physicalSerial.rx.clear();physicalSerial.tx.clear();
  diagnosticLed.begin();edges.clear();
  if(boot){ledDiagEvent(LED_EVENT_BOOT_COMPLETE);advance(970);assert(!pinOn);edges.clear();}
}
std::vector<uint32_t> onWidths(){
  std::vector<uint32_t> widths;uint32_t start=0;bool have=false;
  for(const auto& edge:edges){
    if(edge.on){assert(!have);start=edge.ms;have=true;}
    else if(have){widths.push_back(edge.ms-start);have=false;}
  }
  assert(!have);return widths;
}
void pattern(LedDiagEvent event,uint32_t duration,std::vector<uint32_t> expected){
  reset();ledDiagEvent(event);advance(duration);assert(onWidths()==expected);assert(!pinOn);
}
int main(){
  // No early boot signal; boot complete is short + long, exactly once.
  reset(false);ledDiagEvent(LED_EVENT_WEBPAGE);advance(1000);assert(edges.empty());
  ledDiagEvent(LED_EVENT_BOOT_COMPLETE);advance(970);
  assert((onWidths()==std::vector<uint32_t>{100,650}));
  edges.clear();ledDiagEvent(LED_EVENT_BOOT_COMPLETE);advance(1000);assert(edges.empty());
  pattern(LED_EVENT_WEBPAGE,100,{50});
  pattern(LED_EVENT_SERIAL_RX,150,{25,25});
  pattern(LED_EVENT_INFRA_RECONNECT_ATTEMPT,360,{60,60,60});
  pattern(LED_EVENT_INFRA_RECONNECTED,850,{750});
  pattern(LED_EVENT_CONTROLLED_REBOOT,660,{60,60,60,60,60});
  pattern(LED_EVENT_SELF_TEST,1830,{180,180,180,650});
  // Repeated traffic coalesces instead of extending a transient indefinitely.
  reset();ledDiagEvent(LED_EVENT_WEBPAGE);advance(25);ledDiagEvent(LED_EVENT_WEBPAGE);advance(75);
  assert((onWidths()==std::vector<uint32_t>{50}));
  // Preserve active Wi-Fi and BLE scan cadence and normal idle-off behavior.
  reset();ledDiagEvent(LED_EVENT_WIFI_SCAN);advance(450);ledDiagScanFinished();
  // The scan restarted at 450, then was stopped immediately: ignore that zero-width edge.
  auto widths=onWidths();assert(widths.size()==4 && widths[0]==75 && widths[1]==75 && widths[2]==75 && widths[3]==0);
  reset();ledDiagEvent(LED_EVENT_BLE_SCAN);advance(245);ledDiagScanFinished(true);
  assert((onWidths()==std::vector<uint32_t>{125}));
  // Heartbeat starts every 3 seconds; only state transitions are logged.
  reset();ledDiagSetInfraConfigured(true);uint32_t base=nowMs;
  advance(3400);assert((onWidths()==std::vector<uint32_t>{80,80,80,80}));
  assert(edges[4].ms-base==3000);
  const std::string logged=physicalSerial.tx;ledDiagUpdateInfraState();assert(physicalSerial.tx==logged);
  hasIp=true;ledDiagUpdateInfraState();assert(!pinOn);
  edges.clear();advance(3000);assert(edges.empty());
  hasIp=false;ledDiagUpdateInfraState();assert(pinOn);
  WiFi.mode=0;ledDiagUpdateInfraState();assert(!pinOn); // Intentionally disabled STA
  WiFi.mode=1;ledDiagUpdateInfraState();assert(pinOn);
  ledDiagSetInfraConfigured(false);assert(!pinOn);
  // Check each adjacent priority with observable pulse timing, not internal state.
  reset();ledDiagEvent(LED_EVENT_WIFI_SCAN);advance(10);ledDiagEvent(LED_EVENT_WEBPAGE);advance(50);
  assert((onWidths()==std::vector<uint32_t>{60}));
  reset();ledDiagEvent(LED_EVENT_WEBPAGE);advance(10);ledDiagEvent(LED_EVENT_SERIAL_RX);advance(25);
  assert((onWidths()==std::vector<uint32_t>{35}));
  reset();ledDiagEvent(LED_EVENT_SERIAL_RX);advance(10);ledDiagSetInfraDisconnected(true);advance(80);
  assert((onWidths()==std::vector<uint32_t>{90}));
  reset();ledDiagSetInfraDisconnected(true);advance(10);ledDiagInfraAttempt("SAVED_NETWORK_DISCOVERY");advance(60);
  assert((onWidths()==std::vector<uint32_t>{70}));
  reset();ledDiagInfraAttempt("SAVED_NETWORK_DISCOVERY");advance(10);ledDiagInfraReconnected("USER_REQUEST");advance(750);
  assert((onWidths()==std::vector<uint32_t>{760}));
  // Lower-priority activity cannot restart or interrupt the 750 ms success pulse.
  reset();ledDiagInfraReconnected("USER_REQUEST");advance(10);ledDiagInfraAttempt("SAVED_NETWORK_DISCOVERY");advance(740);
  assert((onWidths()==std::vector<uint32_t>{750}));
  // All requested overlap priorities; heartbeat resumes automatically afterward.
  reset();ledDiagEvent(LED_EVENT_WIFI_SCAN);advance(5);
  ledDiagEvent(LED_EVENT_WEBPAGE);advance(5);
  ledDiagEvent(LED_EVENT_SERIAL_RX);advance(5);
  ledDiagSetInfraDisconnected(true);advance(5);
  ledDiagInfraAttempt("SAVED_NETWORK_DISCOVERY");advance(5);
  ledDiagInfraReconnected("USER_REQUEST");advance(750);assert(!pinOn);
  // The persistent state is not cleared by an attempt, so it resumes after one.
  reset();ledDiagSetInfraDisconnected(true);advance(20);
  ledDiagInfraAttempt("SAVED_NETWORK_DISCOVERY");advance(360);assert(!pinOn);
  advance(2620);assert(pinOn);
  // Reboot preempts even an illuminated success pattern, with five visible flashes.
  reset();ledDiagEvent(LED_EVENT_INFRA_RECONNECTED);advance(100);
  requestControlledRestart(750);assert(!pinOn);edges.clear();
  for(int i=0;i<149;++i){advance(5);servicePendingControlledRestart();assert(ESP.restarts==0);}
  assert((onWidths()==std::vector<uint32_t>{60,60,60,60,60}));
  advance(5);servicePendingControlledRestart();assert(ESP.restarts==1);
  // LED-disabled setting never lights or hangs reboot; explicit self-test still works.
  reset();statusLedEnabled=false;ledDiagEvent(LED_EVENT_WEBPAGE);assert(!pinOn);
  ledDiagEvent(LED_EVENT_SELF_TEST);advance(1830);assert((onWidths()==std::vector<uint32_t>{180,180,180,650}));
  edges.clear();requestControlledRestart(500);advance(495);servicePendingControlledRestart();assert(ESP.restarts==0);
  advance(5);servicePendingControlledRestart();assert(ESP.restarts==1 && edges.empty());
  // A read, including a single character, triggers RX. TX/availability/empty reads do not.
  reset();Serial.println("STATUS periodic TX");Serial.available();Serial.read();advance(200);assert(edges.empty());
  physicalSerial.rx.push_back('x');assert(Serial.read()=='x');advance(150);
  assert((onWidths()==std::vector<uint32_t>{25,25}));
  // Human routes only; typical polls and diagnostic telemetry remain silent.
  for(const char* page:{"/","/scan","/ble","/system","/diagnostics","/terminal","/settings","/help"})assert(ledDiagIsHumanRequest(page,false));
  for(const char* action:{"/wifi-save","/restart-device","/api/wifi/interval","/api/live-updates"})assert(ledDiagIsHumanRequest(action,true));
  for(const char* poll:{"/api/wifi/status","/api/wifi/observed","/api/wifi/channel","/api/wifi/plot","/api/ble/status","/api/ble/observed","/api/terminal","/api/ping","/status.json","/api/web/client-diag"}){
    assert(!ledDiagIsHumanRequest(poll,false));assert(!ledDiagIsHumanRequest(poll,true));
  }
  // Planned radio-off periods do not request the disconnected heartbeat.
  reset();ledDiagSetInfraConfigured(true);hasIp=false;
  wifiPowerMode=2;ledDiagUpdateInfraState();edges.clear();advance(3500);
  assert(onWidths().empty());
  wifiPowerMode=0;wifiRadioSleeping=true;ledDiagUpdateInfraState();advance(3500);
  assert(onWidths().empty());wifiRadioSleeping=false;
  // millis wraparound and long service gaps never cause catch-up flash loops.
  reset();nowMs=UINT32_MAX-24;ledDiagEvent(LED_EVENT_SERIAL_RX);advance(150);
  assert((onWidths()==std::vector<uint32_t>{25,25}));
  reset();ledDiagEvent(LED_EVENT_CONTROLLED_REBOOT);nowMs+=10000;ledDiagService();
  assert(!diagnosticLed.rebootFinished());assert(diagnosticLed.maxServiceGapMs==10000);
  advance(600);assert(diagnosticLed.rebootFinished());
  assert(sizeof(DiagnosticLedManager)<=96);
  std::cout<<"PASS: boot, page, Wi-Fi/BLE scan, RX-only, heartbeat/configuration, attempt/success, priority/preemption, reboot, disabled/self-test, route exclusions, rollover and long gaps\n";
}
'''

# Structural guards complement waveform tests and catch future bypasses.
assert source.count("digitalWrite(STATUS_LED_PIN") == 1
assert "digitalWrite(STATUS_LED_PIN" in manager
assert source.count("ESP.restart();") == 1 and "ESP.restart();" in restart
assert not re.search(r"\b(?:delay|xTimer\w*|statusLedPulse)\s*\(", manager + restart)
assert not re.search(r"\bwhile\s*\(", manager + restart)
assert "ledDiagSerialRx()" in mirror and "if (value >= 0)" in mirror
assert "ledDiagInfraAttempt(\"SAVED_NETWORK_DISCOVERY\")" in source
assert "ledDiagInfraReconnected(source)" in source
assert "DIAGNOSTIC_EVENT_CAPACITY = 32" in source and "CAPACITY = 8192" in source

with tempfile.TemporaryDirectory(prefix="surveyor-led-") as temp:
    cpp, binary = Path(temp) / "led.cpp", Path(temp) / "led"
    declarations = "void ledDiagService(); void ledDiagSerialRx();\n"
    cpp.write_text(STUBS + enum + declarations + mirror + manager + restart + TESTS, encoding="utf-8")
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
