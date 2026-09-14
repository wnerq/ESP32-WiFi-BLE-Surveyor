"""Host tests for the actual command mailbox, Wi-Fi wizard and window parser."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")

def function(signature):
    start = source.index(signature + " {")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

stubs = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <iostream>
class String:public std::string {
public:
  using std::string::string;
  String()=default;
  String(const std::string& s):std::string(s){}
  void toCharArray(char* dst,size_t n)const{snprintf(dst,n,"%s",c_str());}
};
char pendingTerminalCommand[193]={};
bool terminalCommandPending=false,controlledRestartPending=false;
bool wifiScanInProgress=false,bleDiagnosticScanActive=false;
uint8_t webWifiConfigStage=0;
String webWifiConfigSsid;
uint32_t webWifiConfigStartedMs=0,now=0;
uint32_t millis(){return now;}
constexpr int LED_EVENT_WEBPAGE=1;
void ledDiagEvent(int){}
struct SerialFake { std::string text; void println(const char* s){text+=s;text+='\n';} } Serial;
struct ServerFake {
  String body,headerValue="1"; bool bodyPresent=true; int status=0;
  void sendHeader(const char*,const char*){}
  String header(const char*){return headerValue;}
  bool hasArg(const char*){return bodyPresent;}
  String arg(const char*){return body;}
  void send(int code,const char*,const char*){status=code;}
} server;
int executed=0,connections=0,saves=0;
String lastCommand;
void executeTerminalCommand(String command,bool web){
  assert(web);++executed;lastCommand=command;
  if(command=="wifi-config"){webWifiConfigStage=1;webWifiConfigStartedMs=now;}
}
bool connectToWiFi(const String& ssid,const String& password){
  assert(ssid=="Test Network");assert(password=="secretpass" || password.empty());
  ++connections;return true;
}
void saveCredentials(const String&,const String&){++saves;}
'''
tests = r'''
void submit(const char* text){server.body=text;server.bodyPresent=true;handleTerminalCommand();}
int main(){
  uint32_t seconds=42;
  for(const char* bad:{"","0","4","3601","-1","+60","60s","60.0"," 60","999999999"}){
    assert(!parseWifiAccessWindow(bad,seconds));assert(seconds==42);
  }
  for(const char* good:{"5","60","3600"})assert(parseWifiAccessWindow(good,seconds));
  server.headerValue="";submit("restart");assert(server.status==403 && !terminalCommandPending);
  server.headerValue="1";submit("wifi on\nrestart");assert(server.status==400);
  server.body=String(193,'x');handleTerminalCommand();assert(server.status==413);
  submit("wifi on");assert(server.status==202 && executed==0);
  submit("restart");assert(server.status==409);
  wifiScanInProgress=true;serviceTerminalCommand();assert(executed==0);
  wifiScanInProgress=false;serviceTerminalCommand();assert(executed==1 && lastCommand=="wifi on");
  assert(!terminalCommandPending && pendingTerminalCommand[0]==0);
  serviceTerminalCommand();assert(executed==1);
  controlledRestartPending=true;submit("help");assert(server.status==409);controlledRestartPending=false;
  submit("wifi-config");serviceTerminalCommand();assert(webWifiConfigStage==1);
  submit("Test Network");serviceTerminalCommand();assert(webWifiConfigStage==2);
  submit("secretpass");serviceTerminalCommand();assert(connections==1 && saves==1);
  assert(Serial.text.find("secretpass")==std::string::npos && webWifiConfigSsid.empty());
  // Empty bodies are accepted for an open-network password only.
  submit("wifi-config");serviceTerminalCommand();submit("Test Network");serviceTerminalCommand();
  server.body="";server.bodyPresent=false;handleTerminalCommand();assert(server.status==202);
  serviceTerminalCommand();assert(connections==2);
  // An expired password prompt never echoes late secrets as unknown commands.
  submit("wifi-config");serviceTerminalCommand();submit("Test Network");serviceTerminalCommand();
  now+=120000;serviceTerminalCommand();assert(webWifiConfigStage==3);
  int before=executed;submit("late-secret");serviceTerminalCommand();assert(executed==before);
  assert(Serial.text.find("late-secret")==std::string::npos);
  submit("cancel");serviceTerminalCommand();assert(webWifiConfigStage==0);
  submit("help");serviceTerminalCommand();assert(executed==before+1);
  std::cout<<"PASS: window validation, web command limits/queue, scan gate, Wi-Fi wizard and secret handling\n";
}
'''
assert "executeTerminalCommand(Serial.readStringUntil('\\n'), false);" in source
assert "executeTerminalCommand(command, true);" in source
with tempfile.TemporaryDirectory(prefix="surveyor-commands-") as tmp:
    cpp, binary = Path(tmp) / "commands.cpp", Path(tmp) / "commands"
    cpp.write_text(stubs + "\n".join(function(s) for s in [
        "bool parseWifiAccessWindow(const String& value, uint32_t& seconds)",
        "void handleTerminalCommand()", "void serviceTerminalCommand()"
    ]) + tests)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
