"""Run actual BLE prefill and compact storage with small metadata tables."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')

def block(signature):
    start = source.index(signature)
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]

stubs = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using std::size_t;
struct String:std::string {
  using std::string::string;
  String(size_t n):std::string(std::to_string(n)){}
  String(const std::string& s):std::string(s){}
  bool equals(const String& s)const{return *this==s;}
  void toCharArray(char* p,size_t n)const{snprintf(p,n,"%s",c_str());}
};
'''
types = '\n'.join(block('struct '+name+' {')+';' for name in
                  ['BleObservation','BleAddressEntry','SurveyScanMetadata'])
state = r'''
BleObservation* bleHistory=nullptr;
BleAddressEntry* bleAddressTable=nullptr;
SurveyScanMetadata* bleScanMetadata=nullptr;
size_t bleHistoryStart=0,bleHistoryCount=0,bleHistoryCapacity=12000,bleHistoryRetentionLimit=12000;
size_t bleAddressTableCapacity=32,bleAddressCount=0,bleScanMetadataCapacity=0;
size_t bleAddressPeakReferenced=0,bleScanMetadataPeakUsed=0;
uint32_t bleAddressTableFullDrops=0,bleScanCounter=0,lastBleScanUptimeMs=0;
bool bleSurveyEnabled=true,bleDiagnosticScanActive=false,csvExportInProgress=false;
uint32_t surveySessionUptimeMs(){return bleScanCounter*100;}
void appendBleObservation(const BleObservation&);
'''
functions = '\n'.join(block(signature) for signature in [
    'const BleObservation& compactBleHistoryRecord(size_t logicalIndex) {',
    'bool bleAddressIndexIsReferenced(size_t addressIndex) {',
    'size_t countReferencedBleAddresses() {',
    'size_t countReferencedBleScanSlots() {',
    'void updateBleUsageHighWaterMarks() {',
    'void discardOldestBleObservation() {',
    'void discardBleObservationsForScanSlot(uint16_t scanSlot) {',
    'void appendBleObservation(const BleObservation& observation) {',
    'void clearBleHistory() {',
    'int findBleAddress(const uint8_t address[6]) {',
    'int findOrCreateBleAddress(\n',
    'bool prefillBleHistoryToPercent(uint8_t percent, String& detail) {',
])
tests = r'''
int main(){
  std::vector<BleObservation> observations(bleHistoryCapacity);
  std::vector<BleAddressEntry> addresses(bleAddressTableCapacity);
  bleHistory=observations.data();bleAddressTable=addresses.data();String detail;
  for(size_t slots:{1u,2u,256u}) {
    std::vector<SurveyScanMetadata> metadata(slots);
    bleScanMetadata=metadata.data();bleScanMetadataCapacity=slots;clearBleHistory();
    for(uint8_t percent:{50,75,95,99}) {
      assert(prefillBleHistoryToPercent(percent,detail));
      assert(bleHistoryCount==bleHistoryRetentionLimit*percent/100);
      for(size_t i=0;i<bleHistoryCount;++i) {
        auto& o=compactBleHistoryRecord(i);
        assert(o.addressIndex<bleAddressCount && o.scanSlot<slots);
        assert(metadata[o.scanSlot].scanNumber!=0);
        assert(strncmp(addresses[o.addressIndex].name,"TEST-PREFILL-BLE-",17)==0);
      }
      auto scans=bleScanCounter;
      assert(prefillBleHistoryToPercent(percent,detail) && scans==bleScanCounter);
    }
    assert(!prefillBleHistoryToPercent(100,detail));
    bleSurveyEnabled=false;assert(!prefillBleHistoryToPercent(99,detail));bleSurveyEnabled=true;
    bleDiagnosticScanActive=true;assert(!prefillBleHistoryToPercent(99,detail));bleDiagnosticScanActive=false;
    csvExportInProgress=true;assert(!prefillBleHistoryToPercent(99,detail));csvExportInProgress=false;
  }
  puts("PASS: BLE 50/75/95/99% fill, metadata wrap, repeat fills and disabled/busy guards");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp,binary=Path(tmp)/'ble.cpp',Path(tmp)/'ble'
    cpp.write_text(stubs+types+state+functions+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
