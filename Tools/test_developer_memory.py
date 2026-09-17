"""Host checks for memory-setting config round trips and contextual help links."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/main.cpp').read_text(encoding='utf-8')

def through(signature, ending):
    start = source.index(signature)
    return source[start:source.index(ending, start)]

stubs = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <iostream>
class String: public std::string {
public:
  using std::string::string;
  String(const std::string& s):std::string(s){}
  template<class T, std::enable_if_t<std::is_arithmetic_v<T>,int> = 0>
  String(T n):std::string(std::to_string(n)){}
  String substring(size_t a,size_t b)const{return substr(a,b-a);}
  long toInt()const{return strtol(c_str(),nullptr,10);}
  void trim(){auto a=find_first_not_of(" \r\n\t"); if(a==npos){clear();return;} *this=substr(a,find_last_not_of(" \r\n\t")-a+1);}
};
const unsigned long MIN_SCAN_INTERVAL_SECONDS=5,MAX_SCAN_INTERVAL_SECONDS=3600;
const size_t DIAGNOSTIC_EVENT_CAPACITY=32;
String normalizedMdnsHostname(String s){return s;}
bool isValidMdnsHostname(const String& s){return !s.empty();}
String jsonQuoted(const String& s){return String("\"")+s+"\"";}
void markExplicitUserInteraction(){}
struct Server {
  String bytes,plots,location;int code=0;
  String arg(const char* key){return std::string(key)=="terminalBytes"?bytes:plots;}
  void send(int n,const char*,const char*){code=n;}
  void sendHeader(const char*,const char* value){location=value;}
} server;
struct Prefs {
  bool available=true,failWrite=false;unsigned writes=0;uint32_t bytes=0;bool plots=false;
  bool begin(const char*,bool){return available;}
  size_t putUInt(const char*,uint32_t n){++writes;bytes=n;return failWrite?0:sizeof(n);}
  size_t putBool(const char*,bool b){++writes;plots=b;return failWrite?0:sizeof(b);}
  void end(){}
} preferences;
'''
schema = re.search(r'const [^;\n]+ CONFIG_SCHEMA_VERSION[^;]+;', source)[0]
types = through('struct PortableConfig {', '// Explicit prototypes')
validator = through('bool validTerminalBufferBytes(', '\n\nvoid recordDiagnosticEvent')
exporter = through('String portableConfigJson(const PortableConfig& c) {', '// Purpose: Streams the portable')
parser = through('class FlatConfigJsonParser {', 'String configImportResultJson(')
handler = through('void handleDeveloperMemorySettings() {', 'void handleSettingsPage() {')
tests = r'''
bool parse(String text,PortableConfig& out){FlatConfigJsonParser p(text);return p.parse(out);}
int main(){
  PortableConfig c={};c.wifiScanIntervalSeconds=c.bluetoothScanIntervalSeconds=300;
  c.wifiAccessWindowSeconds=60;c.mdnsHostnameAutomatic=c.accessPointSSIDAutomatic=true;
  for(unsigned long size:{0ul,1024ul,2048ul,8192ul,16384ul,32768ul}) {
    for(bool plots:{false,true}) {
      c.terminalBufferBytes=size;c.plotsEnabled=plots;
      auto json=portableConfigJson(c);PortableConfig out={};
      assert(parse(json,out));assert(out.terminalBufferBytes==size && out.plotsEnabled==plots);
    }
  }
  auto json=portableConfigJson(c);
  auto change=[&](const char* key,const char* value){
    String text=json;auto a=text.find(key);assert(a!=String::npos);a=text.find(':',a)+1;
    text.replace(a,text.find(',',a)-a,value);return text;
  };
  PortableConfig out=c;
  for(const char* value:{"-1","1","4096","65536","4294967296","1.5","true","\"1024\""})
    assert(!parse(change("terminalBufferBytes",value),out));
  for(const char* value:{"0","1","\"false\"","null"})assert(!parse(change("plotsEnabled",value),out));
  for(const char* duplicate:{"\"terminalBufferBytes\":0,","\"plotsEnabled\":false,"}){
    String text=json;text.insert(1,duplicate);assert(!parse(text,out));
  }
  // Old backups preserve the current settings when the optional keys are absent.
  for(const char* key:{"terminalBufferBytes","plotsEnabled"}){
    auto a=json.find(std::string("  \"")+key);assert(a!=String::npos);
    json.erase(a,json.find('\n',a)-a+1);
  }
  out.terminalBufferBytes=2048;out.plotsEnabled=false;
  assert(parse(json,out));assert(out.terminalBufferBytes==2048 && !out.plotsEnabled);
  // Invalid forms never write preferences; supported choices persist for next boot.
  for(const char* invalid:{"","abc","-1","1024junk","4096","65536"}){
    auto writes=preferences.writes;server.bytes=invalid;server.plots="0";
    handleDeveloperMemorySettings();assert(server.code==400 && preferences.writes==writes);
  }
  server.bytes="1024";server.plots="true";handleDeveloperMemorySettings();assert(server.code==400);
  for(const char* size:{"0","1024","2048","8192","16384","32768"}){
    server.bytes=size;server.plots="1";handleDeveloperMemorySettings();
    assert(server.code==303 && preferences.bytes==strtoul(size,nullptr,10) && preferences.plots);
    assert(server.location=="/settings#developer-memory");
  }
  preferences.available=false;handleDeveloperMemorySettings();assert(server.code==500);
  preferences.available=true;preferences.failWrite=true;handleDeveloperMemorySettings();assert(server.code==500);
  std::cout << "PASS: developer config sizes, booleans, duplicates, overflow and old backups\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp, binary = Path(tmp)/'settings.cpp', Path(tmp)/'settings'
    cpp.write_text(stubs+schema+types+validator+exporter+parser+handler+tests)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Wno-misleading-indentation',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

start = source.index('String contextHelpScript()')
help_script = source[start:source.index(')rawliteral";',start)]
anchors = set(re.findall(r"\['([a-z-]+)'", help_script))
sections = set(re.findall(r'"([a-z-]+)\|',source)) | {'serial-status-line'}
assert anchors <= sections, f'Missing help sections: {anchors-sections}'
assert {'developer-memory','survey-focus','wifi-power','serial-terminal','survey-button'} <= sections
print('PASS: every contextual help link has a destination, including new features')
