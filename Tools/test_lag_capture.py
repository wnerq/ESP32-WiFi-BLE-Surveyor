"""Compile the actual bounded lag recorder with simulated time and radio state."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')
recorder = source[source.index('struct WebStallTraceRecord {'):source.index('void sampleWebResponseMemory() {')]
stubs = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using std::size_t;
const size_t WEB_STALL_TRACE_CAPACITY=8;
const uint32_t LAG_CAPTURE_THRESHOLD_MS=500;
uint32_t now=1000;
uint32_t millis(){return now;}
struct {uint32_t getFreeHeap(){return 12000;}} ESP;
uint32_t diagnosticLargestFreeBlock(){return 8000;}
bool wifiScanInProgress=false,bleDiagnosticScanActive=true,wifiRadioSleeping=false;
uint8_t wifiPowerMode=2;
struct {uint32_t remaining(uint32_t){return 30000;}} wifiPowerWindow;
'''
tests = r'''
int main(){
  captureRuntimeLag("LOOP","","loop-gap",499);
  assert(webStallTraceCount==0);
  captureRuntimeLag("HANDLER","/settings","finish",500);
  assert(webStallTraceCount==1);
  auto first=webStallTrace[0];
  assert(first.totalMs==500 && first.bleScanActive && first.powerMode==2);
  assert(first.radioWindowRemainingMs==30000 && first.freeHeapBytes==12000);
  assert(strcmp(first.route,"/settings")==0);
  captureRuntimeLag("HANDLER","/status.json","export",900);
  assert(webStallTraceCount==1); // Do not capture export into itself.
  wifiRadioSleeping=true;
  for(int i=0;i<20;i++){++now;captureRuntimeLag("LOOP","","loop-gap",700);}
  assert(webStallTraceCount==8 && webStallTraceSequence==21);
  auto last=webStallTrace[(webStallTraceNext+7)%8];
  assert(last.radioSleeping && last.radioWindowRemainingMs==0 && last.sequence==21);
  puts("PASS: lag threshold, context, export exclusion and ring rollover");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp, binary = Path(tmp)/'lag.cpp', Path(tmp)/'lag'
    cpp.write_text(stubs+recorder+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
