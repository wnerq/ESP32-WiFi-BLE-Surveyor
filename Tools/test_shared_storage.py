"""Exercise actual V45 pool allocation, relocation, eviction and inventory code."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')
def block(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

types = '\n'.join(block('struct '+name+' {')+';' for name in
                  ['SignalStats','WifiApEntry','WifiObservation','SurveyScanMetadata','ScanRecord'])
stubs = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using std::size_t;
struct String: std::string {
  using std::string::string;
  String(size_t n):std::string(std::to_string(n)){}
  String(const std::string& s):std::string(s){}
  void toCharArray(char* p,size_t n)const{snprintf(p,n,"%s",c_str());}
};
struct {template<class T>void print(T){} template<class T>void println(T){}} Serial;
constexpr size_t MIN_SCAN_HISTORY_RECORDS=50,MAX_SCAN_HISTORY_RECORDS=12000;
uint8_t surveyFocus=1;
bool wifiInventoryMode(){return surveyFocus==2;}
'''
state = r'''
using WifiScanMetadata=SurveyScanMetadata;
uint8_t* wifiStoragePool=nullptr;
size_t wifiStoragePoolBytes=0,wifiApPolicyLimit=0;
uint32_t wifiPoolGrowthCount=0,scanCounter=1,lastScanUptimeMs=0;
bool wifiRestoringCheckpoint=false;
WifiApEntry* wifiApTable=nullptr;
WifiObservation* scanHistory=nullptr;
WifiScanMetadata* wifiScanMetadata=nullptr;
size_t wifiApTableCapacity=0,wifiApCount=0,scanHistoryCapacity=0,scanHistoryRetentionLimit=0;
size_t wifiScanMetadataCapacity=0,historyStart=0,historyCount=0;
uint32_t wifiApTableFullDrops=0,wifiApReclamationCount=0;
String historyResizeMessage;
bool growWifiApTable(size_t);
bool wifiScanInProgress=false,csvExportInProgress=false;
uint32_t syntheticPrefillRuns=0;
size_t syntheticPrefillLastTarget=0;
constexpr uint8_t WIFI_AUTH_OPEN=0;
uint32_t surveySessionUptimeMs(){return scanCounter*100;}

'''
signatures = [
    'const WifiObservation& compactHistoryRecord(size_t logicalIndex) {',
    'void formatBssid(', 'ScanRecord historyRecord(size_t logicalIndex) {',
    'void discardOldestWifiObservation() {',
    'void discardObservationsForScanSlot(uint16_t scanSlot) {',
    'size_t wifiHistoryIntegrityAnomalies() {',
    'void appendWifiObservation(const WifiObservation& observation) {',
    'void clearScanHistory() {',
    'int findWifiApByBssid(const uint8_t bssid[6]) {',
    'bool wifiApIndexIsReferenced(size_t apIndex) {',
    'int findOrCreateWifiAp(\n',
    'bool growWifiApTable(size_t requested) {',
    'bool initializeCompactWifiHistory(size_t budgetBytes, size_t apCapacity, size_t scanCapacity) {',
    'bool prefillWifiHistoryToPercent(uint8_t percent, String& detail) {',
]
tests = r'''
void reset(uint8_t focus){
  free(wifiStoragePool);wifiStoragePool=nullptr;surveyFocus=focus;
  assert(initializeCompactWifiHistory(32768,256,512));
  scanCounter=1;wifiScanMetadata[0]={1,100};
}
int ap(unsigned n){
  uint8_t mac[6]={2,0,0,0,(uint8_t)(n>>8),(uint8_t)n};
  return findOrCreateWifiAp(mac,"Test",6,0,nullptr,nullptr);
}
void sight(int index,int8_t rssi=-50){
  assert(index>=0);WifiObservation o={};o.apIndex=index;o.scanSlot=0;o.rssi=rssi;appendWifiObservation(o);
}
int main(){
  reset(1);auto pool=wifiStoragePool;size_t initial=scanHistoryCapacity;
  for(int i=0;i<16;i++)sight(ap(i));
  // Wrap a full ring before AP-table relocation, using an independent oracle.
  for(size_t i=0;i<initial+40;i++)sight(0,-40-(i%30));
  std::string expected;
  for(size_t i=0;i<historyCount;i++)expected+=(char)compactHistoryRecord(i).rssi;
  assert(ap(16)==16);assert(wifiStoragePool==pool && scanHistoryCapacity<initial);
  expected=expected.substr(expected.size()-historyCount);
  for(size_t i=0;i<historyCount;i++)assert((char)compactHistoryRecord(i).rssi==expected[i]);
  assert(wifiHistoryIntegrityAnomalies()==0);
  assert(wifiApTable[0].signal.samples>initial);
  // Fill all protected AP slots with live references; admission must not retarget them.
  clearScanHistory();
  for(size_t i=0;i<wifiApPolicyLimit;i++)sight(ap(i));
  int admitted=ap(9999);assert(admitted>=0);
  for(size_t i=0;i<historyCount;i++)assert(compactHistoryRecord(i).apIndex!=admitted);
  sight(admitted);
  for(size_t i=0;i<historyCount;i++)assert(compactHistoryRecord(i).apIndex<wifiApCount);
  size_t balancedLimit=wifiApPolicyLimit;
  reset(0);assert(wifiApPolicyLimit<balancedLimit);
  for(size_t i=0;i<wifiApPolicyLimit;i++)sight(ap(i));
  assert(ap(9999)==-1);
  clearScanHistory();assert(wifiApTableCapacity==16);
  reset(2);size_t inventoryLimit=wifiApTableCapacity;assert(inventoryLimit>balancedLimit);
  int first=ap(0);sight(first,-80);wifiScanMetadata[0]={2,200};sight(first,-40);
  assert(historyCount==1 && wifiApTable[0].signal.samples==2);
  assert(wifiApTable[0].signal.minRssi==-80 && wifiApTable[0].signal.maxRssi==-40);
  assert(wifiApTable[0].signal.rssiTotal==-120);
  assert(historyRecord(0).uptimeMs==200 && historyRecord(0).rssi==-40);
  scanCounter=3;wifiScanMetadata[0]={3,300};
  for(size_t i=1;i<inventoryLimit;i++)sight(ap(i));
  scanCounter=4;wifiScanMetadata[0]={4,400};sight(ap(9999));
  assert(historyCount==inventoryLimit && wifiApTable[0].signal.samples==1);
  assert(wifiApTable[0].bssid[4]==(9999>>8));
  discardObservationsForScanSlot(0);assert(historyCount==inventoryLimit);
  assert(wifiHistoryIntegrityAnomalies()==0);
  String detail;
  for(uint8_t focus:{0,1,2}) {
    reset(focus);
    sight(ap(65000)); // Keep a real-looking network alongside synthetic data.
    for(uint8_t percent:{50,75,95,99}) {
      assert(prefillWifiHistoryToPercent(percent,detail));
      assert(historyCount==scanHistoryRetentionLimit*percent/100);
      assert(wifiHistoryIntegrityAnomalies()==0);
      auto before=historyCount;auto scans=scanCounter;
      assert(prefillWifiHistoryToPercent(percent,detail));
      assert(historyCount==before && scanCounter==scans);
    }
    assert(!prefillWifiHistoryToPercent(100,detail));
    wifiScanInProgress=true;assert(!prefillWifiHistoryToPercent(99,detail));wifiScanInProgress=false;
    csvExportInProgress=true;assert(!prefillWifiHistoryToPercent(99,detail));csvExportInProgress=false;
    if(focus==2) {
      assert(historyCount>256);
      for(size_t i=1;i<wifiApCount;++i) {
        assert(wifiApTable[i].signal.samples==1);
        assert(strncmp(wifiApTable[i].ssid,"TEST-PREFILL-",13)==0);
        for(size_t j=0;j<i;++j)assert(memcmp(wifiApTable[i].bssid,wifiApTable[j].bssid,6)!=0);
      }
    }
  }
  // A single metadata slot must not clamp the target or loop indefinitely.
  for(uint8_t focus:{0,1}) {
    free(wifiStoragePool);wifiStoragePool=nullptr;surveyFocus=focus;
    assert(initializeCompactWifiHistory(32768,16,1));scanCounter=0;
    assert(prefillWifiHistoryToPercent(99,detail));
    assert(historyCount==scanHistoryRetentionLimit*99/100);
    assert(wifiHistoryIntegrityAnomalies()==0);
  }
  free(wifiStoragePool);
  puts("PASS: shared pool growth, wrapped-ring relocation, policy admission, inventory updates and LRU replacement");
}
'''
if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        cpp,binary=Path(tmp)/'storage.cpp',Path(tmp)/'storage'
        cpp.write_text(stubs+types+state+'\n'.join(block(s) for s in signatures)+tests)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
