"""Round-trip the actual V45 checkpoint functions against an in-memory file."""
import subprocess
import tempfile
from pathlib import Path
import test_shared_storage as core

extra = r'''
#include <vector>
const int FILE_WRITE=1,FILE_READ=0;
std::vector<uint8_t> disk;
bool exists=false;
struct File {
  size_t pos=0;
  operator bool()const{return true;}
  size_t write(const uint8_t* p,size_t n){if(pos+n>disk.size())disk.resize(pos+n);memcpy(disk.data()+pos,p,n);pos+=n;return n;}
  int read(uint8_t* p,size_t n){n=std::min(n,disk.size()-pos);memcpy(p,disk.data()+pos,n);pos+=n;return (int)n;}
  bool seek(size_t p){pos=p;return p<=disk.size();}
  size_t size(){return disk.size();}
  size_t position(){return pos;}
  void close(){}
};
struct {
  File open(const char*,int mode){if(mode==FILE_WRITE){disk.clear();::exists=true;}return {};}
  bool exists(const char*){return ::exists;}
  bool remove(const char*){::exists=false;return true;}
} SPIFFS;
bool spiffsMounted=true;
bool bleSurveyEnabled=false,diagnosticStreamingEnabled=false,diagnosticCheckpointEvents=false;
bool sessionRestoredThisBoot=false;
String sessionCheckpointStatus;
uint32_t sessionUptimeOffsetMs=0;
uint32_t millis(){return 1000;}
uint32_t surveySessionUptimeMs(){return millis()+sessionUptimeOffsetMs;}
const char* SESSION_CHECKPOINT_PATH="checkpoint";
const uint32_t SESSION_CHECKPOINT_MAGIC=0x53565233;
const uint16_t SESSION_CHECKPOINT_VERSION=4;
size_t bleHistoryCount=0,bleAddressCount=0,bleScanMetadataCapacity=0,bleAddressTableCapacity=0,bleHistoryRetentionLimit=0;
BleObservation* bleHistory=nullptr;
BleAddressEntry* bleAddressTable=nullptr;
SurveyScanMetadata* bleScanMetadata=nullptr;
void clearBleHistory(){}
void appendBleObservation(const BleObservation&){}
const BleObservation& compactBleHistoryRecord(size_t){static BleObservation x;return x;}
void recordDiagnosticEvent(const char*,const String&){}
'''
functions = '\n'.join(core.block(s) for s in core.signatures)
checkpoint = '\n'.join(core.block(s) for s in [
    'uint32_t crc32Update(', 'bool checkpointWriteChunk(',
    'bool saveSurveySessionCheckpoint(String& detail) {',
    'bool restoreSurveySessionCheckpoint(String& detail) {'])
helpers = core.tests[:core.tests.index('int main()')]
test = r'''
int main(){
  String detail;
  for(uint8_t mode: {0,1,2}) {
    reset(mode);
    for(unsigned i=0;i<40;i++)sight(ap(i),-70);
    wifiScanMetadata[0]={2,200};scanCounter=2;
    sight(0,-30);
    auto count=historyCount;
    assert(saveSurveySessionCheckpoint(detail));
    auto saved=disk;
    reset(mode);assert(wifiInventoryMode() || wifiApTableCapacity==16);
    assert(restoreSurveySessionCheckpoint(detail));
    assert(historyCount==count && wifiApCount==40);
    assert(wifiApTable[0].signal.samples==2 && wifiApTable[0].signal.rssiTotal==-100);
    assert(!wifiRestoringCheckpoint && !exists);
    if(mode==2)assert(historyRecord(0).uptimeMs==200);
    // CRC failure must not clear live history.
    disk=saved;exists=true;disk.back()^=1;
    assert(!restoreSurveySessionCheckpoint(detail));assert(historyCount==count);
    disk=saved;
    surveyFocus=(mode+1)%3;
    assert(!restoreSurveySessionCheckpoint(detail));
  }
  free(wifiStoragePool);
  puts("PASS: three-profile checkpoint round trips, AP growth restore, inventory statistics, CRC and focus rejection");
}
'''
types = core.types + '\n' + '\n'.join(core.block('struct '+n+' {')+';' for n in ['BleObservation','BleAddressEntry','SessionCheckpointHeader'])
with tempfile.TemporaryDirectory() as tmp:
    cpp,binary=Path(tmp)/'checkpoint.cpp',Path(tmp)/'checkpoint'
    state = core.state.replace("uint32_t surveySessionUptimeMs(){return scanCounter*100;}", "uint32_t surveySessionUptimeMs();")
    cpp.write_text(core.stubs+types+state+extra+functions+checkpoint+helpers+test)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
