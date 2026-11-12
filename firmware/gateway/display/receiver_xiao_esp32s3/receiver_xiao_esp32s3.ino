// Southern Bluegill / XIAO ESP32S3 + B2B Wio-SX1262 kit (102010611).
// Receiver revision 10: probe calibration, dashboard and Wi-Fi setup improvements.
// OLED optional; SD disabled. Hold the Wio USER button (GPIO21) for 5 s for Wi-Fi setup.
// New board starts a NEW filesystem; old receiver logs/settings do not migrate automatically.
// Enabled reception at 907.8 MHz / 500 kHz / SF9 / CR4/7. No LoRa TX/ACK/relay.
// See README.md for first-use storage initialization, upload options and verification limits.
#include <RadioLib.h>
#include <Wire.h>
#include <U8g2lib.h>
// Optional 1.3-inch SH1106 I2C: GND, 3V3, SCK=D5/GPIO6, SDA=D4/GPIO5.
// Compile target: esp32 core 3.3.2, XIAO_ESP32S3, CDC enabled, default_8MB partition.
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0,U8X8_PIN_NONE,6,5);
bool oledReady=false;

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h> // ArduinoJson 7.4.2
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <FS.h>
#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <time.h>
#include <esp_partition.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>

// Time zone for the clock (this one is US Central; change it for your zone)
const char* TZ_INFO = "CST6CDT,M3.2.0,M11.1.0";

// SD access is deliberately disabled until a specific expansion/wiring is verified.
// Definitions are dormant; do not copy the Heltec SD wiring onto this board.
#define ENABLE_SD 0
#define SD_CS 3
#define SD_MOSI 9
#define SD_MISO 8
#define SD_SCK 7
constexpr float RADIO_MHZ=907.8f, RADIO_BW_KHZ=500.0f;
constexpr uint8_t RADIO_SF=9, RADIO_CR=7;
constexpr int8_t RADIO_DBM=10;
SX1262 radio = new Module(41,39,42,40); // NSS, DIO1, reset, busy
bool radioReady=false;
unsigned long lastRadioRetry=0;
WebServer server(80);
SPIClass sdSPI(HSPI);

bool sdOk = false, flashOk=false, storageError=false;
uint32_t invalidPackets=0,savedReadings=0;
String clearToken;
constexpr int USER_BUTTON=21; // Wio expansion USER button; XIAO RESET is not USER
bool screenOn=true,buttonRaw=HIGH,buttonStable=HIGH;
unsigned long buttonChanged=0,screenStarted=0,buttonPressedAt=0;
bool buttonHoldHandled=false;
struct Deployment;
String deploymentCSV(const String& node);
bool commitDeployment(Deployment& d,String& error);
void bindDeploymentToProbe(const String& node);
void loadDeployment();
void noteProbe(const String& node);
void startWifiSetup();
bool wifiSetupIsActive();
bool appendLine(const String& line);
bool initLog(fs::FS& fs);
bool markLogV5(fs::FS& fs);
bool rotateLog(fs::FS& fs);
bool writeLine(fs::FS& fs,const String& line,bool card);
float probeOffset(const String& node);
String oledTitle();
void updateDisplay();
void renderOLED();
void drawBrand();
void serviceOLED();
void serviceWeather();
void startWeatherWorker();
void screen(bool on) {
  screenOn=on;
  // Refresh OLED memory while it is still asleep, then reveal the current data.
  if(on) updateDisplay();
  if(oledReady)oled.setPowerSave(on?0:1);
  // Short press toggles OLED and periodic Serial status.
  screenStarted=millis();
}
void serviceButton() {
  unsigned long now=millis();
  bool v=digitalRead(USER_BUTTON);
  if(v!=buttonRaw){buttonRaw=v;buttonChanged=now;}
  if(now-buttonChanged>=40 && v!=buttonStable){
    buttonStable=v;
    if(v==LOW){buttonPressedAt=now;buttonHoldHandled=false;}
    else if(!buttonHoldHandled){screen(!screenOn);}
  }
  if(buttonStable==LOW && !buttonHoldHandled && now-buttonPressedAt>=5000UL){
    buttonHoldHandled=true;startWifiSetup();
  }
  // Mains-powered receiver: display remains on until USER toggles it off.
}
volatile bool receivedFlag = false;
void IRAM_ATTR setFlag() { receivedFlag = true; }

String lastTemp = "--";
String lastBatt = "";
String lastTime = "no data yet";
unsigned long lastRxMillis = 0;
bool haveRx = false;
float latestRSSI=NAN,latestSNR=NAN;
String latestRadioNode="";
uint64_t lastRxSeconds=0;
const char* receiverResetName() {
  switch(esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset";
    case ESP_RST_PANIC: return "Crash / panic";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep-sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout / low supply";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "Other / unknown";
  }
}
uint64_t receiverSeconds(){return (uint64_t)esp_timer_get_time()/1000000ULL;}
String jsonFloat(float value){return isfinite(value)?String(value,2):String("null");}
unsigned long lastDisplayMillis = 0;
unsigned long lastWifiCheck = 0;

// Wi-Fi onboarding: open only at first setup or after a physical 5-second hold.
// Settings live in NVS; this code never formats or clears a log filesystem.
struct WifiSettings { uint32_t magic; char ssid[33]; char password[65]; };
constexpr uint32_t WIFI_SETTINGS_MAGIC=0x42475731;
constexpr unsigned long SETUP_WINDOW_MS=10UL*60UL*1000UL;
constexpr unsigned long CONNECT_TIMEOUT_MS=30000UL;
Preferences wifiPreferences;
DNSServer setupDNS;
WifiSettings savedWifi={},candidateWifi={};
bool preferencesOk=false,haveSavedWifi=false,setupActive=false;
bool wifiTrying=false,wifiBeginPending=false,scanBusy=false,scanReady=false;
bool setupCloseRequested=false,setupSucceeded=false;
unsigned long setupStarted=0,wifiAttemptStarted=0,wifiBeginAt=0,setupCloseAt=0;
String setupSSID,setupToken,wifiMessage="Not configured";

bool deadlineReached(unsigned long now,unsigned long deadline){return (long)(now-deadline)>=0;}
bool onSetupNetwork(){return setupActive && server.client().localIP()==WiFi.softAPIP();}
String jsonText(const String& s){
  String out="\"";
  for(unsigned i=0;i<s.length();++i){unsigned char c=s[i];
    if(c=='"'||c=='\\'){out+='\\';out+=(char)c;}
    else if(c<32){char escaped[7];snprintf(escaped,sizeof(escaped),"\\u%04x",c);out+=escaped;}
    else out+=(char)c;
  }
  return out+"\"";
}
void wifiReply(int code,const String& text){
  server.sendHeader("Cache-Control","no-store");
  server.send(code,"application/json",String("{\"message\":")+jsonText(text)+"}");
}
bool requireSetup(bool mutation=false){
  if(!onSetupNetwork()){wifiReply(403,"Hold USER for 5 seconds, then join the receiver setup network.");return false;}
  if(mutation && setupCloseRequested){wifiReply(409,"Setup is closing. Rejoin your home Wi-Fi.");return false;}
  if(mutation && server.header("X-Setup-Token")!=setupToken){wifiReply(403,"Reopen the setup page and try again.");return false;}
  return true;
}
void beginSavedWifi(){
  if(haveSavedWifi){WiFi.setAutoReconnect(true);WiFi.begin(savedWifi.ssid,savedWifi.password);}
}
void loadWifiSettings(){
  preferencesOk=wifiPreferences.begin("bluegill-wifi",false);
  if(!preferencesOk){wifiMessage="Settings storage unavailable";return;}
  if(wifiPreferences.getBytesLength("config")!=sizeof(savedWifi))return;
  WifiSettings loaded={};
  if(wifiPreferences.getBytes("config",&loaded,sizeof(loaded))!=sizeof(loaded))return;
  if(loaded.magic!=WIFI_SETTINGS_MAGIC || !memchr(loaded.ssid,0,sizeof(loaded.ssid)) ||
     !memchr(loaded.password,0,sizeof(loaded.password)) || !loaded.ssid[0])return;
  savedWifi=loaded;haveSavedWifi=true;wifiMessage="Connecting to saved Wi-Fi";
}
void printSetupRadioState(){
  wifi_mode_t mode=WIFI_MODE_NULL;
  uint8_t primary=0;
  wifi_second_chan_t secondary=WIFI_SECOND_CHAN_NONE;
  wifi_config_t config={};
  const esp_err_t modeResult=esp_wifi_get_mode(&mode);
  const esp_err_t channelResult=esp_wifi_get_channel(&primary,&secondary);
  const esp_err_t configResult=esp_wifi_get_config(WIFI_IF_AP,&config);
  const char* modeName=mode==WIFI_MODE_APSTA?"AP+STA":
    (mode==WIFI_MODE_AP?"AP":(mode==WIFI_MODE_STA?"STA":"off"));
  Serial.printf("Wi-Fi diag: mode=%s (err=%d) channel=%u (err=%d) AP-config=%d hidden=%d clients=%u STA-status=%d\n",
    modeName,(int)modeResult,(unsigned)primary,(int)channelResult,(int)configResult,
    configResult==ESP_OK?(int)config.ap.ssid_hidden:-1,
    (unsigned)WiFi.softAPgetStationNum(),(int)WiFi.status());
}

void stopWifiSetup(){
  if(!setupActive)return;
  if(scanBusy)esp_wifi_scan_stop();
  setupDNS.stop();WiFi.softAPdisconnect(false);WiFi.mode(WIFI_STA);
  setupActive=false;setupToken="";setupCloseAt=0;scanReady=false;setupCloseRequested=false;
  if(scanBusy){WiFi.scanDelete();scanBusy=false;}
  else WiFi.scanDelete();
  lastWifiCheck=millis();
  if(WiFi.status()!=WL_CONNECTED)beginSavedWifi();
  else WiFi.setAutoReconnect(true);
  screenStarted=millis();updateDisplay();
}
void startWifiSetup(){
  if(setupActive){setupStarted=millis();screen(true);return;}
  IPAddress ip(192,168,4,1),mask(255,255,255,0);
  // Avoid colliding with a home router using the usual setup subnet.
  if(WiFi.status()==WL_CONNECTED && WiFi.localIP()[0]==192 && WiFi.localIP()[1]==168 && WiFi.localIP()[2]==4)ip=IPAddress(192,168,8,1);
  // Pause both Arduino auto-reconnect and our timed retries while serving setup.
  // Disconnect without erasing credentials; keep STA available for scan/join.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false,false);
  if(!WiFi.mode(WIFI_AP_STA) || !WiFi.softAPConfig(ip,ip,mask) ||
      !WiFi.softAP(setupSSID.c_str(),nullptr,1,0,2)){
    printSetupRadioState();
    WiFi.mode(WIFI_STA);lastWifiCheck=millis();beginSavedWifi();
    wifiMessage="Setup hotspot failed; hold USER to retry";Serial.println(wifiMessage);return;
  }
  setupToken=String(esp_random(),HEX)+String(esp_random(),HEX);
  setupActive=true;setupStarted=millis();setupCloseAt=0;setupCloseRequested=false;setupSucceeded=false;
  setupDNS.start(53,"*",WiFi.softAPIP());
  wifiMessage=haveSavedWifi?"Choose a network to change Wi-Fi; existing settings are retained until a new connection succeeds.":"Choose your home 2.4 GHz Wi-Fi network.";
  screen(true);
  Serial.print("Wi-Fi setup: join ");Serial.println(setupSSID);
  Serial.print("Open http://");Serial.println(WiFi.softAPIP());
  printSetupRadioState();
}
void failWifiAttempt(const String& reason){
  wifiTrying=false;wifiBeginPending=false;memset(&candidateWifi,0,sizeof(candidateWifi));
  WiFi.disconnect(false,false); // Keep the hotspot available for another attempt.
  wifiMessage=reason+(haveSavedWifi?" Previous saved settings retained.":" Try again.");
  updateDisplay();
}
void serviceWifi(){
  unsigned long now=millis();
  if(setupActive)setupDNS.processNextRequest();
  if(setupActive && setupCloseRequested){
    if(deadlineReached(now,setupCloseAt)){
      bool cancelled=wifiTrying;
      if(cancelled){
        wifiTrying=false;wifiBeginPending=false;memset(&candidateWifi,0,sizeof(candidateWifi));
        WiFi.setAutoReconnect(false);WiFi.disconnect(false,false);
      }
      stopWifiSetup();
      wifiMessage=haveSavedWifi?"Setup closed; saved Wi-Fi retained.":"Setup closed; hold USER for 5 seconds to configure Wi-Fi.";
    }
    return;
  }
  if(scanBusy){int n=WiFi.scanComplete();if(n!=WIFI_SCAN_RUNNING){scanBusy=false;scanReady=n>=0;}}
  if(wifiTrying){
    if(wifiBeginPending && deadlineReached(now,wifiBeginAt)){
      WiFi.setAutoReconnect(false);WiFi.disconnect(false,false);
      WiFi.begin(candidateWifi.ssid,candidateWifi.password);
      wifiBeginPending=false;wifiAttemptStarted=now;
    }else if(!wifiBeginPending && WiFi.status()==WL_CONNECTED && WiFi.SSID()==String(candidateWifi.ssid)){
      if(!preferencesOk || wifiPreferences.putBytes("config",&candidateWifi,sizeof(candidateWifi))!=sizeof(candidateWifi)){
        failWifiAttempt("Connected, but saving settings failed.");
      }else{
        savedWifi=candidateWifi;haveSavedWifi=true;memset(&candidateWifi,0,sizeof(candidateWifi));
        wifiTrying=false;
        wifiMessage="Connected and saved. Setup shuts down in 10 seconds. Rejoin your home Wi-Fi to open the dashboard.";
        setupSucceeded=true;setupCloseAt=now+10000UL;updateDisplay();
      }
    }else if(!wifiBeginPending && now-wifiAttemptStarted>=CONNECT_TIMEOUT_MS){
      failWifiAttempt("Could not connect. Check the password, 2.4 GHz network and signal.");
    }
  }
  if(setupActive && !wifiTrying &&
      ((setupCloseAt && deadlineReached(now,setupCloseAt)) || now-setupStarted>=SETUP_WINDOW_MS))stopWifiSetup();
  // A router outage does not reopen an unauthenticated setup hotspot.
  if(!setupActive && !wifiTrying && !scanBusy && haveSavedWifi && WiFi.status()!=WL_CONNECTED && now-lastWifiCheck>=30000UL){
    lastWifiCheck=now;WiFi.disconnect(false,false);beginSavedWifi();
  }
}

const char WIFI_SETUP_PAGE[] PROGMEM=R"wifihtml(
<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Bluegill Wi-Fi setup</title>
<style>body{font:16px/1.5 system-ui;margin:24px auto;padding:0 16px;max-width:620px;background:#eef3f6;color:#183047}section{background:white;padding:22px;border-radius:12px;border-top:4px solid #1d6873}label{display:block;margin-top:16px}input,select,button{font:inherit;box-sizing:border-box;min-height:44px;border-radius:6px;padding:10px;max-width:100%}input,select{width:100%;border:1px solid #789}button{background:#1d5361;color:white;border:0;margin:12px 8px 0 0;cursor:pointer}button:disabled{opacity:.55}small{display:block;color:#526778}:focus-visible{outline:3px solid #efae32;outline-offset:3px}#message{min-height:3em;font-weight:600}</style>
<h1>Connect your receiver</h1><p>Wi-Fi setup · revision 2</p><section><p>Select your home <strong>2.4 GHz Wi-Fi</strong>. No Bluegill setup password is needed. Your router still requires its own password.</p>
<form id="wifi"><button type="button" id="scan">Search for networks</button><small id="scanStatus" role="status"></small><label for="networks">Nearby networks</label><select id="networks"><option value="">Select a network or enter its name below</option></select>
<label for="ssid">Wi-Fi network name</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off" autocapitalize="none" spellcheck="false">
<label for="password">Home Wi-Fi password</label><input id="password" name="password" type="password" maxlength="64" autocomplete="new-password" autocapitalize="none" spellcheck="false"><small>Leave blank only if your home network has no password. A blank field does not reuse an old password.</small>
<label><input type="checkbox" id="show" style="width:auto;min-height:0"> Show password</label><button id="connect" type="submit">Connect and save</button></form>
<p id="message" role="status" aria-live="polite">Loading connection status...</p><p id="address"></p><button type="button" id="finish">Close setup</button>
<p>Logging continues during setup. Saved readings are never erased by changing Wi-Fi. Setup closes after 10 minutes, or 10 seconds after a successful connection.</p><small>This temporary setup network is open. Use it near your receiver. If the phone says “No internet,” stay connected to finish setup. If the page closes during connection, rejoin the setup network and open this address again.</small></section>
<script>
const token='__SETUP_TOKEN__',$=id=>document.getElementById(id);let fetching=false,scanning=false,connecting=false,closing=false,pollTimer;
async function request(path,options={}){const r=await fetch(path,{cache:'no-store',...options});const data=await r.json();if(!r.ok)throw Error(data.message||'Request failed');return data;}
function controls(){ $('connect').disabled=closing||connecting||scanning;$('scan').disabled=closing||connecting||scanning;$('finish').disabled=closing;}
async function poll(){if(fetching||closing)return;fetching=true;try{const s=await request('/wifi/state');if(closing)return;connecting=s.trying;scanning=s.scanning;controls();$('message').textContent=s.message;$('address').textContent=s.connected?'Dashboard on your home Wi-Fi: http://'+s.ip:'';if(s.complete){closing=true;clearInterval(pollTimer);$('wifi').hidden=true;$('finish').hidden=true;$('message').textContent='Connected and saved. Setup shuts down in 10 seconds. Rejoin your home Wi-Fi and use the dashboard address below. Saved readings are unchanged.';return;}
if(scanning)$('scanStatus').textContent='Looking for networks...';else{const n=await request('/wifi/networks');if(closing)return;const current=$('networks').value;$('networks').replaceChildren(new Option('Select a network or enter its name below',''));for(const x of n.networks)$('networks').add(new Option(x.ssid+(x.open?' (open)':''),x.ssid));$('networks').value=current;$('scanStatus').textContent=n.ready?(n.networks.length?'Choose a network, or enter a hidden network name.':'No networks found. Enter the name manually or try again.'):'Tap Search for networks, or enter the name manually.';}}
catch(e){if(closing)return;$('message').textContent=e.message+'. If setup closed, rejoin your home Wi-Fi. Hold USER for 5 seconds to reopen setup.';}finally{fetching=false;}}
$('networks').onchange=()=>{$('ssid').value=$('networks').value;};$('show').onchange=()=>{$('password').type=$('show').checked?'text':'password';};
$('scan').onclick=async()=>{scanning=true;controls();try{await request('/wifi/scan',{method:'POST',headers:{'X-Setup-Token':token}});}catch(e){$('scanStatus').textContent=e.message;}finally{await poll();}};
$('wifi').onsubmit=async e=>{e.preventDefault();connecting=true;controls();$('message').textContent='Connecting; this may take 30 seconds...';try{await request('/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Setup-Token':token},body:new URLSearchParams({ssid:$('ssid').value,password:$('password').value})});$('password').value='';}catch(e){if(closing)return;connecting=false;controls();$('message').textContent=e.message;}};
$('finish').onclick=async()=>{
 closing=true;controls();$('message').textContent='Closing setup...';
 try{
  await request('/wifi/close',{method:'POST',headers:{'X-Setup-Token':token}});
  clearInterval(pollTimer);
  $('wifi').hidden=true;$('finish').hidden=true;$('scanStatus').textContent='';
  $('message').textContent='Setup will close in about 2 seconds. Open your phone Wi-Fi settings and rejoin your home network. Saved readings are unchanged.';
  const hint=document.createElement('p');hint.textContent='The receiver cannot switch your phone Wi-Fi automatically. Use the dashboard address above after rejoining. Hold USER for 5 seconds to reopen setup.';$('message').after(hint);
 }catch(e){closing=false;controls();$('message').textContent=e.message+'. If the setup network has disappeared, it has closed; rejoin your home Wi-Fi.';}
};
poll();pollTimer=setInterval(poll,2000);
</script></html>
)wifihtml";
void handleWifiPage(){
  if(!requireSetup())return;
  String page=FPSTR(WIFI_SETUP_PAGE);page.replace("__SETUP_TOKEN__",setupToken);
  server.sendHeader("Cache-Control","no-store");server.sendHeader("Referrer-Policy","no-referrer");
  server.send(200,"text/html",page);
}
void handleWifiState(){
  if(!requireSetup())return;
  bool connected=WiFi.status()==WL_CONNECTED;
  String data=String("{\"complete\":")+(setupSucceeded?"true":"false")+",\"trying\":"+(wifiTrying?"true":"false")+",\"scanning\":"+(scanBusy?"true":"false")+
    ",\"connected\":"+(connected?"true":"false")+",\"ip\":"+jsonText(connected?WiFi.localIP().toString():String(""))+",\"message\":"+jsonText(wifiMessage)+"}";
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",data);
}
void handleWifiScan(){
  if(!requireSetup(true))return;
  if(wifiTrying||scanBusy){wifiReply(409,"Wait for the current connection or scan to finish.");return;}
  WiFi.scanDelete();scanReady=false;
  int result=WiFi.scanNetworks(true);
  scanBusy=result==WIFI_SCAN_RUNNING;scanReady=result>=0;
  wifiReply(result==WIFI_SCAN_FAILED?500:202,result==WIFI_SCAN_FAILED?"Scan failed. Enter the network name manually.":"Looking for networks...");
}
void handleWifiNetworks(){
  if(!requireSetup())return;
  String data=String("{\"ready\":")+(scanReady?"true":"false")+",\"networks\":[";
  int count=scanReady?WiFi.scanComplete():0,shown=0;
  for(int i=0;i<count && shown<20;i++){
    String name=WiFi.SSID(i);if(!name.length())continue;
    bool duplicate=false;for(int j=0;j<i;j++)if(WiFi.SSID(j)==name){duplicate=true;break;}
    if(duplicate)continue;
    if(shown++)data+=',';
    data+=String("{\"ssid\":")+jsonText(name)+",\"open\":"+(WiFi.encryptionType(i)==WIFI_AUTH_OPEN?"true":"false")+"}";
  }
  data+="]}";server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",data);
}
void handleWifiConnect(){
  if(!requireSetup(true))return;
  if(wifiTrying||scanBusy){wifiReply(409,"Wait for the current connection or scan to finish.");return;}
  if(!preferencesOk){wifiReply(500,"Settings storage unavailable. Existing logs are unchanged.");return;}
  String ssid=server.arg("ssid"),password=server.arg("password");
  if(!ssid.length() || ssid.length()>32 || password.length()>64){wifiReply(400,"Use a network name up to 32 bytes and a password up to 64 characters.");return;}
  if(password.length()>0 && password.length()<8){wifiReply(400,"Wi-Fi passwords normally require at least 8 characters.");return;}
  if(password.length()==64){for(unsigned i=0;i<64;i++)if(!isxdigit((unsigned char)password[i])){wifiReply(400,"A 64-character Wi-Fi key must contain only hexadecimal digits.");return;}}
  memset(&candidateWifi,0,sizeof(candidateWifi));candidateWifi.magic=WIFI_SETTINGS_MAGIC;
  ssid.toCharArray(candidateWifi.ssid,sizeof(candidateWifi.ssid));password.toCharArray(candidateWifi.password,sizeof(candidateWifi.password));
  wifiTrying=true;wifiBeginPending=true;wifiBeginAt=millis()+200UL;
  setupStarted=millis();setupCloseAt=0;wifiMessage="Connecting... please allow 30 seconds.";
  wifiReply(202,"Connecting. Settings will be saved only after a successful connection.");
}
void handleWifiClose(){
  if(!requireSetup(true))return;
  setupCloseRequested=true;setupCloseAt=millis()+1500UL;
  wifiMessage="Closing setup; saved readings and last working Wi-Fi retained.";
  wifiReply(200,"Setup closing; logs retained.");
}
bool dashboardAllowed(){
  // The open setup hotspot exposes setup only, never log downloads or Clear.
  if(onSetupNetwork()){
    if(server.method()==HTTP_GET)handleWifiPage();else server.send(403,"text/plain","Log changes are unavailable on the setup network.");
    return false;
  }
  return true;
}

bool wifiSetupIsActive(){return setupActive;}

const char* OLD_LOG_HEADER = "timestamp_utc,temp_f,rssi,batt_v,time_valid,uptime_ms,node_id,boot_id,sequence,quality";
const char* V3_LOG_HEADER = "timestamp_utc,temp_f,rssi,batt_v,time_valid,uptime_ms,node_id,boot_id,sequence,quality,deployment_revision,location_name,water_type,latitude,longitude,probe_depth_m,deployment_effective_utc";
const char* V4_LOG_HEADER = "timestamp_utc,temp_f,rssi,batt_v,time_valid,uptime_ms,node_id,boot_id,sequence,quality,deployment_revision,water_body_name,water_type,latitude,longitude,probe_depth_m,deployment_effective_utc,location_name,location_basis";
const char* LOG_HEADER = "timestamp_utc,temp_f,rssi,batt_v,time_valid,uptime_ms,node_id,boot_id,sequence,quality,deployment_revision,water_body_name,water_type,latitude,longitude,probe_depth_m,deployment_effective_utc,location_name,location_basis,temp_raw_f,calibration_offset_f";

// Uses the SD card for the log when one is present, otherwise internal storage
fs::FS& logFS() {
  if (sdOk) return SD;
  return LittleFS;
}

void timestampNow(char* buf, size_t n) {
  struct tm t;
  if (!getLocalTime(&t, 0)) { snprintf(buf, n, "no-time"); return; }
  time_t now=time(nullptr); gmtime_r(&now,&t);
  strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &t);
}

int batteryPercent(float v) {
  int pct = (int)((v - 3.3) / (4.2 - 3.3) * 100.0);   // rough LiPo estimate
  return constrain(pct, 0, 100);
}

void updateDisplay() {
  lastDisplayMillis=millis();
  if(setupActive)printSetupRadioState();
  if(!screenOn)return;
  if(setupActive){
    Serial.printf("Wi-Fi setup: %s | http://%s/wifi | %s\n",setupSSID.c_str(),WiFi.softAPIP().toString().c_str(),wifiMessage.c_str());
  }else{
    Serial.printf("Receiver: %s F | field battery %s V | dashboard http://%s/ | LoRa %s | storage %s\n",
      lastTemp.c_str(),lastBatt.c_str(),WiFi.localIP().toString().c_str(),radioReady?"listening":"retrying",storageError?"ERROR":"OK");
  }
  renderOLED();
}

// Strict packet decoding; accepts legacy temp[,battery] and version 1 packets.
struct Reading { float temp=NAN, batt=NAN; String node="legacy",boot="",seq="",quality="ok"; };
bool number(const String& s, float& v) {
  if (!s.length()) return false;
  char* end; v=strtof(s.c_str(), &end);
  return end != s.c_str() && *end == '\0' && isfinite(v);
}
bool digits(const String& s) {
  if (!s.length() || s.length()>10) return false;
  for (unsigned i=0;i<s.length();i++) if (!isDigit(s[i])) return false;
  return strtoull(s.c_str(),nullptr,10)<=UINT32_MAX;
}
bool decode(String payload, Reading& r);
bool decode(String payload, Reading& r) {
  if (payload.length()>112) return false;
  String f[7]; int count=0, start=0;
  for (unsigned i=0;i<=payload.length();i++) if (i==payload.length() || payload[i]==',') {
    if (count==7) return false;
    f[count++]=payload.substring(start,i); start=i+1;
  }
  if (count==7) {
    if (f[0]!="1" || !f[1].length() || f[1].length()>16 || !digits(f[2]) || !digits(f[3])) return false;
    for(unsigned i=0;i<f[1].length();i++) if (!isAlphaNumeric(f[1][i]) && f[1][i]!='_' && f[1][i]!='-') return false;
    r.node=f[1];r.boot=f[2];r.seq=f[3];r.quality=f[6];
    if (r.quality!="ok" && r.quality!="sensor_error") return false;
    if (r.quality=="ok" && !number(f[4],r.temp)) return false;
    if (r.quality=="sensor_error" && f[4].length()) return false;
    if (!number(f[5],r.batt)) return false;
  } else if (count==1 || count==2) {
    if (!number(f[0],r.temp)) return false;
    if(count==2 && !number(f[1],r.batt)) return false;
  } else return false;
  if (isfinite(r.temp) && (r.temp < -67 || r.temp > 257)) { r.temp=NAN; r.quality="sensor_error"; }
  if (isfinite(r.batt) && (r.batt<0 || r.batt>6)) return false;
  return true;
}
void logReading(const String& payload, float rssi, float snr) {
  Reading r;
  if(!decode(payload,r)){++invalidPackets;return;}
  noteProbe(r.node);
  latestRSSI=rssi;latestSNR=snr;latestRadioNode=r.node;
  lastRxSeconds=receiverSeconds();
  lastRxMillis=millis(); haveRx=true;
  const bool validTemp=isfinite(r.temp);
  const float offset=validTemp?probeOffset(r.node):0;
  const String rawTemp=validTemp?String(r.temp,1):String("");
  if(validTemp)r.temp+=offset;
  lastTemp=isfinite(r.temp)?String(r.temp,1):String("FAULT");
  lastBatt=isfinite(r.batt)?String(r.batt,2):String("");
  char ts[24]; timestampNow(ts,sizeof(ts)); lastTime=ts;
  String line=String(ts)+","+(isfinite(r.temp)?String(r.temp,1):String(""))+","+String(rssi,0)+","+lastBatt+","+
    (String(ts)=="no-time"?"0":"1")+","+String(millis())+","+r.node+","+r.boot+","+r.seq+","+r.quality+deploymentCSV(r.node)+","+rawTemp+","+(validTemp?String(offset,1):String(""))+"\n";
  storageError=!appendLine(line);
  if(!storageError) ++savedReadings;
  Serial.printf("%s: %s",storageError?"SAVE FAILED":"Saved",line.c_str());
}
bool markLogV5(fs::FS& fs){
  if(fs.exists("/log-v5.ready"))return true;
  File marker=fs.open("/log-v5.ready",FILE_WRITE);if(!marker)return false;
  bool ok=marker.print("v5\n")==3;marker.flush();marker.close();return ok;
}
bool initLog(fs::FS& fs) {
  if(fs.exists("/log-v5.csv"))return markLogV5(fs);
  if(fs.exists("/log-v5.tmp")&&!fs.remove("/log-v5.tmp"))return false;
  bool card=(&fs==static_cast<fs::FS*>(&SD));
  const char* source=nullptr;
  if(!fs.exists("/log-v5.ready")){
    if(fs.exists("/log-v4.csv"))source="/log-v4.csv";
    else if(!fs.exists("/log-v4.ready") && fs.exists("/log-v3.csv"))source="/log-v3.csv";
    else if(!fs.exists("/log-v4.ready") && fs.exists("/log-v2.csv"))source="/log-v2.csv";
  }
  File old;if(source){old=fs.open(source,FILE_READ);if(!old)return false;}
  uint64_t oldSize=source?old.size():0,total=card?SD.totalBytes():LittleFS.totalBytes(),used=card?SD.usedBytes():LittleFS.usedBytes();
  if(total<=used||total-used<oldSize+oldSize/3+16384+(card?1024ULL*1024:64ULL*1024)){if(source)old.close();return false;}
  File out=fs.open("/log-v5.tmp",FILE_WRITE);if(!out){if(source)old.close();return false;}
  bool ok=out.println(LOG_HEADER)>0;int columns=0;
  if(source){
    String header=old.readStringUntil('\n');header.trim();columns=header==OLD_LOG_HEADER?10:header==V3_LOG_HEADER?17:header==V4_LOG_HEADER?19:0;ok=ok&&columns>0;
    while(ok&&old.available()){
      String row;bool tooLong=false;
      while(old.available()){int c=old.read();if(c=='\n')break;if(c>=0){if(row.length()<1024)row+=(char)c;else tooLong=true;}}
      if(row.endsWith("\r"))row.remove(row.length()-1);
      if(tooLong){ok=false;break;}if(!row.length())continue;
      int fields=1;bool quoted=false;
      for(unsigned i=0;i<row.length();i++){
        if(row[i]=='"'){if(quoted&&i+1<row.length()&&row[i+1]=='"')i++;else quoted=!quoted;}
        else if(row[i]==','&&!quoted)fields++;
      }
      if(quoted||fields!=columns){ok=false;break;}
      row+=columns==10?",,,,,,,,,,,\n":columns==17?",,,,\n":",,\n";ok=out.print(row)==row.length();yield();
    }old.close();
  }
  out.flush();out.close();if(!ok)return false;
  return fs.rename("/log-v5.tmp","/log-v5.csv")&&markLogV5(fs);
}
constexpr size_t LOG_ROTATE_BYTES=512UL*1024;
uint64_t storageTotal() {return sdOk?SD.totalBytes():(flashOk?LittleFS.totalBytes():0);}
uint64_t storageUsed() {return sdOk?SD.usedBytes():(flashOk?LittleFS.usedBytes():0);}
uint64_t storageReserve(bool card) {return card?1024ULL*1024:64ULL*1024;}
bool archiveIndex(const char* name,uint32_t& index) {
  const char* base=strrchr(name,'/');base=base?base+1:name;
  if(strlen(base)!=22 || strncmp(base,"archive-",8)!=0 || strcmp(base+18,".csv")!=0) return false;
  uint64_t n=0;
  for(int i=8;i<18;i++){if(base[i]<'0'||base[i]>'9')return false;n=n*10+(base[i]-'0');}
  if(n==0 || n>UINT32_MAX)return false;
  index=(uint32_t)n;return true;
}
bool rotateLog(fs::FS& fs) {
  if(!fs.exists("/logs") && !fs.mkdir("/logs")) return false;
  File dir=fs.open("/logs");if(!dir || !dir.isDirectory())return false;
  uint32_t highest=0;
  File f=dir.openNextFile();
  while(f){uint32_t n;if(!f.isDirectory() && archiveIndex(f.name(),n) && n>highest)highest=n;f.close();f=dir.openNextFile();}
  dir.close();if(highest==UINT32_MAX)return false;
  char path[40];snprintf(path,sizeof(path),"/logs/archive-%010lu.csv",(unsigned long)(highest+1));
  if(fs.exists(path))return false; // never overwrite history
  if(!fs.rename("/log-v5.csv",path))return false;
  return initLog(fs); // after a reset here, startup recreates the current file
}
bool writeLine(fs::FS& fs,const String& line,bool card) {
  uint64_t total=card?SD.totalBytes():LittleFS.totalBytes();
  uint64_t used=card?SD.usedBytes():LittleFS.usedBytes();
  // Reserve filesystem headroom; never delete old data to make room.
  if(total<=used || total-used<storageReserve(card)+line.length()+4096)return false;
  if(!initLog(fs)) return false;
  File old=fs.open("/log-v5.csv",FILE_READ);if(!old)return false;
  size_t size=old.size();old.close();
  if(size+line.length()>LOG_ROTATE_BYTES && !rotateLog(fs))return false;
  File f=fs.open("/log-v5.csv",FILE_APPEND);
  if(!f) return false;
  size_t n=f.print(line); f.flush(); f.close(); return n==line.length();
}
// Stream an archive directory page so a large card does not require a large String.
void handleArchives() {
  server.sendHeader("Cache-Control","no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"text/html","<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><title>Saved logs</title><h1>Saved logs</h1><p>Active storage only. Downloads do not delete files.</p><p><a href='/'>Back to dashboard</a></p><ul>");
  File dir=logFS().open("/logs");
  if(dir && dir.isDirectory()) {
    File f=dir.openNextFile();
    while(f){uint32_t n;if(!f.isDirectory() && archiveIndex(f.name(),n)){
      char base[28];snprintf(base,sizeof(base),"archive-%010lu.csv",(unsigned long)n);
      server.sendContent(String("<li><a href='/download?name=")+base+"'>"+base+"</a> ("+String((unsigned long)f.size())+" bytes)</li>");
    }f.close();f=dir.openNextFile();}dir.close();
  }
  if(logFS().exists("/archive-v2.csv"))server.sendContent("<li><a href='/archive.csv'>Original single archive</a></li>");
  server.sendContent("</ul><p>If no files are listed, no archives have been created yet. Charts show the current log; download archives to inspect earlier history.</p>");server.sendContent("");
}
void handleArchiveDownload() {
  String name=server.arg("name");uint32_t n;
  if(name.indexOf('/')>=0 || !archiveIndex(name.c_str(),n)){server.send(400,"text/plain","Invalid archive name");return;}
  File f=logFS().open(String("/logs/")+name,FILE_READ);
  if(!f){server.send(404,"text/plain","Archive not found");return;}
  server.sendHeader("Content-Disposition",String("attachment; filename=")+name);
  server.sendHeader("Cache-Control","no-store");server.streamFile(f,"text/csv");f.close();
}

bool appendLine(const String& line) {
  if(sdOk && writeLine(SD,line,true)) return true;
  if(sdOk) {sdOk=false;Serial.println("SD write failed; using internal flash");}
  return flashOk && writeLine(LittleFS,line,false);
}
void handleStatus() {
  uint64_t total=storageTotal(),used=storageUsed(),free=total>used?total-used:0;
  bool nearFull=total==0 || free<storageReserve(sdOk)+4096 || free<total/10;
  String s="{\"received\":"+String(haveRx?"true":"false")+",\"age_s\":"+String((unsigned long)(haveRx?receiverSeconds()-lastRxSeconds:0))+
    ",\"storage_error\":"+String(storageError?"true":"false")+",\"storage\":\""+String(sdOk?"SD":"Internal flash")+
    "\",\"used\":"+String((unsigned long)storageUsed())+
    ",\"total\":"+String((unsigned long)storageTotal())+
    ",\"saved\":"+String(savedReadings)+",\"near_full\":"+String(nearFull?"true":"false")+",\"invalid\":"+String(invalidPackets)+"}";
  s.remove(s.length()-1);
  s+=",\"rssi\":"+jsonFloat(latestRSSI)+",\"snr\":"+jsonFloat(latestSNR)+
    ",\"radio_node\":"+jsonText(latestRadioNode)+
    ",\"receiver_uptime_s\":"+String((unsigned long)receiverSeconds())+
    ",\"receiver_reset\":"+jsonText(String(receiverResetName()))+",\"radio_ready\":"+String(radioReady?"true":"false")+"}";
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",s);
}
void handleClear() {
  // A per-boot token plus POST prevents accidental link activation/cross-site form submission.
  if(server.header("X-Log-Token")!=clearToken){server.send(403,"text/plain","Reload page before clearing");return;}
  bool ok=(sdOk||flashOk);
  if(ok && logFS().exists("/log-v5.csv")) ok=logFS().remove("/log-v5.csv");
  if(ok) ok=initLog(logFS());
  storageError=!ok;
  server.send(ok?200:500,"text/plain",ok?"Current log cleared; archive and legacy logs retained":"Clear failed");
}

// Local deployment metadata is kept separately from temperature logs and Wi-Fi.
// Each successful save commits a new immutable snapshot; no old reading is rewritten.
struct LegacyDeployment {
  uint32_t magic,revision,receiverSession,uptimeMs;
  char node[17],name[65],effectiveUTC[24];
  uint8_t waterType,hasCoordinates,hasDepth,depthFeet;
  double latitude,longitude,depthM;
  uint32_t checksum;
};
struct Deployment {
  uint32_t magic,revision,receiverSession,uptimeMs;
  char node[17],name[151],location[151],effectiveUTC[24];
  uint8_t waterType,hasCoordinates,hasDepth,depthFeet;
  double latitude,longitude,depthM;
  uint32_t checksum;
};
constexpr uint32_t DEPLOYMENT_MAGIC=0x42474432;
Deployment deployment={};
bool deploymentSet=false,deploymentHistoryWarning=false;
uint32_t deploymentHigh=0,receiverSession=0;
void initOLED(){
  Wire.begin(5,6); Wire.setTimeOut(25);
  uint8_t address=0;
  for(uint8_t candidate=0x3C;candidate<=0x3D;++candidate){
    Wire.beginTransmission(candidate);
    if(Wire.endTransmission()==0){address=candidate;break;}
  }
  if(!address){Serial.println("OLED absent; receiver continues without display");return;}
  oled.setI2CAddress(address<<1);oled.setBusClock(100000);oled.begin();
  Wire.setTimeOut(25);oledReady=true;oled.clearBuffer();
  drawBrand();
}
static const uint8_t brandBitmap[] PROGMEM={
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfc,0xff,0xff,0xff,0xff,0xff,0xff,0x01,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0x01,0x0e,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x0e,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x0e,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x0e,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x8e,0xff,0xff,0xff,0xff,0xff,0x83,0x01,0x8e,0xff,0xff,0xff,0xff,0xff,0x83,0x01,0x0e,0xfe,0xff,0xff,0xff,0xff,0x83,0x01,0x0e,0x7c,0x00,0x00,0x0e,0xe0,0x83,0x01,0x0e,0x78,0x00,0x00,0x07,0x70,0x83,0x01,0x0e,0xf0,0x00,0xc0,0x03,0x3c,0x83,0x01,0x0e,0xe0,0x01,0xe0,0x00,0x1e,0x83,0x01,0x0e,0xc0,0x03,0x70,0x80,0x07,0x83,0x01,0x0e,0x80,0x07,0x38,0xc0,0x03,0x83,0x01,0x0e,0x00,0x0e,0x0e,0xe0,0x00,0x83,0x01,0x0e,0x00,0x1c,0x07,0x78,0x00,0x83,0x01,0x4e,0x00,0xf8,0x03,0x3c,0x00,0x83,0x01,0xce,0x00,0xe0,0x01,0x0e,0x30,0x83,0x01,0xce,0x01,0x70,0x80,0x07,0x7c,0x83,0x01,0x8e,0x03,0x38,0xc0,0x03,0x3e,0x83,0x01,0x0e,0x07,0x1c,0xe0,0x00,0x3e,0x83,0x01,0x0e,0x0e,0x0e,0x78,0x00,0x00,0x83,0x01,0x0e,0x1c,0x07,0x3c,0x00,0x00,0x83,0x01,0x0e,0xb8,0x03,0x1f,0x00,0x00,0x83,0x01,0x0e,0xf0,0x01,0x0f,0x00,0x00,0x83,0x01,0x0e,0xf0,0x00,0x0f,0x00,0x00,0x83,0x01,0x0e,0xf8,0x01,0x1e,0x3c,0x00,0x83,0x01,0x0e,0x9c,0x03,0x3c,0xfc,0xff,0x83,0x01,0x0e,0x0e,0x06,0x70,0xe0,0xff,0x83,0x01,0x0e,0x07,0x1c,0xe0,0x00,0xfc,0x83,0x01,0x8e,0x03,0x38,0xc0,0x01,0x00,0x83,0x01,0xce,0x01,0x70,0x80,0x07,0x00,0x83,0x01,0xce,0x00,0xf8,0x00,0x0f,0x00,0x83,0x01,0xce,0x00,0xfc,0x03,0x1e,0x00,0x83,0x01,0x0e,0x00,0x1e,0x07,0x78,0x00,0x83,0x01,0x0e,0x00,0x07,0x0e,0xf0,0x00,0x83,0x01,0x0e,0xc0,0x03,0x1c,0xc0,0x03,0x83,0x01,0x0e,0xe0,0x01,0x78,0x80,0x07,0x83,0x01,0x0e,0xf0,0x00,0xf0,0x00,0x0f,0x83,0x01,0x0e,0x78,0x00,0xe0,0x03,0x3e,0x83,0x01,0x0e,0x3c,0x00,0x80,0x07,0x78,0x83,0x01,0x06,0x1e,0x00,0x80,0x1f,0xf0,0x83,0x01,0x06,0xff,0xff,0xff,0xff,0xff,0x83,0x01,0x06,0xff,0xff,0xff,0xff,0xff,0x83,0x01,0x06,0x00,0x00,0x00,0x00,0x00,0x82,0x01,0x06,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x06,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x06,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0x01,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0x01,0xfc,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

// Optional display/weather only. Neither network worker nor display writes logs/settings.
struct WeatherRequest { char location[151]; };
struct WeatherResult {
  char location[151],place[48],message[48];
  bool ok=false; float airF=NAN; int code=-1,offset=0;
  time_t observed=0; uint32_t fetchedAt=0;
};
void weatherWorker(void*);
QueueHandle_t weatherRequests=nullptr,weatherResults=nullptr;
WeatherResult weather={};
String weatherLocation;
bool weatherPending=false;
uint32_t weatherAttempt=0,brandStarted=0,cycleStarted=0,connectedAt=0;
uint32_t displayIP=0;
// Public endpoints below are for personal, non-commercial bench use.
// Commercial distribution requires a licensed service or your own weather proxy.
static const char WEATHER_CA[] PROGMEM=R"cert(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)cert";
String urlEncode(const String& value){
  String out;const char hex[]="0123456789ABCDEF";
  for(size_t i=0;i<value.length();i++){
    uint8_t c=value[i];
    if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=(char)c;
    else {out+='%';out+=hex[c>>4];out+=hex[c&15];}
  }return out;
}
bool weatherJSON(const String& url,JsonDocument& doc){
  WiFiClientSecure client;client.setCACert(WEATHER_CA);client.setHandshakeTimeout(8);
  HTTPClient http;http.useHTTP10(true);http.setConnectTimeout(5000);http.setTimeout(5000);
  if(!http.begin(client,url))return false;
  http.setUserAgent("SouthernBluegill-BenchLogger/8");
  int status=http.GET();
  if(status!=200 || http.getSize()>16384){http.end();return false;}
  // HTTP/1.0 avoids chunked transfer framing in the JSON stream.
  DeserializationError error=deserializeJson(doc,http.getStream());
  http.end();return !error;
}
const char* weatherLabel(int c){
  if(c==0)return "CLEAR";if(c<=2)return "PARTLY CLOUDY";if(c==3)return "CLOUDY";
  if(c==45||c==48)return "FOG";if(c>=51&&c<=57)return "DRIZZLE";
  if((c>=61&&c<=67)||(c>=80&&c<=82))return "RAIN";
  if((c>=71&&c<=77)||(c>=85&&c<=86))return "SNOW";
  if(c>=95&&c<=99)return "THUNDERSTORM";return "UNAVAILABLE";
}
void weatherWorker(void*){
  WeatherRequest req;
  char cachedLocation[151]={},cachedPlace[48]={};float lat=0,lon=0;bool resolved=false;
  for(;;){
    if(xQueueReceive(weatherRequests,&req,portMAX_DELAY)!=pdTRUE)continue;
    WeatherResult result={};strlcpy(result.location,req.location,sizeof(result.location));
    strlcpy(result.message,"Weather unavailable",sizeof(result.message));
    if(strcmp(cachedLocation,req.location)!=0){resolved=false;strlcpy(cachedLocation,req.location,sizeof(cachedLocation));}
    if(!resolved){
      JsonDocument geo;
      String url="https://geocoding-api.open-meteo.com/v1/search?name="+urlEncode(req.location)+"&count=10&language=en&countryCode=US";
      if(weatherJSON(url,geo)){
        JsonArray places=geo["results"].as<JsonArray>();
        String query=req.location;int comma=query.indexOf(',');if(comma>=0)query=query.substring(0,comma);query.trim();
        bool zip=query.length()==5;for(size_t i=0;i<query.length();i++)zip=zip&&isDigit(query[i]);
        JsonObject chosen;unsigned matches=0;
        for(JsonObject candidate:places){
          String name=candidate["name"]|"",feature=candidate["feature_code"]|"";
          bool match=feature.startsWith("PPL")&&name.equalsIgnoreCase(query);
          if(zip){for(JsonVariant post:candidate["postcodes"].as<JsonArray>())if(query==post.as<String>())match=true;}
          if(match){chosen=candidate;matches++;}
        }
        // Never silently choose among multiple cities. Ask for a ZIP if ambiguous.
        if(matches==1){
          JsonObject place=chosen;
          if(place["latitude"].is<double>()&&place["longitude"].is<double>()){
            lat=place["latitude"].as<float>();lon=place["longitude"].as<float>();
            String name=String(place["name"]|"")+", "+String(place["admin1"]|"");
            name.toCharArray(cachedPlace,sizeof(cachedPlace));resolved=true;
          }
        }else strlcpy(result.message,"Use ZIP for weather",sizeof(result.message));
      }
    }
    if(resolved){
      JsonDocument doc;
      String url="https://api.open-meteo.com/v1/forecast?latitude="+String(lat,5)+"&longitude="+String(lon,5)+"&current=temperature_2m,weather_code&temperature_unit=fahrenheit&timezone=auto&timeformat=unixtime";
      if(weatherJSON(url,doc)){
        JsonObject current=doc["current"];
        if(current["temperature_2m"].is<double>()&&current["weather_code"].is<int>()&&current["time"].is<long>()&&doc["utc_offset_seconds"].is<int>()){
          result.airF=current["temperature_2m"].as<float>();result.code=current["weather_code"];
          result.observed=current["time"].as<time_t>();result.offset=doc["utc_offset_seconds"];
          time_t now=time(nullptr);
          result.ok=isfinite(result.airF)&&result.airF>=-150&&result.airF<=150&&
            result.code>=0&&result.code<=99&&abs(result.offset)<=14*3600&&
            result.observed<=now+900&&result.observed>=now-3*3600;
          if(result.ok){result.fetchedAt=millis();strlcpy(result.place,cachedPlace,sizeof(result.place));result.message[0]=0;}
        }
      }
    }
    xQueueOverwrite(weatherResults,&result);
  }
}
void startWeatherWorker(){
  if(!oledReady)return;
  weatherRequests=xQueueCreate(1,sizeof(WeatherRequest));weatherResults=xQueueCreate(1,sizeof(WeatherResult));
  if(!weatherRequests||!weatherResults){Serial.println("Weather queue unavailable; logging continues");return;}
  if(xTaskCreate(weatherWorker,"oled-weather",12288,nullptr,1,nullptr)!=pdPASS){
    vQueueDelete(weatherRequests);vQueueDelete(weatherResults);weatherRequests=nullptr;weatherResults=nullptr;
    Serial.println("Weather worker unavailable; logging continues");
  }
}
void serviceWeather(){
  if(!weatherRequests||!weatherResults)return;
  String location=deploymentSet?String(deployment.location):String("");location.trim();
  bool changed=location!=weatherLocation;
  if(changed){weatherLocation=location;weather={};weatherAttempt=0;}
  WeatherResult result;
  if(xQueueReceive(weatherResults,&result,0)==pdTRUE){
    weatherPending=false;
    if(location==String(result.location)){
      if(result.ok || !weather.ok)weather=result;
      else strlcpy(weather.message,result.message,sizeof(weather.message));
      Serial.printf("OLED weather: %s\n",result.ok?result.place:result.message);
    }
  }
  uint32_t now=millis();
  // Backoff also applies after failure. Only the worker performs HTTP/TLS/DNS.
  uint32_t interval=weather.ok&&!weather.message[0]?1800000UL:300000UL;
  if(!weatherPending&&location.length()&&WiFi.status()==WL_CONNECTED&&!setupActive&&time(nullptr)>1700000000&&
     (!weatherAttempt||now-weatherAttempt>=interval)){
    WeatherRequest req={};location.toCharArray(req.location,sizeof(req.location));
    if(xQueueOverwrite(weatherRequests,&req)==pdPASS){weatherPending=true;weatherAttempt=now?now:1;}
  }
}
void centeredText(int baseline,String text){
  // OLED uses ASCII fonts. Replace unsupported UTF-8 bytes without mis-sizing strings.
  for(size_t i=0;i<text.length();i++)if((uint8_t)text[i]>126||(uint8_t)text[i]<32)text.setCharAt(i,' ');
  if(oled.getStrWidth(text.c_str())>128){
    while(text.length()&&oled.getStrWidth((text+"...").c_str())>128)text.remove(text.length()-1);
    text+="...";
  }
  int width=oled.getStrWidth(text.c_str());oled.drawStr((128-width)/2,baseline,text.c_str());
}
void drawBrand(){
  if(!oledReady)return;
  oled.clearBuffer();oled.drawXBMP(35,5,58,54,brandBitmap);oled.sendBuffer();
}
void renderOLED(){
  if(!oledReady||!screenOn)return;
  uint32_t now=millis();
  if(now-brandStarted<3000UL){drawBrand();return;}
  oled.clearBuffer();oled.setFont(u8g2_font_6x10_tf);
  if(setupActive){
    centeredText(10,setupSucceeded?"WI-FI CONNECTED":"WI-FI SETUP");oled.drawHLine(0,14,128);
    if(setupSucceeded){centeredText(28,"Rejoin home Wi-Fi");centeredText(43,WiFi.localIP().toString());centeredText(61,"Open in your browser");}
    else {centeredText(27,"Join this network:");centeredText(39,setupSSID);centeredText(51,WiFi.softAPIP().toString());centeredText(63,"Stay connected");}
  }else{
    uint32_t cycle=(now-cycleStarted)%27000UL;
    bool connected=WiFi.status()==WL_CONNECTED;
    bool showAddress=connected&&(now-connectedAt<10000UL);
    if(showAddress||(cycle>=18000UL&&cycle<24000UL)){
      centeredText(10,connected?"WI-FI CONNECTED":"WI-FI OFFLINE");oled.drawHLine(0,14,128);
      centeredText(32,connected?"OPEN DASHBOARD":"Logging continues");
      if(connected){oled.setFont(u8g2_font_9x15_tf);String ip=WiFi.localIP().toString();if(oled.getStrWidth(ip.c_str())>128)oled.setFont(u8g2_font_6x10_tf);centeredText(53,ip);}
      else centeredText(53,"Hold USER for setup");
    }else if(cycle>=24000UL){oled.drawXBMP(35,5,58,54,brandBitmap);}
    else if(cycle>=10000UL){
      centeredText(10,"LOCAL TIME / WEATHER");oled.drawHLine(0,14,128);
      time_t epoch=time(nullptr);struct tm clock={};char clockText[20]="Syncing clock";
      if(epoch>1700000000){
        if(weather.ok){epoch+=weather.offset;gmtime_r(&epoch,&clock);}
        else localtime_r(&epoch,&clock); // Central fallback is labeled until location resolves.
        strftime(clockText,sizeof(clockText),"%I:%M %p",&clock);
      }
      oled.setFont(u8g2_font_9x15_tf);centeredText(32,clockText);oled.setFont(u8g2_font_6x10_tf);
      if(weather.ok){
        bool stale=time(nullptr)-weather.observed>5400;
        centeredText(46,stale?"WEATHER STALE":"AIR "+String(weather.airF,0)+" F "+weatherLabel(weather.code));
        unsigned long age=(millis()-weather.fetchedAt)/60000UL;
        centeredText(62,String(stale?"Last fetch ":"Updated ")+age+" min ago");
      }else{
        centeredText(46,!weatherLocation.length()?"Set location on web":(weather.message[0]?weather.message:"Getting weather..."));
        centeredText(62,epoch>1700000000?"Clock: US Central":"Waiting for internet");
      }
    }else{
      String title=oledTitle();if(!title.length())title="WATER TEMPERATURE";
      centeredText(10,title);oled.drawHLine(0,14,128);
      if(haveRx){
        String reading=lastTemp=="FAULT"?"SENSOR ERROR":lastTemp+" F";oled.setFont(u8g2_font_logisoso24_tf);
        if(oled.getStrWidth(reading.c_str())>128)oled.setFont(u8g2_font_9x15_tf);
        centeredText(43,reading);oled.setFont(u8g2_font_6x10_tf);
        unsigned long minutes=(receiverSeconds()-lastRxSeconds)/60ULL;
        centeredText(62,storageError?"LOG SAVE ERROR":(minutes>=35?"OVERDUE "+String(minutes)+"m":String(minutes)+"m ago | "+(lastBatt.length()?lastBatt+"V":"--V")));
      }else {centeredText(35,"No reports yet");centeredText(57,radioReady?"LoRa listening":"Radio retrying");}
    }
  }
  // An error remains visible on every normal screen, including weather/address/logo.
  if(storageError){oled.setFont(u8g2_font_6x10_tf);oled.setDrawColor(0);oled.drawBox(0,54,128,10);oled.setDrawColor(1);centeredText(63,"LOG SAVE ERROR");}
  oled.sendBuffer();
}
void serviceOLED(){
  uint32_t now=millis();uint32_t ip=WiFi.status()==WL_CONNECTED?(uint32_t)WiFi.localIP():0;
  if(ip!=displayIP){displayIP=ip;connectedAt=now;cycleStarted=now;}
  // Start the normal cycle after the startup/connected-address screens finish.
  if(now-brandStarted<3000UL || (ip&&now-connectedAt<10000UL))cycleStarted=now;
  static uint32_t refreshed=0;
  if(now-refreshed>=1000UL){refreshed=now;renderOLED();}
}

String oledTitle(){
  if(!deploymentSet)return "Water temperature";
  if(deployment.waterType==0)return "Bench test";
  return String(deployment.name);
}
String lastNode="";
void noteProbe(const String& node){lastNode=node;bindDeploymentToProbe(node);}
uint32_t deploymentChecksum(const Deployment& d){
  const uint8_t* p=(const uint8_t*)&d;uint32_t h=2166136261UL;
  for(size_t i=0;i<offsetof(Deployment,checksum);i++){h^=p[i];h*=16777619UL;}
  return h;
}
bool deploymentFilename(const char* name,uint32_t& revision){
  const char* b=strrchr(name,'/');b=b?b+1:name;
  if(strlen(b)!=18 || strncmp(b,"dep-",4) || strcmp(b+14,".cfg"))return false;
  uint64_t v=0;for(int i=4;i<14;i++){if(b[i]<'0'||b[i]>'9')return false;v=v*10+(b[i]-'0');}
  if(v==0||v>UINT32_MAX)return false;revision=(uint32_t)v;return true;
}
void deploymentPath(uint32_t revision,char* path,size_t size){snprintf(path,size,"/deployments/dep-%010lu.cfg",(unsigned long)revision);}
bool readDeployment(const char* path,Deployment& d){
  File f=LittleFS.open(path,FILE_READ);if(!f)return false;
  bool ok=false;
  if(f.size()==sizeof(d)){
    ok=f.read((uint8_t*)&d,sizeof(d))==sizeof(d) && d.magic==DEPLOYMENT_MAGIC && d.revision>0 && d.waterType<=4 &&
      memchr(d.node,0,sizeof(d.node))&&memchr(d.name,0,sizeof(d.name))&&memchr(d.location,0,sizeof(d.location))&&memchr(d.effectiveUTC,0,sizeof(d.effectiveUTC))&&d.checksum==deploymentChecksum(d);
  }else if(f.size()==sizeof(LegacyDeployment)){
    LegacyDeployment old={};ok=f.read((uint8_t*)&old,sizeof(old))==sizeof(old);
    const uint8_t* bytes=(const uint8_t*)&old;uint32_t h=2166136261UL;
    for(size_t i=0;i<offsetof(LegacyDeployment,checksum);i++){h^=bytes[i];h*=16777619UL;}
    ok=ok&&old.magic==0x42474431&&old.revision>0&&old.waterType<=4&&memchr(old.node,0,sizeof(old.node))&&memchr(old.name,0,sizeof(old.name))&&memchr(old.effectiveUTC,0,sizeof(old.effectiveUTC))&&old.checksum==h;
    if(ok){
      memset(&d,0,sizeof(d));d.magic=DEPLOYMENT_MAGIC;d.revision=old.revision;d.receiverSession=old.receiverSession;d.uptimeMs=old.uptimeMs;
      strcpy(d.node,old.node);strcpy(d.name,old.name);strcpy(d.effectiveUTC,old.effectiveUTC);d.waterType=old.waterType;d.hasCoordinates=old.hasCoordinates;d.hasDepth=old.hasDepth;d.depthFeet=old.depthFeet;
      d.latitude=old.latitude;d.longitude=old.longitude;d.depthM=old.depthM;d.checksum=deploymentChecksum(d);
    }
  }
  f.close();return ok;
}
void loadDeployment(){
  if(!flashOk)return;
  File dir=LittleFS.open("/deployments");if(!dir||!dir.isDirectory())return;
  File f=dir.openNextFile();
  while(f){uint32_t rev;
    if(!f.isDirectory() && deploymentFilename(f.name(),rev)){
      if(rev>deploymentHigh)deploymentHigh=rev;
      char path[48];deploymentPath(rev,path,sizeof(path));Deployment d={};
      if(readDeployment(path,d) && d.revision==rev){if(!deploymentSet||rev>deployment.revision){deployment=d;deploymentSet=true;}}
      else deploymentHistoryWarning=true;
    }
    f.close();f=dir.openNextFile();
  }
  dir.close();
}
const char* waterTypeName(uint8_t type){
  switch(type){case 1:return "Pond";case 2:return "Lake / reservoir";case 3:return "River / stream";case 4:return "Other water";default:return "Bench / air test";}
}
String deploymentJSON(){
  String data=String("{\"configured\":")+(deploymentSet?"true":"false")+",\"latest_probe\":"+jsonText(lastNode)+
    ",\"history_warning\":"+(deploymentHistoryWarning?"true":"false")+",\"token\":"+jsonText(clearToken);
  if(deploymentSet){
    data+=",\"revision\":"+String(deployment.revision)+",\"node\":"+jsonText(deployment.node)+",\"name\":"+jsonText(deployment.name)+",\"location\":"+jsonText(deployment.location)+
      ",\"water_type\":"+String((unsigned)deployment.waterType)+",\"latitude\":"+(deployment.hasCoordinates?String(deployment.latitude,6):String("null"))+
      ",\"longitude\":"+(deployment.hasCoordinates?String(deployment.longitude,6):String("null"))+
      ",\"depth\":"+(deployment.hasDepth?String(deployment.depthFeet?deployment.depthM/0.3048:deployment.depthM,4):String("null"))+
      ",\"unit\":"+jsonText(deployment.depthFeet?"ft":"m")+",\"effective_utc\":"+jsonText(deployment.effectiveUTC);
  }
  return data+"}";
}
void settingsReply(int code,const String& message){
  server.sendHeader("Cache-Control","no-store");server.send(code,"application/json",String("{\"message\":")+jsonText(message)+"}");
}
bool settingNumber(const String& text,double& value){
  if(!text.length())return false;
  char* end;value=strtod(text.c_str(),&end);
  return end!=text.c_str() && *end=='\0' && isfinite(value);
}
bool settingTextOK(const String& value,size_t limit){
  if(value.length()>limit)return false;
  for(unsigned i=0;i<value.length();i++)if((unsigned char)value[i]<32 || (unsigned char)value[i]==127)return false;
  return true;
}
struct ProbeCalibration { char node[17]; int16_t tenths; };
ProbeCalibration calibration={};
void loadCalibration(){
  Preferences prefs;
  if(!prefs.begin("probe-cal",true))return;
  ProbeCalibration saved={};
  if(prefs.getBytes("offset",&saved,sizeof(saved))==sizeof(saved) &&
     saved.node[16]==0 && saved.tenths>=-100 && saved.tenths<=100)calibration=saved;
  prefs.end();
}
float probeOffset(const String& node){
  return node==calibration.node?calibration.tenths/10.0f:0.0f;
}
void handleCalibrationGet(){
  if(!dashboardAllowed())return;
  String node=lastNode.length()?lastNode:String(calibration.node);
  if(node=="legacy")node="";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json",String("{\"node\":")+jsonText(node)+
    ",\"offset\":"+String(probeOffset(node),1)+",\"configured_probe\":"+jsonText(calibration.node)+
    ",\"token\":"+jsonText(clearToken)+"}");
}
void handleCalibrationSave(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before saving.");return;}
  String node=server.arg("node");
  String current=lastNode.length()?lastNode:String(calibration.node);
  if(!node.length()||node=="legacy"||node!=current){settingsReply(409,"The active probe changed or has not reported yet. Reload Settings.");return;}
  double offset;
  if(!settingNumber(server.arg("offset"),offset)||offset<-10||offset>10||
     fabs(offset*10-round(offset*10))>0.000001){
    settingsReply(400,"Enter an offset from -10.0 to +10.0 °F in steps of 0.1.");return;
  }
  ProbeCalibration next={};
  node.toCharArray(next.node,sizeof(next.node));next.tenths=(int16_t)lround(offset*10);
  Preferences prefs;
  if(!prefs.begin("probe-cal",false)){settingsReply(500,"Could not open calibration storage.");return;}
  size_t written=prefs.putBytes("offset",&next,sizeof(next));prefs.end();
  if(written!=sizeof(next)){settingsReply(500,"Could not save calibration. Previous offset remains active.");return;}
  calibration=next;
  settingsReply(200,"Offset saved. It applies to new readings from this probe.");
}

void handleDeploymentGet(){
  if(!dashboardAllowed())return;
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",deploymentJSON());
}
void handleDeploymentSave(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before saving.");return;}
  if(!flashOk){settingsReply(503,"Internal storage is unavailable. Settings were not changed.");return;}
  if(deploymentHistoryWarning){settingsReply(409,"A deployment history record is damaged. Save the history and investigate before changing settings.");return;}
  String node=lastNode.length()?lastNode:(deploymentSet?String(deployment.node):String(""));
  String name=server.arg("water_body_name"),location=server.arg("location"),type=server.arg("water_type"),depth=server.arg("depth"),unit=server.arg("unit");
  name.trim();location.trim();
  if(!location.length()||!settingTextOK(location,150)){settingsReply(400,"Enter your nearest city and state, or ZIP code (up to 150 bytes).");return;}
  if(!settingTextOK(name,150)){settingsReply(400,"Water body name must be 150 bytes or fewer, without line breaks. It may be left blank.");return;}
  if(type.length()!=1||type[0]<'0'||type[0]>'4'||(unit!="ft"&&unit!="m")){settingsReply(400,"Choose a measurement type and depth unit.");return;}
  Deployment d={};d.magic=DEPLOYMENT_MAGIC;d.waterType=type[0]-'0';d.depthFeet=unit=="ft";
  double value=0;
  if(depth.length()){
    if(!settingNumber(depth,value)||value<0){settingsReply(400,"Depth must be a non-negative number, or blank if unknown.");return;}
    d.hasDepth=1;d.depthM=unit=="ft"?(double)value*0.3048:value;
    if(d.depthM>12000){settingsReply(400,"Depth exceeds 12,000 meters. Check the value and unit.");return;}
  }
  name.toCharArray(d.name,sizeof(d.name));location.toCharArray(d.location,sizeof(d.location));
  node.toCharArray(d.node,sizeof(d.node));
  String error;if(!commitDeployment(d,error)){settingsReply(500,error);return;}
  settingsReply(200,"Settings saved. New readings will include this location and depth. Earlier readings are unchanged.");
}
bool commitDeployment(Deployment& d,String& error){
  if(deploymentHigh==UINT32_MAX){error="Settings history is full. Previous settings retained.";return false;}
  if(!flashOk||LittleFS.totalBytes()<=LittleFS.usedBytes()+storageReserve(false)+sizeof(d)+4096){error="Not enough internal storage to save settings safely.";return false;}
  if(!LittleFS.exists("/deployments")&&!LittleFS.mkdir("/deployments")){error="Could not create settings history.";return false;}
  d.revision=deploymentHigh+1;d.receiverSession=receiverSession;d.uptimeMs=millis();timestampNow(d.effectiveUTC,sizeof(d.effectiveUTC));d.checksum=deploymentChecksum(d);
  char path[48];deploymentPath(d.revision,path,sizeof(path));
  if(LittleFS.exists(path)){error="History entry already exists. Restart before retrying.";return false;}
  File f=LittleFS.open("/deployments/pending.tmp",FILE_WRITE);
  if(!f){error="Could not save settings. Previous settings retained.";return false;}
  size_t written=f.write((const uint8_t*)&d,sizeof(d));f.flush();f.close();Deployment check={};
  if(written!=sizeof(d)||!readDeployment("/deployments/pending.tmp",check)||!LittleFS.rename("/deployments/pending.tmp",path)){
    error="Settings save failed. Previous settings retained.";return false;
  }
  deployment=d;deploymentHigh=d.revision;deploymentSet=true;return true;
}
void bindDeploymentToProbe(const String& node){
  // A first-time user can save settings before the next 15-minute radio report.
  // Bind an unassigned configuration once, to the first received transmitter.
  if(!deploymentSet||deploymentHistoryWarning||deployment.node[0]||!node.length())return;
  Deployment bound=deployment;node.toCharArray(bound.node,sizeof(bound.node));String error;
  if(!commitDeployment(bound,error))Serial.println("Could not attach saved settings to transmitter; readings remain unassigned.");
}

String csvCell(const String& value){String out="\"";for(unsigned i=0;i<value.length();i++){if(value[i]=='"')out+='"';out+=value[i];}return out+"\"";}
String deploymentCSV(const String& node){
  if(!deploymentSet || deploymentHistoryWarning || node!=String(deployment.node))return ",,,,,,,,,";
  return String(",")+String(deployment.revision)+","+csvCell(deployment.name)+","+csvCell(waterTypeName(deployment.waterType))+","+
    (deployment.hasCoordinates?String(deployment.latitude,6):String(""))+","+
    (deployment.hasCoordinates?String(deployment.longitude,6):String(""))+","+
    (deployment.hasDepth?String(deployment.depthM,4):String(""))+","+csvCell(deployment.effectiveUTC)+","+csvCell(deployment.location)+","+(deployment.location[0]?"city_state_or_zip":"");
}

void handleDeploymentHistory(){
  if(!dashboardAllowed())return;
  server.sendHeader("Cache-Control","no-store");server.sendHeader("Content-Disposition","attachment; filename=deployment_history.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200,"text/csv","revision,probe_id,water_body_name,water_type,latitude,longitude,depth_m,effective_utc,receiver_session,uptime_ms,location_name,location_basis\n");
  if(flashOk){File dir=LittleFS.open("/deployments");if(dir&&dir.isDirectory()){
    File f=dir.openNextFile();while(f){uint32_t rev;
      if(!f.isDirectory()&&deploymentFilename(f.name(),rev)){
        char path[48];deploymentPath(rev,path,sizeof(path));Deployment d={};
        if(readDeployment(path,d)&&d.revision==rev){
          String line=String(rev)+","+csvCell(d.node)+","+csvCell(d.name)+","+csvCell(waterTypeName(d.waterType))+","+
            (d.hasCoordinates?String(d.latitude,6):String(""))+","+(d.hasCoordinates?String(d.longitude,6):String(""))+","+
            (d.hasDepth?String(d.depthM,4):String(""))+","+csvCell(d.effectiveUTC)+","+String(d.receiverSession)+","+String(d.uptimeMs)+","+csvCell(d.location)+","+(d.location[0]?"city_state_or_zip":"")+"\n";
          server.sendContent(line);
        }
      }f.close();f=dir.openNextFile();
    }dir.close();
  }}
  server.sendContent("");
}
const char SETTINGS_PAGE[] PROGMEM=R"settingshtml(
<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Logger settings</title>
<style>body{font:16px/1.5 system-ui;margin:24px auto;padding:0 16px;max-width:620px;background:#eef3f6;color:#183047}section{background:white;padding:24px;border-radius:12px;border-top:4px solid #1d6873}label{display:block;margin:18px 0 6px;font-weight:600}input,select,button{font:inherit;box-sizing:border-box;min-height:44px;border-radius:6px;padding:10px;max-width:100%}input,select{width:100%;border:1px solid #789;background:white;color:#183047}button{background:#1d5361;color:white;border:0;margin-top:22px;padding:12px 22px;cursor:pointer}button:disabled{opacity:.55}.pair{display:grid;grid-template-columns:1fr 140px;gap:14px}.hint{color:#526778;font-size:14px;margin-top:6px}:focus-visible{outline:3px solid #efae32;outline-offset:3px}#feedback{min-height:1.5em;font-weight:600}details{margin-top:20px}summary{cursor:pointer;min-height:44px;align-content:center}@media(max-width:400px){.pair{grid-template-columns:1fr 110px}section{padding:18px}}</style>
<p><a href="/">← Back to graphs</a></p><h1>Logger settings</h1><section>
<p>Set the location and depth for your readings.</p>
<form id="settings"><fieldset id="fields" disabled style="border:0;padding:0;margin:0">
<label for="location">City, state or ZIP code</label><input id="location" required maxlength="150" autocomplete="off" placeholder="Tupelo, MS or 38801"><p class="hint">Use the nearest city or ZIP to the water you are measuring.</p>
<label for="water_body_name">Water body name <span class="hint">(optional)</span></label><input id="water_body_name" maxlength="150" placeholder="For example: Back pond or Lake Lamar Bruce">
<label for="water_type">Measuring</label><select id="water_type"><option value="0">Testing — air / bench</option><option value="1">Pond</option><option value="2">Lake / reservoir</option><option value="3">River / stream</option><option value="4">Other water</option></select>
<div class="pair"><div><label for="depth">Depth below surface</label><input id="depth" type="number" min="0" step="any" placeholder="Optional"></div><div><label for="unit">Unit</label><select id="unit"><option value="ft">Feet</option><option value="m">Meters</option></select></div></div><p class="hint">Leave depth blank during an air test or if unknown.</p>
<button id="save" type="submit">Save settings</button></fieldset></form>
<p id="feedback" role="status" aria-live="polite"></p><p id="current" class="hint"></p>
<p class="hint">Settings are saved on this receiver and included with new readings. Previous readings keep their original details.</p>
<details><summary>Settings history</summary><p><a href="/deployment-history.csv">Download settings history</a></p><p class="hint">Saved changes are retained across restarts and clearing the current log. Wi-Fi settings are managed separately by holding USER for five seconds.</p></details>
</section><section id="calibration"><h2>Probe calibration</h2>
<p class="hint">Compare the probe and a trusted thermometer in the same stirred water after both settle. Enter reference minus probe temperature: if the probe reads 1.2 °F high, enter −1.2.</p>
<form id="calibrate"><label for="offset">Temperature offset (°F)</label><input id="offset" type="number" step="0.1" min="-10" max="10" required disabled><button id="saveOffset" disabled>Save offset</button></form>
<p id="calFeedback" role="status"></p><p class="hint">The receiver stores one probe offset. Saving for a different probe replaces the previous setting. Use 0 to disable the correction. Earlier readings are unchanged.</p>
</section><script>
const $=id=>document.getElementById(id);let token='',unit='ft',busy=false;
async function load(populate=true){const r=await fetch('/deployment',{cache:'no-store'});if(!r.ok)throw Error('Could not load settings');const s=await r.json();token=s.token;
if(populate){$('location').value=s.configured?s.location||'':'';$('water_body_name').value=s.configured?s.name||'':'';$('water_type').value=s.configured?String(s.water_type):'0';$('depth').value=s.configured&&s.depth!==null?s.depth:'';$('unit').value=s.configured?s.unit:'ft';unit=$('unit').value;}
$('current').textContent=s.configured?(s.location?'Saved location: '+s.location:'Add your city, state or ZIP code above.'):'No settings saved yet.';
$('fields').disabled=s.history_warning;if(s.history_warning)$('feedback').textContent='A saved settings record could not be read. Download the history before making changes.';
}
$('unit').onchange=()=>{const next=$('unit').value;if($('depth').value!==''){const v=Number($('depth').value);if(Number.isFinite(v))$('depth').value=String(Number((next==='m'?v*0.3048:v/0.3048).toFixed(6)));}unit=next;};
$('settings').onsubmit=async e=>{e.preventDefault();if(busy)return;busy=true;$('save').disabled=true;$('feedback').textContent='Saving...';try{
const body=new URLSearchParams();for(const key of ['location','water_body_name','water_type','depth','unit'])body.set(key,$(key).value);
const r=await fetch('/deployment',{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Settings-Token':token},body});const data=await r.json();if(!r.ok)throw Error(data.message||'Could not save settings');await load(false);$('feedback').textContent='Settings saved. New readings will use these details.';
}catch(e){$('feedback').textContent=e.message;}finally{busy=false;$('save').disabled=false;}};
let cal=null;
async function loadCal(){
 const r=await fetch('/calibration',{cache:'no-store'});if(!r.ok)throw Error('Could not load calibration.');
 cal=await r.json();$('offset').value=cal.offset.toFixed(1);$('offset').disabled=$('saveOffset').disabled=!cal.node;
 $('calFeedback').textContent=cal.node?'Probe: '+cal.node+(cal.configured_probe&&cal.configured_probe!==cal.node?' · Saving replaces the offset for '+cal.configured_probe:''):'Wait for an identified probe to send a reading, then reload Settings.';
}
$('calibrate').onsubmit=async e=>{
 e.preventDefault();if(!cal||!cal.node)return;$('saveOffset').disabled=true;
 try{
  const body=new URLSearchParams({node:cal.node,offset:$('offset').value});
  const r=await fetch('/calibration',{method:'POST',headers:{'X-Settings-Token':cal.token},body});
  const result=await r.json();if(!r.ok)throw Error(result.message);$('calFeedback').textContent=result.message;
 }catch(e){$('calFeedback').textContent=e.message;}finally{$('saveOffset').disabled=false;}
};
loadCal().catch(e=>$('calFeedback').textContent=e.message);
load().catch(e=>{$('feedback').textContent=e.message+'. Reload to try again.';});
</script></html>
)settingshtml";
void handleSettingsPage(){
  if(!dashboardAllowed())return;
  server.sendHeader("Cache-Control","no-store");server.send(200,"text/html",FPSTR(SETTINGS_PAGE));
}

// The web page. The browser downloads the CSV and draws the graphs itself.
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Water temperature logger</title>
<style>body{font:16px system-ui;margin:20px auto;padding:0 16px;max-width:900px;background:#eef3f6;color:#183047}section{background:white;border-radius:12px;padding:16px;margin:14px 0}button,a{margin:5px}button{padding:10px;border-radius:6px;border:1px solid #789;background:#fff}svg{width:100%;touch-action:pan-y}text{fill:#345;font-size:12px}.warn{color:#a21c16;font-weight:bold}#now{font-size:2.3rem;margin:0}.muted{color:#526778}button[aria-pressed="true"]{background:#183047;color:white;border-color:#183047}button:focus-visible,a:focus-visible{outline:3px solid #efae32;outline-offset:3px}button:active{transform:translateY(1px)}button:disabled{opacity:.55;cursor:wait}.feedback{min-height:1.5em;font-weight:600}.top{padding:10px 0}.top h1{margin:0 0 6px}.help{margin:6px 0 16px}.help summary{cursor:pointer;color:#245765}.reading{border-top:4px solid #216976}.eyebrow{font-size:.75rem;letter-spacing:.08em;font-weight:700;color:#526778;margin-top:0}button{cursor:pointer;min-height:42px}section{box-shadow:0 1px 2px #18304712}</style>
<header class="top"><h1>Water temperature logger</h1><p class="muted">Receiver revision 10 · Seeed ESP32S3 · Local logging</p><p><a href="/settings">Settings</a> · <a href="#logs">Logs and reporting</a></p></header><details class="help"><summary>Change Wi-Fi or use the receiver button</summary><p>To change Wi-Fi, hold the receiver USER button for 5 seconds, then join the Bluegill setup network. Logs are retained. Optional SH1106 display stays on and cycles. Short USER toggles; hold 5 seconds for Wi-Fi setup. Display weather: Open-Meteo / GeoNames; modeled air conditions, not probe data.</p></details><section class="reading"><p class="eyebrow">LATEST WATER TEMPERATURE</p><h2 id="now">Waiting for data</h2><p id="fresh"></p><p id="change"></p><p id="health"></p><p id="storage"></p><p id="receiverInfo" class="muted"></p><p>Deployment details are recorded per reading after Settings are saved.</p></section>
<section><button data-h="24" aria-pressed="true">24 hours</button><button data-h="168" aria-pressed="false">7 days</button><button data-h="0" aria-pressed="false">All current log</button><p id="rangeFeedback" class="feedback" role="status" aria-live="polite">Showing last 24 hours</p><p id="stats"></p><h2>Temperature (°F)</h2><svg id="temp" viewBox="0 0 700 270" role="img" aria-label="Temperature history"></svg><p id="detail">Tap a chart to inspect a reading.</p></section>
<section><h2>Field battery voltage (V)</h2><svg id="batt" viewBox="0 0 700 270" role="img" aria-label="Battery voltage history"></svg><p class="muted">Voltage is not a calibrated battery percentage.</p></section>
<section><h2>Radio link diagnostics</h2><p id="radioLatest">Waiting for a packet received this receiver boot.</p><p id="radioMargin" class="muted"></p><p id="reportGaps"></p><p id="reportTiming"></p><p class="muted">Gap statistics use the selected period in the current log. Sequence gaps are counted separately for each transmitter and boot ID, between its first and last observed sequence only. Receiver outages and failed saves can also cause gaps. No missing count is inferred across transmitter resets or beyond the latest report. Distance cannot be calculated reliably from RSSI.</p><svg id="rssi" viewBox="0 0 700 270" role="img" aria-label="Received signal strength in dBm"></svg><p>RSSI describes received packets; it does not establish packet delivery rate.</p></section>
<section id="logs"><h2>Logs and reporting</h2><button id="save">Download CSV</button><p><a href="https://thesouthernbluegill.com/observations/" target="_blank" rel="noopener noreferrer">Share a water-temperature observation with The Southern Bluegill</a></p><p class="muted">Download the CSV for your records and use the observation form to submit a field reading manually. The gateway does not send logs automatically.</p><button id="clear">Clear current log…</button><p><a href="/archives">Browse / save archives</a> · <a href="/pre-calibration.csv">Pre-calibration backup</a> · <a href="/previous.csv" download="pre_settings_log.csv">Save pre-settings log</a> · <a href="/legacy.csv" download="legacy.csv">Save original-format log</a></p><p class="muted">Save a copy before clearing. Clearing affects the current log on the active storage only. An archive or a log on another storage device remains intact.</p><p id="actionFeedback" class="feedback" role="status" aria-live="polite"></p><p id="notice"></p></section>
<script>
let rows=[],hours=24,busy=false,status=null;
const $=id=>document.getElementById(id),finite=Number.isFinite;
function parse(csv){return csv.trim().split(/\r?\n/).slice(1).map(l=>{const c=l.split(',');return {t:Date.parse(c[0]),temp:c[1]===''?NaN:Number(c[1]),rssi:Number(c[2]),batt:c[3]===''?NaN:Number(c[3]),quality:c[9],node:c[6],boot:c[7],seq:/^\d+$/.test(c[8]||'')?Number(c[8]):NaN};}).filter(r=>finite(r.t)).sort((a,b)=>a.t-b.t);}
function duration(seconds){
 if(!finite(seconds)||seconds<0)return 'unavailable';
 const days=Math.floor(seconds/86400),h=Math.floor(seconds%86400/3600),m=Math.floor(seconds%3600/60);
 return (days?days+'d ':'')+(h?h+'h ':'')+m+'m';
}
function linkStats(source){
 const groups=new Map(),nodes=new Map();let unsequenced=0,duplicates=0,missing=0,observed=0,longest=0,intervals=0;
 for(const r of source){
  if(!nodes.has(r.node))nodes.set(r.node,[]);nodes.get(r.node).push(r);
  if(!r.boot||!Number.isSafeInteger(r.seq)||r.seq<1||r.seq>4294967295){unsequenced++;continue;}
  const key=JSON.stringify([r.node,r.boot]);if(!groups.has(key))groups.set(key,new Set());
  const set=groups.get(key);if(set.has(r.seq))duplicates++;set.add(r.seq);
 }
 for(const set of groups.values()){const seq=[...set].sort((a,b)=>a-b);observed+=seq.length;for(let i=1;i<seq.length;i++)missing+=seq[i]-seq[i-1]-1;}
 for(const list of nodes.values()){const a=[...list].sort((a,b)=>a.t-b.t);for(let i=1;i<a.length;i++){const d=(a[i].t-a[i-1].t)/1000;if(d>0){longest=Math.max(longest,d);intervals++;}}}
 return {missing,observed,duplicates,unsequenced,groups:groups.size,nodes:nodes.size,longest,intervals};
}
function renderDiagnostics(){
 const selected=rows.filter(r=>!hours||r.t>=Date.now()-hours*3600000),d=linkStats(selected);
 $('reportGaps').textContent='Selected log: '+d.nodes+' transmitter(s) · '+d.groups+' sequenced run(s) · '+d.observed+' unique sequenced reports · '+d.missing+' missing sequence(s) · '+d.duplicates+' duplicate row(s)'+(d.unsequenced?' · '+d.unsequenced+' row(s) without usable sequence':'');
 $('reportTiming').textContent=d.intervals?'Longest interval between logged reports from the same transmitter: '+duration(d.longest)+' ('+(d.longest/60).toFixed(1)+' min; includes reset boundaries).':'More timestamped reports are needed to calculate intervals.';
 if(!status)return;
 $('receiverInfo').textContent='Receiver uptime: '+duration(status.receiver_uptime_s)+' · reset reason: '+(status.receiver_reset||'unavailable')+' · LoRa '+(status.radio_ready?'listening':'initialization failed; retrying')+' · 907.8 MHz / 500 kHz / SF9';
 const valid=status.received&&finite(status.rssi),stale=!status.received||status.age_s>22.5*60;
 $('radioLatest').textContent=valid?'Latest received packet: RSSI '+status.rssi.toFixed(1)+' dBm · SNR '+(finite(status.snr)?status.snr.toFixed(1)+' dB':'unavailable')+' · transmitter '+status.radio_node+' · '+duration(status.age_s)+' ago'+(stale?' — STALE':''):'Waiting for a packet received this receiver boot. Historical RSSI remains in the graph.';
 $('radioLatest').className=stale?'warn':'';
 $('radioMargin').textContent=valid&&finite(status.snr)?'SNR margin above the approximate SF9 decoding threshold (−12.5 dB): '+(status.snr+12.5).toFixed(1)+' dB. This is an indicative margin, not a delivery guarantee. SNR is live only; it is not stored in this CSV.':'RSSI: less negative is stronger. SNR: higher is better. Radio settings: 907.8 MHz / SF9 / 500 kHz. SNR is live only; it is not stored in this CSV.';
}
function draw(id,key,label){
 const svg=$(id),cut=hours?Date.now()-hours*3600000:-Infinity;
 const source=rows.filter(r=>r.t>=cut),pts=source.filter(r=>finite(r[key])&&(key!=='temp'||r.quality==='ok'));
 svg.replaceChildren();if(!pts.length){svg.innerHTML='<text x="50" y="80">No timestamped data in this period</text>';return;}
 const x0=source[0].t,x1=Math.max(source[source.length-1].t,x0+1000), vals=pts.map(r=>r[key]);
 let y0=Math.min(...vals),y1=Math.max(...vals);if(y1-y0<1){y0-=.5;y1+=.5;}else{let pad=(y1-y0)*.08;y0-=pad;y1+=pad;}
 const sx=t=>58+(t-x0)/(x1-x0)*625,sy=v=>230-(v-y0)/(y1-y0)*210;
 const stamp=t=>new Date(t).toLocaleString([],{month:'short',day:'numeric',hour:'numeric',minute:'2-digit'});
 const add=(tag,attrs,text)=>{let e=document.createElementNS('http://www.w3.org/2000/svg',tag);for(let [k,v]of Object.entries(attrs))e.setAttribute(k,v);if(text!==undefined)e.textContent=text;svg.append(e);return e;};
 for(let i=0;i<5;i++){let v=y0+(y1-y0)*i/4,y=sy(v);add('line',{x1:58,x2:683,y1:y,y2:y,stroke:'#dce4ea'});add('text',{x:2,y:y+4},v.toFixed(key==='batt'?2:1));}
 for(let i=0;i<3;i++){let t=x0+(x1-x0)*i/2;add('text',{x:sx(t),y:255,'text-anchor':i===0?'start':i===2?'end':'middle'},stamp(t));}
 // Keep extrema and segment boundaries. Break at faults and gaps > 22.5 minutes.
 let segments=[],seg=[],prev=null;
 for(let r of source){if(!finite(r[key])||(key==='temp'&&r.quality!=='ok')){if(seg.length)segments.push(seg);seg=[];prev=null;continue;}if(prev&&r.t-prev.t>22.5*60000){segments.push(seg);seg=[];}seg.push(r);prev=r;}if(seg.length)segments.push(seg);
 for(let s of segments){let reduced=[];const step=Math.max(1,Math.ceil(s.length/350));for(let i=0;i<s.length;i+=step){const b=s.slice(i,i+step);const picks=[b[0],b.reduce((a,r)=>r[key]<a[key]?r:a),b.reduce((a,r)=>r[key]>a[key]?r:a),b[b.length-1]];reduced.push(...[...new Set(picks)].sort((a,b)=>a.t-b.t));}add('polyline',{points:reduced.map(r=>sx(r.t)+','+sy(r[key])).join(' '),fill:'none',stroke:'#187caa','stroke-width':2});if(s.length===1)add('circle',{cx:sx(s[0].t),cy:sy(s[0][key]),r:3,fill:'#187caa'});}
 svg.onclick=e=>{let x=(e.clientX-svg.getBoundingClientRect().left)/svg.getBoundingClientRect().width*700,target=x0+(x-58)/625*(x1-x0);let r=pts.reduce((a,b)=>Math.abs(b.t-target)<Math.abs(a.t-target)?b:a);$('detail').textContent=new Date(r.t).toLocaleString()+' · '+label+': '+r[key].toFixed(key==='batt'?2:1)+' · node '+r.node;};
}
function render(){let good=rows.filter(r=>finite(r.temp)&&r.quality==='ok'),last=good[good.length-1];if(last){$('now').textContent=last.temp.toFixed(1)+' °F';$('fresh').textContent='Latest valid measurement received '+new Date(last.t).toLocaleString();let old=good.filter(r=>Math.abs(r.t-(last.t-86400000))<=22.5*60000).sort((a,b)=>Math.abs(a.t-(last.t-86400000))-Math.abs(b.t-(last.t-86400000)))[0];$('change').textContent=old?'Change from about 24 hours earlier: '+(last.temp-old.temp).toFixed(1)+' °F':'24-hour comparison not yet available';}else{$('now').textContent='No valid timestamped temperature';$('fresh').textContent='Pre-sync records remain in the CSV.';$('change').textContent='';}
let selected=good.filter(r=>!hours||r.t>=Date.now()-hours*3600000),v=selected.map(r=>r.temp);$('stats').textContent=v.length?'Selected period: '+v.length+' valid readings · min '+Math.min(...v).toFixed(1)+' · max '+Math.max(...v).toFixed(1)+' · mean '+(v.reduce((a,b)=>a+b,0)/v.length).toFixed(1)+' °F (sample mean)':'No valid temperatures in selected period';draw('temp','temp','Temperature °F');draw('batt','batt','Battery V');draw('rssi','rssi','RSSI dBm');renderDiagnostics();}
async function refresh(){if(busy)return;busy=true;try{const s=await fetch('/status',{cache:'no-store'});if(!s.ok)throw Error('Status request failed');status=await s.json();renderDiagnostics();let stale=!status.received||status.age_s>22.5*60;$('health').className=stale||status.storage_error?'warn':'';$('health').textContent=(stale?'No recent packet':'Packet received '+Math.floor(status.age_s/60)+' min ago')+' · '+(status.storage_error?'LOG WRITE ERROR':'Last save status OK')+' · saved this boot '+status.saved+' · rejected '+status.invalid;$('storage').className=status.near_full?'warn':'';$('storage').textContent=(status.near_full?'LOW STORAGE — save logs and free space. ':'')+status.storage+': '+(status.used/1024).toFixed(0)+' KiB used / '+(status.total/1024).toFixed(0)+' KiB total · '+((status.total-status.used)/1024).toFixed(0)+' KiB free';const r=await fetch('/log.csv',{cache:'no-store'});if(!r.ok)throw Error('Log unavailable');rows=parse(await r.text());render();$('notice').className='';$('notice').textContent='Updated '+new Date().toLocaleTimeString();}catch(e){$('notice').textContent=e.message+'; displayed data may be old';$('notice').className='warn';}finally{busy=false;}}
document.querySelectorAll('[data-h]').forEach(b=>b.onclick=()=>{
 hours=Number(b.dataset.h);
 document.querySelectorAll('[data-h]').forEach(other=>other.setAttribute('aria-pressed',String(other===b)));
 $('rangeFeedback').textContent=hours===0?'Showing all readings in the current log':'Showing last '+(hours===24?'24 hours':'7 days');
 if(rows.length<2)$('rangeFeedback').textContent+=' — only '+rows.length+' timestamped reading(s) available';
 render();
});
$('save').onclick=async()=>{const b=$('save');b.disabled=true;b.textContent='Preparing…';$('actionFeedback').textContent='Preparing CSV download…';try{
 const r=await fetch('/log.csv',{cache:'no-store'});if(!r.ok)throw Error('Download failed: HTTP '+r.status);
 const url=URL.createObjectURL(await r.blob());const a=document.createElement('a');a.href=url;a.download='water_temp_log.csv';document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),60000);
 $('actionFeedback').textContent='CSV download requested. Check your browser downloads before clearing the log.';
}catch(e){$('actionFeedback').textContent=e.message;}finally{b.disabled=false;b.textContent='Download CSV';}};
$('clear').onclick=async()=>{
 if(!confirm('Erase the current log on the active storage? Save your CSV first. This cannot be undone.')){$('actionFeedback').textContent='Clear cancelled. Log retained.';return;}
 const button=$('clear');button.disabled=true;button.textContent='Clearing…';$('actionFeedback').textContent='Clearing current log…';
 try{let r=await fetch('/clear',{method:'POST',headers:{'X-Log-Token':'TOKEN_VALUE'}});let text=await r.text();if(!r.ok)throw Error(text);await refresh();$('actionFeedback').textContent=text;}
 catch(e){$('actionFeedback').textContent='Clear failed: '+e.message;}
 finally{button.disabled=false;button.textContent='Clear current log…';}
};
refresh();setInterval(refresh,60000);
</script></html>

)rawliteral";

void handleRoot() {
  String page=FPSTR(PAGE);page.replace("TOKEN_VALUE",clearToken);
  server.sendHeader("Cache-Control","no-store");server.send(200,"text/html",page);
}

void handleCsv() {
  File f = logFS().open("/log-v5.csv", FILE_READ);
  if (!f) { server.send(404, "text/plain", "No log yet"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=water_temp_log.csv");
  server.sendHeader("Cache-Control","no-store");
  server.streamFile(f, "text/csv");
  f.close();
}

// A new receiver has no filesystem. Never auto-format a nonblank/corrupt partition.
bool flashPartitionIsErased() {
  const esp_partition_t* p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,NULL);
  if(!p)return false;
  uint8_t buf[512];
  for(size_t offset=0;offset<p->size;offset+=sizeof(buf)){
    size_t n=(p->size-offset<sizeof(buf))?p->size-offset:sizeof(buf);
    if(esp_partition_read(p,offset,buf,n)!=ESP_OK)return false;
    for(size_t i=0;i<n;i++)if(buf[i]!=0xFF)return false;
    yield();
  }
  return true;
}
bool mountFlashSafely() {
  if(LittleFS.begin(false))return true;
  if(!flashPartitionIsErased()){
    Serial.println("Storage not mounted. Existing bytes retained. For a NEW receiver only, type INIT NEW FLASH in Serial Monitor to ERASE its filesystem partition.");
    return false;
  }
  Serial.println("Initializing completely blank receiver filesystem");
  return LittleFS.format() && LittleFS.begin(false);
}
void serviceStorageSerial() {
  static String command;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\n'||c=='\r'){
      command.trim();
      if(command=="INIT NEW FLASH" && !flashOk && !sdOk){
        Serial.println("Explicit initialization requested: erasing only the log filesystem partition on THIS receiver.");
        flashOk=LittleFS.format() && LittleFS.begin(false);
        if(flashOk){loadDeployment();storageError=!initLog(logFS());}
        Serial.println(flashOk?"Storage initialized. Wi-Fi and firmware retained.":"Storage initialization FAILED. Check partition selection.");
      }
      command="";
    }else if(command.length()<40)command+=c;
  }
}
bool startRadio() {
  receivedFlag=false;
  pinMode(38,OUTPUT);digitalWrite(38,LOW);
  SPI.begin(7,8,9,41); // SCK, MISO, MOSI, NSS
  int16_t state=radio.begin(RADIO_MHZ,RADIO_BW_KHZ,RADIO_SF,RADIO_CR,
    RADIOLIB_SX126X_SYNC_WORD_PRIVATE,RADIO_DBM,8,1.8);
  if(state==RADIOLIB_ERR_NONE)state=radio.setDio2AsRfSwitch(true);
  if(state==RADIOLIB_ERR_NONE)state=radio.setCRC(true);
  if(state==RADIOLIB_ERR_NONE){
    radio.setRfSwitchPins(38,RADIOLIB_NC); // separate receive enable; DIO2 handles transmit switch
    radio.setDio1Action(setFlag);state=radio.startReceive();
  }
  if(state!=RADIOLIB_ERR_NONE){Serial.printf("Radio init/RX failed: %d; retry in 30 seconds\n",state);return false;}
  Serial.println("LoRa listening: 907.8 MHz / 500 kHz / SF9 / CR4/7");return true;
}

void setup() {
  Serial.begin(115200);
  delay(300); // USB CDC startup; no indefinite wait for a connected computer.
  Serial.println("Bluegill receiver revision 10 / XIAO ESP32S3 / SH1106 / Wi-Fi setup diagnostic");
  initOLED();brandStarted=millis();cycleStarted=brandStarted;screenStarted=millis();
  // Storage: internal flash, plus the SD card if one is connected
  flashOk=mountFlashSafely(); // auto-initialize ONLY a completely erased partition
  storageError=!flashOk;
  Serial.printf("Internal log storage: %u used of %u bytes\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
#if ENABLE_SD
  // Conservative SD bus speed for the first hardware test over jumper wires.
  // SD.begin uses its normal non-formatting mount; never erase a failed card.
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  Serial.printf("SD init: CS=%d MOSI=%d MISO=%d CLK=%d, SPI=1000000 Hz\n",
                SD_CS, SD_MOSI, SD_MISO, SD_SCK);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  unsigned long sdStarted = millis();
  sdOk = SD.begin(SD_CS, sdSPI, 1000000);
  Serial.printf("SD mount %s after %lu ms\n", sdOk ? "OK" : "FAILED",
                (unsigned long)(millis() - sdStarted));
  if (sdOk) {
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      Serial.println("SD mounted but no card type detected; using internal flash");
      SD.end();
      sdOk = false;
    } else {
      const char* cardName = cardType == CARD_MMC ? "MMC" :
                             cardType == CARD_SD ? "SDSC" :
                             cardType == CARD_SDHC ? "SDHC/SDXC" : "unknown";
      Serial.printf("SD card: %s, capacity %llu MiB\n", cardName,
                    (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
      Serial.printf("SD filesystem: %llu KiB used of %llu KiB\n",
                    (unsigned long long)(SD.usedBytes() / 1024ULL),
                    (unsigned long long)(SD.totalBytes() / 1024ULL));
      Serial.println("SD card ready; checking log file next");
    }
  } else {
    SD.end();
    Serial.println("SD initialization failed; using internal flash if available");
    Serial.println("Check 3.3V power, ground, CS/MOSI/MISO/CLK wiring and FAT32 card");
  }

#else
  Serial.println("SD disabled; using internal flash");
#endif

  storageError=!(sdOk || flashOk) || !initLog(logFS());
  Serial.printf("Active storage: %s; log initialization: %s\n",
                sdOk ? "SD" : (flashOk ? "Internal flash" : "Unavailable"),
                storageError ? "FAILED" : "OK");
  pinMode(USER_BUTTON,INPUT_PULLUP);
  clearToken=String(esp_random(),HEX)+String(esp_random(),HEX);
  receiverSession=esp_random();loadDeployment();loadCalibration();
  // Saved settings are independent of the log filesystem; no startup wait.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  char setupName[28];
  snprintf(setupName,sizeof(setupName),"Bluegill-Setup-%04X",(unsigned)(ESP.getEfuseMac() & 0xFFFF));
  setupSSID=setupName;
  loadWifiSettings();beginSavedWifi();
  configTzTime(TZ_INFO,"pool.ntp.org","time.nist.gov");

  server.on("/",HTTP_GET,[](){if(onSetupNetwork())handleWifiPage();else handleRoot();});
  server.on("/settings",HTTP_GET,handleSettingsPage);
  server.on("/calibration",HTTP_GET,handleCalibrationGet);
  server.on("/calibration",HTTP_POST,handleCalibrationSave);
  server.on("/deployment",HTTP_GET,handleDeploymentGet);
  server.on("/deployment",HTTP_POST,handleDeploymentSave);
  server.on("/deployment-history.csv",HTTP_GET,handleDeploymentHistory);
  server.on("/pre-calibration.csv",HTTP_GET,[](){
    if(!dashboardAllowed())return;
    File f=logFS().open("/log-v4.csv",FILE_READ);
    if(!f){server.send(404,"text/plain","No pre-calibration log");return;}
    server.sendHeader("Content-Disposition","attachment; filename=pre_calibration.csv");
    server.streamFile(f,"text/csv");f.close();
  });
  server.on("/previous.csv",HTTP_GET,[](){
    if(!dashboardAllowed())return;
    File f=logFS().open(logFS().exists("/log-v3.csv")?"/log-v3.csv":"/log-v2.csv",FILE_READ);
    if(!f){server.send(404,"text/plain","No pre-settings log on active storage");return;}
    server.sendHeader("Cache-Control","no-store");server.sendHeader("Content-Disposition","attachment; filename=pre_settings_log.csv");
    server.streamFile(f,"text/csv");f.close();
  });
  server.on("/wifi",HTTP_GET,handleWifiPage);
  server.on("/wifi/state",HTTP_GET,handleWifiState);
  server.on("/wifi/networks",HTTP_GET,handleWifiNetworks);
  server.on("/wifi/scan",HTTP_POST,handleWifiScan);
  server.on("/wifi/connect",HTTP_POST,handleWifiConnect);
  server.on("/wifi/close",HTTP_POST,handleWifiClose);
  server.onNotFound([](){
    if(onSetupNetwork()){
      server.sendHeader("Cache-Control","no-store");
      server.sendHeader("Location",String("http://")+WiFi.softAPIP().toString()+"/wifi",true);
      server.send(302,"text/plain","");
    }else server.send(404,"text/plain","Not found");
  });
  server.on("/log.csv",HTTP_GET,[](){if(dashboardAllowed())handleCsv();});  server.on("/status",HTTP_GET,[](){if(dashboardAllowed())handleStatus();});
  server.on("/archives",HTTP_GET,[](){if(dashboardAllowed())handleArchives();});
  server.on("/download",HTTP_GET,[](){if(dashboardAllowed())handleArchiveDownload();});
  server.on("/clear",HTTP_POST,[](){if(dashboardAllowed())handleClear();});
  server.on("/archive.csv",HTTP_GET,[](){
    if(!dashboardAllowed())return;
    File f=logFS().open("/archive-v2.csv",FILE_READ);
    if(!f){server.send(404,"text/plain","No archive");return;}
    server.streamFile(f,"text/csv");f.close();
  });
  server.on("/legacy.csv",HTTP_GET,[](){
    if(!dashboardAllowed())return;
    File f=logFS().open("/log.csv",FILE_READ);
    if(!f){server.send(404,"text/plain","No legacy log");return;}
    server.streamFile(f,"text/csv");f.close();
  });
  const char* headers[]={"X-Log-Token","X-Setup-Token","X-Settings-Token"};server.collectHeaders(headers,3);
  server.begin();

  // Radio failures must not block Wi-Fi setup or access to saved logs.
  radioReady=startRadio();
  lastRadioRetry=millis();
  if(!haveSavedWifi)startWifiSetup();
  startWeatherWorker();
  updateDisplay();
}

void loop() {
  serviceButton();
  server.handleClient();

  serviceWifi();
  static uint32_t announcedIP=0;
  uint32_t currentIP=WiFi.status()==WL_CONNECTED?(uint32_t)WiFi.localIP():0;
  if(currentIP!=announcedIP){
    announcedIP=currentIP;
    if(currentIP)Serial.printf("Dashboard: http://%s/\n",WiFi.localIP().toString().c_str());
    else Serial.println("Home Wi-Fi disconnected; local logging remains active.");
  }
  serviceStorageSerial();
  serviceWeather();
  serviceOLED();
  if(!radioReady && millis()-lastRadioRetry>=30000UL){lastRadioRetry=millis();radioReady=startRadio();}

  if(radioReady && receivedFlag) {
    receivedFlag = false;
    String msg;
    int state = radio.readData(msg);
    if (state == RADIOLIB_ERR_NONE) {
      logReading(msg, radio.getRSSI(), radio.getSNR());
      updateDisplay();
    }
    state=radio.startReceive();
    if(state!=RADIOLIB_ERR_NONE){Serial.printf("RX start failed: %d\n",state);radioReady=false;}
  }

  // Current-log clearing uses the confirmed webpage action. INIT NEW FLASH is accepted only when storage cannot mount.
  if (millis() - lastDisplayMillis > 30000) updateDisplay();   // keeps "heard X min ago" fresh
}
