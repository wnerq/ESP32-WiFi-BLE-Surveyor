"""Run actual button services against simulated GPIO/time: wsl --exec python3 Tools/test_survey_button.py."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
state = source[source.index("#ifndef SURVEY_BUTTON_PIN"):source.index("// ScanRecord remains")]
services = source[source.index("void initializeSurveyButton() {"):source.index("// Purpose: Arduino entry point")]
stubs = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
const int INPUT_PULLUP=2, LOW=0;
uint32_t now=0;
bool down=false, wifiScanInProgress=false, awake=false, wakeOk=true;
int scans=0, wakes=0, windows=0;
uint32_t millis(){return now;}
void pinMode(int,int){}
int digitalRead(int){return down ? LOW : 1;}
struct { void println(const char*){} } Serial;
struct { void arm(uint32_t){++windows;} } wifiPowerWindow;
bool wakeWifiRadio(){++wakes; if(wakeOk) awake=true; return wakeOk;}
bool beginLoggedWifiScan(bool initial,bool automatic){
  assert(!initial && !automatic);
  if(wifiScanInProgress) return false;
  ++scans; awake=true; return true;
}
'''
tests = r'''
void tick(uint32_t dt,bool pressed){now+=dt;down=pressed;serviceSurveyButton();}
void reset(uint32_t time=0,bool pressed=false){
  now=time;down=pressed;scans=wakes=windows=0;wakeOk=true;
  awake=wifiScanInProgress=false;initializeSurveyButton();
}
int main(){
  reset(); tick(10,true);tick(10,false);tick(40,false);
  assert(scans==0 && wakes==0); // contact noise
  tick(10,true);tick(30,true);tick(200,false);tick(30,false);
  assert(scans==1 && wakes==0 && awake);
  reset();tick(10,true);tick(30,true);tick(970,true);
  assert(scans==0 && wakes==1 && windows==1 && awake);
  tick(5000,true);tick(10,false);tick(30,false);
  assert(scans==0 && wakes==1); // exactly one hold, no release scan
  reset();awake=true;tick(10,true);tick(30,true);tick(1000,true);
  tick(1,false);tick(30,false);assert(scans==0 && windows==1);
  reset();wakeOk=false;tick(10,true);tick(30,true);tick(1000,true);
  tick(1,false);tick(30,false);assert(scans==0 && windows==0 && wakes==1);
  reset();tick(10,true);tick(30,true);tick(970,false);tick(30,false);
  assert(scans==0 && wakes==1); // release observed after missed threshold
  reset();tick(10,true);tick(30,true);tick(969,false);tick(30,false);
  assert(scans==1 && wakes==0); // release debounce does not turn tap into hold
  reset(UINT32_MAX-500);tick(10,true);tick(30,true);tick(970,true);
  tick(10,false);tick(30,false);assert(scans==0 && wakes==1);
  reset(0,true);tick(2000,true);tick(1,false);tick(30,false);
  assert(scans==0 && wakes==0); // held at boot
  tick(10,true);tick(30,true);tick(20,false);tick(30,false);assert(scans==1);
  reset();wifiScanInProgress=true;tick(10,true);tick(30,true);
  tick(20,false);tick(30,false);wifiScanInProgress=false;tick(100,false);
  assert(scans==0); // busy presses are not queued
  std::cout << "Button gesture tests passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="surveyor-button-") as tmp:
    cpp = Path(tmp) / "button.cpp"
    binary = Path(tmp) / "button"
    cpp.write_text(stubs + state + services + tests)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
