// Set this to the operator inbox for the firmware distribution.
#ifndef LOGGER_PAIRING_REQUEST_EMAIL
#define LOGGER_PAIRING_REQUEST_EMAIL "info@thesouthernbluegill.com"
#endif
// Southern Bluegill / XIAO ESP32S3 + B2B Wio-SX1262 kit (102010611).
// Receiver revision 20: recover interrupted log writes and retry uploads.
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
#include <mbedtls/sha256.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h> // ArduinoJson 7.4.2
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <FS.h>
#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <WebServer.h>
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
uint8_t displayMode=0; // Normal, Dim, Off
bool screenOn=true,buttonRaw=HIGH,buttonStable=HIGH;
unsigned long buttonChanged=0,screenStarted=0,buttonPressedAt=0;
bool buttonHoldHandled=false;
struct Deployment;
constexpr size_t REPORT_BODY_MAX=2048;
struct ReportJob { char body[REPORT_BODY_MAX]; char key[45]; char credential[44]; char url[112]; bool pairing; uint8_t checkCount; };
bool savedReading(const String& line,char* key,String& stamp,ReportJob* job);
String deploymentCSV(const String& node);
bool commitDeployment(Deployment& d,String& error);
void bindDeploymentToProbe(const String& node);
void loadDeployment();
void noteProbe(const String& node);
void startWifiSetup();
bool wifiSetupIsActive();
void enqueueReport(const String& node,const String& boot,const String& seq,const char* ts,float rawF,float offsetF,float tempF);
void loadReporting();
void handlePairing();
void handleOwnerLink();
bool startReportingWorker();
bool pairingTokenOK(const String&);
String pairingRandomKey();
String pairingHash(const String&);
void serviceReporting();
void reportWorker(void*);
bool reportDigits32(const String&,uint32_t&);
bool reportNodeOK(const String&);
bool reportTimestampOK(const char*);
String reportScan();
bool startReportingWorker();
void handleReportingGet();
void handleReportingSave();
bool appendLine(const String& line);
bool initLog(fs::FS& fs);
bool markLogV5(fs::FS& fs);
bool rotateLog(fs::FS& fs);
bool writeLine(fs::FS& fs,const String& line,bool card);
float probeOffset(const String& node);
String oledTitle();
void updateDisplay();
void renderOLED();
bool savedUploadUsesLog();
String oledReportStatus();
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
    else if(!buttonHoldHandled&&displayMode!=2){screen(!screenOn);}
  }
  if(buttonStable==LOW && !buttonHoldHandled && now-buttonPressedAt>=5000UL){
    buttonHoldHandled=true;startWifiSetup();
  }
  // Mains-powered receiver: display remains on until USER toggles it off.
}
volatile uint32_t radioIrqs=0;
uint32_t handledRadioIrqs=0,radioReadErrors=0,radioCrcErrors=0,radioRearmErrors=0,radioDecoded=0;
int16_t radioLastError=RADIOLIB_ERR_NONE;
uint32_t logVersion=0;
void IRAM_ATTR setFlag() { ++radioIrqs; }

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
  screen(displayMode!=2);
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
  ++radioDecoded;
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
  if(!storageError){
    ++savedReadings;++logVersion;
    if(validTemp)enqueueReport(r.node,r.boot,r.seq,ts,rawTemp.toFloat(),offset,lastTemp.toFloat());
  }
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
  const size_t size=old.size();
  const int lastByte=size&&old.seek(size-1)?old.read():-1;
  old.close();
  if(size&&lastByte<0)return false;
  const bool partial=size&&lastByte!='\n';
  const char separator[]=",#INCOMPLETE_WRITE\n";
  const bool rotate=size+line.length()+(partial?sizeof(separator)-1:0)>LOG_ROTATE_BYTES;
  if(rotate&&!rotateLog(fs))return false;
  File f=fs.open("/log-v5.csv",FILE_APPEND);
  if(!f) return false;
  if(partial&&!rotate&&f.print(separator)!=sizeof(separator)-1){f.close();return false;}
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
  s+=",\"log_version\":"+String(logVersion)+",\"radio_irqs\":"+String((uint32_t)radioIrqs)+
    ",\"radio_decoded\":"+String(radioDecoded)+",\"radio_read_errors\":"+String(radioReadErrors)+
    ",\"radio_crc_errors\":"+String(radioCrcErrors)+",\"radio_rearm_errors\":"+String(radioRearmErrors)+
    ",\"radio_last_error\":"+String(radioLastError);
  s+=",\"rssi\":"+jsonFloat(latestRSSI)+",\"snr\":"+jsonFloat(latestSNR)+
    ",\"radio_node\":"+jsonText(latestRadioNode)+
    ",\"receiver_uptime_s\":"+String((unsigned long)receiverSeconds())+
    ",\"receiver_reset\":"+jsonText(String(receiverResetName()))+",\"radio_ready\":"+String(radioReady?"true":"false")+"}";
  server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",s);
}
void handleClear() {
  // A per-boot token plus POST prevents accidental link activation/cross-site form submission.
  if(server.header("X-Log-Token")!=clearToken){server.send(403,"text/plain","Reload page before clearing");return;}
  if(savedUploadUsesLog()){server.send(409,"text/plain","Cancel the saved-reading upload before clearing the log.");return;}
  bool ok=(sdOk||flashOk);
  if(ok && logFS().exists("/log-v5.csv")) ok=logFS().remove("/log-v5.csv");
  if(ok){++logVersion;ok=initLog(logFS());}
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
  oled.setContrast(displayMode==1?16:127);
  if(displayMode==2){screenOn=false;oled.setPowerSave(1);}
  else drawBrand();
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
  if(displayMode!=2&&!weatherPending&&location.length()&&WiFi.status()==WL_CONNECTED&&!setupActive&&time(nullptr)>1700000000&&
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
  if(!setupActive&&now-brandStarted<3000UL){drawBrand();return;}
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
      centeredText(32,connected?"LOCAL DASHBOARD":"Logging continues");
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
      centeredText(10,title);
      if(haveRx){
        oled.setFont(u8g2_font_9x15_tf);
        if(lastTemp=="FAULT")centeredText(38,"SENSOR ERROR");
        else {
          constexpr int unitWidth=17;
          oled.setFont(u8g2_font_logisoso24_tf);
          if(oled.getStrWidth(lastTemp.c_str())+unitWidth>124)oled.setFont(u8g2_font_9x15_tf);
          int width=oled.getStrWidth(lastTemp.c_str());
          int left=(128-width-unitWidth)/2;
          oled.drawStr(left,39,lastTemp.c_str());
          oled.setFont(u8g2_font_6x10_tf);
          oled.drawCircle(left+width+5,29,1);
          oled.drawStr(left+width+9,35,"F");
        }
        oled.setFont(u8g2_font_6x10_tf);
        unsigned long minutes=(receiverSeconds()-lastRxSeconds)/60ULL;
        centeredText(50,minutes>=35?"OVERDUE "+String(minutes)+"m":String(minutes)+"m ago | TX "+(lastBatt.length()?lastBatt+"V":"--V"));
        centeredText(62,oledReportStatus());
      }else {centeredText(33,"Waiting for reading");centeredText(48,radioReady?"LoRa listening":"Radio retrying");centeredText(62,oledReportStatus());}
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

// Keep reporting credentials, pending payloads and saved-upload offsets tied
// to the environment that created them. Local logs and Wi-Fi are shared.
#ifdef LOGGER_PAIRING_PREVIEW
constexpr char REPORT_PREFS[]="logger-pair";
constexpr char HISTORY_PREFS[]="logger-history";
constexpr char REPORT_DIR[]="/pairq";
constexpr char SAVED_PLAN[]="/saved-missing.bin";
#else
constexpr char REPORT_PREFS[]="logger-live";
constexpr char HISTORY_PREFS[]="logger-live-h";
constexpr char REPORT_DIR[]="/liveq";
constexpr char SAVED_PLAN[]="/live-missing.bin";
#endif
static_assert(sizeof(REPORT_PREFS)<=16 && sizeof(HISTORY_PREFS)<=16, "NVS namespace too long");
constexpr uint16_t REPORT_LIMIT=48;
const char REPORT_CA[]=R"reportca(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlrsWNBCUaqxElqjANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjIwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjIwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQDO3v2m++zsFDQ8BwZabFn3GTXd98GdVarTzTukk3LvCvpt
nfbwhYBboUhSnznFt+4orO/LdmgUud+tAWyZH8QiHZ/+cnfgLFuv5AS/T3KgGjSY
6Dlo7JUle3ah5mm5hRm9iYz+re026nO8/4Piy33B0s5Ks40FnotJk9/BW9BuXvAu
MC6C/Pq8tBcKSOWIm8Wba96wyrQD8Nr0kLhlZPdcTK3ofmZemde4wj7I0BOdre7k
RXuJVfeKH2JShBKzwkCX44ofR5GmdFrS+LFjKBC4swm4VndAoiaYecb+3yXuPuWg
f9RhD1FLPD+M2uFwdNjCaKH5wQzpoeJ/u1U8dgbuak7MkogwTZq9TwtImoS1mKPV
+3PBV2HdKFZ1E66HjucMUQkQdYhMvI35ezzUIkgfKtzra7tEscszcTJGr61K8Yzo
dDqs5xoic4DSMPclQsciOzsSrZYuxsN2B6ogtzVJV+mSSeh2FnIxZyuWfoqjx5RW
Ir9qS34BIbIjMt/kmkRtWVtd9QCgHJvGeJeNkP+byKq0rxFROV7Z+2et1VsRnTKa
G73VululycslaVNVJ1zgyjbLiGH7HrfQy+4W+9OmTN6SpdTi3/UGVN4unUu0kzCq
gc7dGtxRcw1PcOnlthYhGXmy5okLdWTK1au8CcEYof/UVKGFPP0UJAOyh9OktwID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQUu//KjiOfT5nK2+JopqUVJxce2Q4wDQYJKoZIhvcNAQEMBQADggIBAB/Kzt3H
vqGf2SdMC9wXmBFqiN495nFWcrKeGk6c1SuYJF2ba3uwM4IJvd8lRuqYnrYb/oM8
0mJhwQTtzuDFycgTE1XnqGOtjHsB/ncw4c5omwX4Eu55MaBBRTUoCnGkJE+M3DyC
B19m3H0Q/gxhswWV7uGugQ+o+MePTagjAiZrHYNSVc61LwDKgEDg4XSsYPWHgJ2u
NmSRXbBoGOqKYcl3qJfEycel/FVL8/B/uWU9J2jQzGv6U53hkRrJXRqWbTKH7QMg
yALOWr7Z6v2yTcQvG99fevX4i8buMTolUVVnjWQye+mew4K6Ki3pHrTgSAai/Gev
HyICc/sgCq+dVEuhzf9gR7A/Xe8bVr2XIZYtCtFenTgCR2y59PYjJbigapordwj6
xLEokCZYCDzifqrXPW+6MYgKBesntaFJ7qBFVHvmJ2WZICGoo7z7GJa7Um8M7YNR
TOlZ4iBgxcJlkoKM8xAfDoqXvneCbT+PHV28SSe9zE8P4c52hgQjxcCMElv924Sg
JPFI/2R80L5cFtHvma3AH/vLrrw4IgYmZNralw4/KBVEqE8AyvCazM90arQ+POuV
7LXTWtiBmelDGDfrs7vRWGJB82bSj6p4lVQgw1oudCvV0b4YacCs1aTPObpRhANl
6WLAYv7YTVWW4tAR+kg0Eeye7QUd5MjWHYbL
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G
jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2
4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7
VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm
ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
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
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)reportca";
// Only the Arduino loop writes this staging object. FreeRTOS copies it
// into the queue before the reporting worker can read its own copy.
static ReportJob reportJobScratch;
struct ReportResult { int code; char storedAt[25]; bool pairing; bool paired; bool checked; uint16_t present; bool alreadyOnline; };
QueueHandle_t reportJobs=nullptr,reportResults=nullptr;
bool reportEnabled=false,reportInFlight=false,reportPaused=false;
uint16_t reportPending=0;
uint32_t reportNext=0,reportDelay=15000;
String reportFile,reportError,reportLast;
String uploadCredential,ownerCredential,pairMessage,pairedOrigin;
bool receiverPaired=false,pairBusy=false;

bool reportQueueWarning=false,reportScanRetry=false;
String oledReportStatus(){
  if(!reportEnabled)return "Sharing off";
  if(reportPaused)return "Uploads paused";
  if(reportPending)return String(reportPending)+" queued";
  if(reportQueueWarning)return "Check upload log";
  return reportLast.length()?"Synced":"Waiting to sync";
}
bool reportDigits32(const String& s,uint32_t& out){
  if(!s.length()||s.length()>10)return false;
  uint64_t n=0;
  for(size_t i=0;i<s.length();++i){char c=s[i];if(c<'0'||c>'9')return false;n=n*10+(c-'0');}
  if(n>0xffffffffULL)return false;out=(uint32_t)n;return true;
}
bool reportNodeOK(const String& s){
  if(s.length()!=16)return false;
  for(size_t i=0;i<s.length();++i){char c=s[i];if(!(c>='0'&&c<='9')&&!(c>='A'&&c<='F'))return false;}
  return true;
}
bool reportTimestampOK(const char* s){
  if(!s||strlen(s)!=24)return false;
  const char shape[]="0000-00-00T00:00:00.000Z";
  for(size_t i=0;i<24;++i)if(shape[i]=='0'?(s[i]<'0'||s[i]>'9'):s[i]!=shape[i])return false;
  return true;
}
String reportScan(){
  String oldest;
  if(!flashOk||(!reportPending&&!reportScanRetry&&!LittleFS.exists(REPORT_DIR)))return oldest;
  File dir=LittleFS.open(REPORT_DIR);
  if(!dir||!dir.isDirectory()){
    reportScanRetry=true;
    reportError="Queue could not be read. Retrying.";
    reportNext=millis()+15000;
    return oldest;
  }
  uint16_t pending=0;
  for(File f=dir.openNextFile();f;f=dir.openNextFile()){
    String name=f.name();int slash=name.lastIndexOf('/');if(slash>=0)name.remove(0,slash+1);
    if(!f.isDirectory()&&name.endsWith(".json")){
      ++pending;
      if(!oldest.length()||name<oldest)oldest=name;
    }
    f.close();
  }
  reportPending=pending;
  if(reportScanRetry)reportError="";
  reportScanRetry=false;
  return oldest.length()?String(REPORT_DIR)+"/"+oldest:String("");
}
void loadReporting(){
  Preferences p;
  if(p.begin(REPORT_PREFS,true)){
    pairedOrigin=p.getString("origin","");
    uploadCredential=p.getString("upload","");
    ownerCredential=p.getString("owner","");
    receiverPaired=pairedOrigin.length()&&p.getBool("paired",false)&&pairingTokenOK(uploadCredential)&&pairingTokenOK(ownerCredential);
    reportEnabled=receiverPaired&&p.getBool("enabled",false);
    reportLast=p.getString("last","");
    reportQueueWarning=p.getBool("queuewarn",false);
    p.end();
  }
  reportScan();
}
void enqueueReport(const String& node,const String& boot,const String& seq,const char* ts,float rawF,float offsetF,float tempF){
  if(!reportEnabled||!flashOk||!deploymentSet||deploymentHistoryWarning||
     node!=deployment.node||!deployment.location[0]||deployment.waterType<1||
     deployment.waterType>4||strcmp(ts,"no-time")==0||!reportNodeOK(node))return;
  uint32_t b,s;if(!reportDigits32(boot,b)||!reportDigits32(seq,s))return;
  const char* types[]={"","pond","lake","river","other"};
  JsonDocument doc;
  JsonObject r=doc["readings"].to<JsonArray>().add<JsonObject>();
  r["node"]=node;r["boot"]=b;r["sequence"]=s;r["recordedAt"]=ts;
  r["rawF"]=roundf(rawF*10)/10;r["offsetF"]=roundf(offsetF*10)/10;r["tempF"]=roundf(tempF*10)/10;
  r["waterType"]=types[deployment.waterType];r["location"]=deployment.location;
  if(deployment.name[0])r["waterBody"]=deployment.name;
  if(deployment.hasDepth)r["depthM"]=deployment.depthM;
  String body;if(!serializeJson(doc,body)||body.length()>=REPORT_BODY_MAX)return;
  String path=String(REPORT_DIR)+"/"+node+"-"+String(b)+"-"+String(s)+".json";
  if(LittleFS.exists(path))return; // An already queued ID keeps its original payload.
  if(reportPending>=REPORT_LIMIT || LittleFS.totalBytes()<=LittleFS.usedBytes()+96UL*1024||
     (!LittleFS.exists(REPORT_DIR)&&!LittleFS.mkdir(REPORT_DIR))){
    reportQueueWarning=true;Preferences p;if(p.begin(REPORT_PREFS,false)){p.putBool("queuewarn",true);p.end();}
    reportError="Queue full or unavailable. This reading was saved locally only.";return;
  }
  String tmp=String(REPORT_DIR)+"/pending.tmp";
  File f=LittleFS.open(tmp,FILE_WRITE);
  const bool wrote=f&&f.print(body)==body.length();
  if(f){f.flush();f.close();}
  if(!wrote||!LittleFS.rename(tmp,path)){
    LittleFS.remove(tmp);reportQueueWarning=true;
    Preferences p;if(p.begin(REPORT_PREFS,false)){p.putBool("queuewarn",true);p.end();}
    reportError="Queue write failed. This reading was saved locally only.";return;
  }
  if(!reportPending)reportNext=millis();
  ++reportPending;
}
void reportDiagnostics(const char* stage){
  Serial.printf("Catch-up: %s, task=%s, stack low-water=%u, free heap=%u, max alloc=%u\n",
    stage,pcTaskGetName(nullptr),(unsigned)uxTaskGetStackHighWaterMark(nullptr),
    (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getMaxAllocHeap());
}
bool readReportReply(HTTPClient& http,int length,JsonDocument& reply){
  constexpr size_t limit=512;
  if(length==0||length>int(limit)||length<-1)return false;
  auto& stream=http.getStream();
  static char body[limit+1]; // Used by the reporting worker only.
  size_t used=0;
  const uint32_t start=millis();
  while((length<0||used<(size_t)length)&&millis()-start<5000UL){
    if(stream.available()){
      int value=stream.read();
      if(value>=0){
        if(used==limit)return false;
        body[used++]=(char)value;
      }
    }else{
      if(!http.connected())break;
      delay(1);
    }
  }
  if(!used||(length>=0&&used!=(size_t)length)||(length<0&&http.connected()))return false;
  body[used]=0;
  return !deserializeJson(reply,static_cast<const char*>(body),used);
}
void reportWorker(void*){
  static ReportJob job;
  for(;;){
    if(xQueueReceive(reportJobs,&job,portMAX_DELAY)!=pdTRUE)continue;
    ReportResult result={-1,{},job.pairing,false,false,0,false};
    if(job.checkCount)reportDiagnostics("check HTTPS begin");
    WiFiClientSecure client;
    client.setCACert(REPORT_CA);client.setHandshakeTimeout(8);
    HTTPClient http;http.setConnectTimeout(5000);http.setTimeout(5000);
    // getStream() bypasses HTTPClient's chunked-transfer decoder.
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if(http.begin(client,String(job.url))){
      http.addHeader("Content-Type","application/json");
      http.addHeader("Authorization",String("Bearer ")+job.credential);
      result.code=http.POST(reinterpret_cast<uint8_t*>(job.body),strlen(job.body));
      if(result.code==200||result.code==208){
        JsonDocument reply;
        const int length=http.getSize();
        if(readReportReply(http,length,reply)){
          if(job.checkCount && reply["version"].as<int>()==1 &&
             reply["checked"].as<int>()==job.checkCount && reply["present"].is<uint16_t>() &&
             reply["present"].as<uint32_t>()<(1UL<<job.checkCount)){
            result.checked=true;result.present=reply["present"].as<uint16_t>();
          }
          if(result.code==208&&reply["version"].as<int>()==1&&(reply["alreadyOnline"]|false)&&
             strcmp(reply["key"]|"",job.key)==0)result.alreadyOnline=true;
          if(job.pairing)result.paired=reply["version"].as<int>()==1&&(reply["paired"]|false);
          JsonArray receipts=reply["receipts"].as<JsonArray>();
          const char* key=receipts[0]["key"]|"";const char* at=receipts[0]["storedAt"]|"";
          if(reply["version"].as<int>()==1&&receipts.size()==1&&strcmp(key,job.key)==0&&reportTimestampOK(at))
            memcpy(result.storedAt,at,25);
        }
      }
      http.end();
    }
    if(job.checkCount){
      Serial.printf("Catch-up: check HTTPS result %d, validated=%u\n",result.code,(unsigned)result.checked);
      reportDiagnostics("check HTTPS end");
    }
    xQueueSend(reportResults,&result,portMAX_DELAY);
  }
}
bool startReportingWorker(){
  if(reportJobs&&reportResults)return true;
  reportJobs=xQueueCreate(1,sizeof(ReportJob));reportResults=xQueueCreate(1,sizeof(ReportResult));
  if(reportJobs&&reportResults&&xTaskCreate(reportWorker,"logger-report",12288,nullptr,1,nullptr)==pdPASS)return true;
  if(reportJobs)vQueueDelete(reportJobs);if(reportResults)vQueueDelete(reportResults);
  reportJobs=nullptr;reportResults=nullptr;return false;
}
// Saved uploads read a confirmed snapshot of the current log and its supported archives.
// No historical upload is created by enabling live sharing.
struct SavedUpload {
  uint32_t archive=1, lastArchive=0, tailBytes=0, offset=0, total=0, sent=0, already=0;
};
SavedUpload savedUpload, savedPreview;
enum SavedPhase { SAVED_IDLE, SAVED_SCAN, SAVED_REVIEW, SAVED_UPLOAD, SAVED_DONE, SAVED_ERROR };
SavedPhase savedPhase=SAVED_IDLE;
uint32_t savedTicket=0,savedOmitted=0,savedCheckNext=0,savedCheckDelay=15000;
String savedFirst,savedLast,savedMessage;
constexpr uint8_t SAVED_BATCH=16;
struct SavedCandidate {uint32_t archive,offset;char key[45];char stamp[21];};
struct SavedPosition {uint32_t archive,offset;};
SavedCandidate savedBatch[SAVED_BATCH];
uint8_t savedBatchSize=0;
bool savedInFlight=false, savedChecking=false;

bool submitSavedCheck();
void finishSavedCheck(const ReportResult& result);

bool storeSavedUpload(const SavedUpload& snapshot);
void finishSavedReading(const ReportResult& result);
bool storeSavedUpload(const SavedUpload& snapshot){
  Preferences p;if(!p.begin(HISTORY_PREFS,false))return false;
  bool ok=p.putBytes("missing-v1",&snapshot,sizeof(snapshot))==sizeof(snapshot);p.end();return ok;
}
void loadSavedUpload(){
  Preferences p;if(!p.begin(HISTORY_PREFS,true))return;
  SavedUpload job;
  if(p.getBytes("missing-v1",&job,sizeof(job))==sizeof(job)&&job.total&&job.archive&&job.sent+job.already<=job.total){
    savedUpload=job;savedPhase=job.sent+job.already==job.total?SAVED_DONE:SAVED_UPLOAD;
  }
  p.end();
}
uint32_t newestSavedArchive(){
  uint32_t newest=0;File dir=LittleFS.open("/logs");if(!dir||!dir.isDirectory())return 0;
  for(File f=dir.openNextFile();f;f=dir.openNextFile()){
    uint32_t id=0;
    if(!f.isDirectory()&&archiveIndex(f.name(),id)&&id>newest)newest=id;
    f.close();
  }
  return newest;
}
bool savedCsvFields(const String& line,String (&c)[21]){
  size_t field=0;bool quoted=false,closed=false;
  for(size_t i=0;i<line.length();++i){
    char x=line[i];
    if(quoted){
      if(x=='"'&&i+1<line.length()&&line[i+1]=='"'){c[field]+='"';++i;}
      else if(x=='"'){quoted=false;closed=true;}else c[field]+=x;
    }else if(x==','){if(++field>=21)return false;closed=false;}
    else if(closed)return false;
    else if(x=='"'){if(c[field].length())return false;quoted=true;}
    else c[field]+=x;
  }
  return (field==20||field==18)&&!quoted;
}
bool savedTimestamp(const String& ts){
  if(ts.length()!=20||ts[4]!='-'||ts[7]!='-'||ts[10]!='T'||ts[13]!=':'||ts[16]!=':'||ts[19]!='Z')return false;
  for(size_t i=0;i<19;++i)if(i!=4&&i!=7&&i!=10&&i!=13&&i!=16&&(ts[i]<'0'||ts[i]>'9'))return false;
  struct tm t={};char* end=strptime(ts.c_str(),"%Y-%m-%dT%H:%M:%SZ",&t);
  if(!end||*end||t.tm_year<125)return false;
  const int y=t.tm_year+1900;
  const int days[]={31,28+(y%4==0&&(y%100!=0||y%400==0)),31,30,31,30,31,31,30,31,30,31};
  if(t.tm_mon<0||t.tm_mon>=12||t.tm_mday<1||t.tm_mday>days[t.tm_mon]||t.tm_sec>59)return false;
  const time_t upper=time(nullptr)+300;
  struct tm now={};char limit[24];
  return gmtime_r(&upper,&now)&&strftime(limit,sizeof(limit),"%Y-%m-%dT%H:%M:%SZ",&now)&&ts<=limit;
}
bool savedReading(const String& line,char* key,String& stamp,ReportJob* job){
  String c[21];uint32_t boot,seq,revision;
  double raw,offset,temp,depth=0;
  if(line.endsWith(",#INCOMPLETE_WRITE")||!savedCsvFields(line,c)||c[4]!="1"||c[9]!="ok"||!savedTimestamp(c[0])||!reportNodeOK(c[6])||
     !reportDigits32(c[7],boot)||!reportDigits32(c[8],seq)||!reportDigits32(c[10],revision)||!revision||
     !settingTextOK(c[11],150)||!c[17].length()||!settingTextOK(c[17],150)||
     !settingNumber(c[1],temp))return false;
  // Pre-calibration rows (including migrated v4 rows) have no offset columns.
  if(!c[19].length()&&!c[20].length()){raw=temp;offset=0;}
  else if(!settingNumber(c[19],raw)||!settingNumber(c[20],offset))return false;
  if(raw<-67||raw>257||offset<-10||offset>10||temp<-77||temp>267||fabs(raw+offset-temp)>0.051)return false;
  const char* type=c[12]=="Pond"?"pond":c[12]=="Lake / reservoir"?"lake":
    c[12]=="River / stream"?"river":c[12]=="Other water"?"other":nullptr;
  // The v4 and current log headers both place depth after longitude.
  const String& savedDepth=c[15];
  const bool hasDepth=savedDepth.length()>0;
  if(!type||(hasDepth&&(!settingNumber(savedDepth,depth)||depth<0||depth>12000)))return false;
  snprintf(key,sizeof(ReportJob::key),"%s:%lu:%lu",c[6].c_str(),(unsigned long)boot,(unsigned long)seq);
  stamp=c[0];
  if(!job)return true; // Review validates rows and sends IDs only.
  // Recover full depth precision used by live uploads, but only from the row's
  // original immutable deployment and only when its logged metadata agrees.
  if(hasDepth){
    char path[48];deploymentPath(revision,path,sizeof(path));Deployment d={};
    if(readDeployment(path,d)&&d.revision==revision&&d.hasDepth&&c[6]==d.node&&c[11]==d.name&&c[17]==d.location&&
       c[12]==waterTypeName(d.waterType)&&savedDepth==String(d.depthM,4)&&isfinite(d.depthM)&&d.depthM>=0&&d.depthM<=12000)depth=d.depthM;
  }
  JsonDocument doc;JsonObject r=doc["missingReading"].to<JsonObject>();
  r["node"]=c[6];r["boot"]=boot;r["sequence"]=seq;r["recordedAt"]=c[0];
  // Match the live queue's single-precision rounding and JSON serialization.
  r["rawF"]=roundf((float)raw*10)/10;r["offsetF"]=roundf((float)offset*10)/10;r["tempF"]=roundf((float)temp*10)/10;
  r["waterType"]=type;r["location"]=c[17];if(c[11].length())r["waterBody"]=c[11];if(hasDepth)r["depthM"]=depth;
  return measureJson(doc)<sizeof(job->body)&&serializeJson(doc,job->body,sizeof(job->body));
}
String savedReportingOrigin(){
#ifdef LOGGER_PAIRING_PREVIEW
  // Preview deployment URLs are immutable. Use the moving pilot alias for
  // catch-up so an already-paired receiver can reach the matching server code.
  if(pairedOrigin.endsWith(".publii-cloudfare.pages.dev"))
    return "https://feature-logger-reporting-pil.publii-cloudfare.pages.dev";
#endif
  return pairedOrigin;
}
void failSavedUpload(const char* message){savedMessage=message;savedPhase=SAVED_ERROR;}
bool submitSavedCheck(){
  if(!savedBatchSize||(int32_t)(millis()-savedCheckNext)<0)return false;
  if(!startReportingWorker()){failSavedUpload("Could not start checking saved readings.");return false;}
  ReportJob& job=reportJobScratch;memset(&job,0,sizeof(job));job.checkCount=savedBatchSize;
  JsonDocument d;JsonArray keys=d["keys"].to<JsonArray>();
  for(uint8_t n=0;n<savedBatchSize;++n)keys.add((const char*)savedBatch[n].key);
  if(measureJson(d)>=sizeof(job.body)||!serializeJson(d,job.body,sizeof(job.body))){
    failSavedUpload("Could not prepare the reading check.");return false;
  }
  (savedReportingOrigin()+"/api/logger/readings?check=1").toCharArray(job.url,sizeof(job.url));
  uploadCredential.toCharArray(job.credential,sizeof(job.credential));
  if(xQueueSend(reportJobs,&job,0)!=pdTRUE)return false;
  savedChecking=true;reportInFlight=true;reportDiagnostics("check IDs queued");return true;
}
void finishSavedCheck(const ReportResult& result){
  savedChecking=false;
  if(savedPhase!=SAVED_SCAN)return; // The owner cancelled while the check was in flight.
  if(!result.checked){
    if(result.code==429){
      savedCheckNext=millis()+savedCheckDelay;
      savedCheckDelay=savedCheckDelay<150000U?savedCheckDelay*2:300000U;
      savedMessage="Reporting site is busy. Retrying automatically.";
    }else if(result.code==401||result.code==403)failSavedUpload("The site rejected this receiver’s key. Check the fleet registration.");
    else if(result.code<0)failSavedUpload("Could not connect to the reporting site. Check Wi-Fi and try again.");
    else if(result.code==200)failSavedUpload("The site returned an invalid reading check. Try again.");
    else {savedMessage="Reading check returned HTTP "+String(result.code)+". Try again.";savedPhase=SAVED_ERROR;}
    return;
  }
  savedCheckNext=millis();savedCheckDelay=15000;savedMessage="";
  if(LittleFS.totalBytes()<=LittleFS.usedBytes()+storageReserve(false)+4096){
    failSavedUpload("Not enough local space to check the saved log.");return;
  }
  File plan=LittleFS.open(SAVED_PLAN,FILE_APPEND);
  if(!plan){failSavedUpload("Could not save the list of missing readings.");return;}
  for(uint8_t n=0;n<savedBatchSize;++n){
    if(result.present&(1U<<n)){++savedPreview.already;continue;}
    const SavedCandidate& entry=savedBatch[n];
    const SavedPosition pos={entry.archive,entry.offset};
    if(plan.write(reinterpret_cast<const uint8_t*>(&pos),sizeof(pos))!=sizeof(pos)){
      plan.close();failSavedUpload("Could not save the list of missing readings.");return;
    }
    ++savedPreview.total;const String stamp(entry.stamp);
    if(!savedFirst.length()||stamp<savedFirst)savedFirst=stamp;
    if(!savedLast.length()||stamp>savedLast)savedLast=stamp;
  }
  plan.close();savedBatchSize=0;
}
void serviceSavedUpload(){
  const bool scan=savedPhase==SAVED_SCAN;
  if(!scan&&savedPhase!=SAVED_UPLOAD)return;
  if(pairBusy||reportInFlight||wifiSetupIsActive()||WiFi.status()!=WL_CONNECTED||time(nullptr)<1735689600)return;
  if(!scan&&(!reportEnabled||reportPaused||reportPending||(int32_t)(millis()-reportNext)<0))return;
  const auto openLog=[](uint32_t archive,uint32_t lastArchive){
    char path[40];snprintf(path,sizeof(path),"/logs/archive-%010lu.csv",(unsigned long)archive);
    if(archive==lastArchive+1&&!LittleFS.exists(path))strcpy(path,"/log-v5.csv");
    return LittleFS.open(path,FILE_READ);
  };
  const auto readRow=[](File& f,uint32_t limit,String& line,bool* incompleteTail=nullptr){
    static char buffer[1281]; // Single owner: the Arduino loop.
    const uint32_t start=f.position();
    if(start>=limit)return false;
    const size_t wanted=min(size_t(limit-start),sizeof(buffer));
    const size_t count=f.read((uint8_t*)buffer,wanted);
    if(!count||count>wanted)return false;
    char* end=(char*)memchr(buffer,'\n',count);
    if(!end){
      if(incompleteTail)*incompleteTail=count==limit-start;
      return false;
    }
    if(!f.seek(start+(end-buffer)+1))return false;
    if(end>buffer&&end[-1]=='\r')--end;
    *end=0;
    line=buffer;
    return true;
  };
  SavedUpload& j=scan?savedPreview:savedUpload;
  if(!scan){
    if(j.sent+j.already>=j.total){savedPhase=SAVED_DONE;return;}
    File plan=LittleFS.open(SAVED_PLAN,FILE_READ);SavedPosition pos={};
    const bool found=plan&&plan.size()==j.total*sizeof(pos)&&plan.seek((j.sent+j.already)*sizeof(pos))&&
      plan.read(reinterpret_cast<uint8_t*>(&pos),sizeof(pos))==sizeof(pos);
    if(plan)plan.close();
    if(!found||!pos.archive||pos.archive>j.lastArchive+1){failSavedUpload("Missing-reading list unavailable. Check again.");return;}
    File f=openLog(pos.archive,j.lastArchive);String line,stamp;ReportJob& job=reportJobScratch;memset(&job,0,sizeof(job));
    const uint32_t limit=pos.archive==j.lastArchive+1?j.tailBytes:(f?(uint32_t)f.size():0);
    if(!f||f.size()<limit||pos.offset>=limit||!f.seek(pos.offset)||!readRow(f,limit,line)||!savedReading(line,job.key,stamp,&job)){
      failSavedUpload("A saved reading is no longer available. Check again.");return;
    }
    f.close();if(!startReportingWorker()){failSavedUpload("Could not start the upload worker.");return;}
    (savedReportingOrigin()+"/api/logger/readings?missing=1").toCharArray(job.url,sizeof(job.url));
    uploadCredential.toCharArray(job.credential,sizeof(job.credential));
    if(xQueueSend(reportJobs,&job,0)==pdTRUE){savedInFlight=true;reportInFlight=true;}
    return;
  }
  if(savedBatchSize==SAVED_BATCH){submitSavedCheck();return;}
  if(j.archive>j.lastArchive+1){
    if(savedBatchSize){submitSavedCheck();return;}
    savedPhase=SAVED_REVIEW;reportDiagnostics("check complete");return;
  }
  File f=openLog(j.archive,j.lastArchive);
  const uint32_t limit=j.archive==j.lastArchive+1?j.tailBytes:(f?(uint32_t)f.size():0);
  if(!f||!limit||f.size()<limit){failSavedUpload("A saved log is missing, empty or unreadable.");return;}
  if(j.offset>=limit){++j.archive;j.offset=0;return;}
  if(!f.seek(j.offset)){failSavedUpload("Could not read the saved log.");return;}
  for(unsigned rows=0;rows<8&&f.position()<limit&&savedBatchSize<SAVED_BATCH;++rows){
    const uint32_t at=f.position();String line,stamp;
    bool incompleteTail=false;
    if(!readRow(f,limit,line,&incompleteTail)){
      if(at&&incompleteTail){++savedOmitted;j.offset=limit;break;}
      failSavedUpload("A saved log row is incomplete or unreadable.");return;
    }
    j.offset=f.position();
    if(at==0){
      line.trim();
      if(line!=LOG_HEADER&&line!=V4_LOG_HEADER){failSavedUpload("A saved log has an unrecognized CSV header.");return;}
      break;
    }
    SavedCandidate& entry=savedBatch[savedBatchSize];
    if(!savedReading(line,entry.key,stamp,nullptr)){++savedOmitted;continue;}
    ++savedBatchSize;entry.archive=j.archive;entry.offset=at;stamp.toCharArray(entry.stamp,sizeof(entry.stamp));
  }
  f.close();
}
void finishSavedReading(const ReportResult& result){
  savedInFlight=false;
  if(result.storedAt[0]||result.alreadyOnline){
    SavedUpload next=savedUpload;
    if(result.alreadyOnline)++next.already;
    else{
      ++next.sent;
      if(!reportLast.length()||String(result.storedAt)>reportLast)reportLast=result.storedAt;
    }
    if(!storeSavedUpload(next)){failSavedUpload("Could not save upload progress. Check online history again.");return;}
    savedUpload=next;reportDelay=15000;reportNext=millis()+250;
    if(next.sent+next.already==next.total){
      savedPhase=SAVED_DONE;
      LittleFS.remove(SAVED_PLAN);
      Preferences p;if(reportLast.length()&&p.begin(REPORT_PREFS,false)){p.putString("last",reportLast);p.end();}
    }
  }else if((result.code>=400&&result.code<500&&result.code!=429)||result.code==200||result.code==208){
    failSavedUpload("The site did not confirm the saved reading. Check online history again.");
  }else{
    reportNext=millis()+reportDelay;reportDelay=reportDelay<150000U?reportDelay*2:300000U;
  }
}
void handleSavedStatus(){
  if(!dashboardAllowed())return;
  const char* stages[]={"idle","scanning","review","uploading","done","error"};
  const bool reviewing=savedPhase==SAVED_SCAN||savedPhase==SAVED_REVIEW;
  const SavedUpload& j=reviewing?savedPreview:savedUpload;
  JsonDocument d;d["stage"]=stages[savedPhase];d["count"]=j.total;d["omitted"]=savedOmitted;
  d["sent"]=j.sent;d["already"]=j.already;d["first"]=savedFirst;d["last"]=savedLast;
  d["message"]=savedMessage;d["ticket"]=savedTicket;d["canCheck"]=receiverPaired&&flashOk&&!sdOk;d["canStart"]=receiverPaired&&reportEnabled&&flashOk&&!sdOk;d["paused"]=reportPaused;d["online"]=WiFi.status()==WL_CONNECTED;
  String out;serializeJson(d,out);server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",out);
}
void handleSavedAction(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before continuing.");return;}
  const String action=server.arg("action");
  if(savedInFlight){settingsReply(409,"Wait a moment for the current reading to finish, then tap Stop sending again.");return;}
  if(action=="cancel"){
    if(!storeSavedUpload(SavedUpload{})){settingsReply(500,"Could not cancel saved uploads.");return;}
    LittleFS.remove(SAVED_PLAN);savedPhase=SAVED_IDLE;savedBatchSize=0;savedUpload={};savedPreview={};savedMessage="";settingsReply(200,"Saved upload cancelled. Local readings are unchanged.");return;
  }
  if(!receiverPaired||!flashOk||sdOk){settingsReply(409,"Pair the receiver and check that internal flash is available.");return;}
  if(action=="review"&&savedPhase!=SAVED_UPLOAD&&savedPhase!=SAVED_SCAN&&!savedChecking){
    if(WiFi.status()!=WL_CONNECTED){settingsReply(409,"Connect the receiver to Wi-Fi before checking.");return;}
    if(LittleFS.exists(SAVED_PLAN)&&!LittleFS.remove(SAVED_PLAN)){settingsReply(500,"Could not reset the saved-reading check.");return;}
    savedBatchSize=0;
    if(time(nullptr)<1735689600){settingsReply(409,"Wait for the receiver clock to sync, then review saved readings.");return;}
    File f=LittleFS.open("/log-v5.csv",FILE_READ);
    if(!f||!f.size()){settingsReply(409,"Could not open the current saved log.");return;}
    savedPreview={};savedPreview.tailBytes=(uint32_t)f.size();f.close();
    savedPreview.lastArchive=newestSavedArchive();savedTicket=esp_random();savedOmitted=0;savedFirst="";savedLast="";savedMessage="";
    savedCheckNext=millis();savedCheckDelay=15000;
    savedPhase=SAVED_SCAN;reportDiagnostics("check started");settingsReply(202,"Checking saved logs…");return;
  }
  if(action=="start"&&savedPhase==SAVED_REVIEW&&savedPreview.total&&server.arg("ticket")==String(savedTicket)){
    File f=LittleFS.open("/log-v5.csv",FILE_READ);
    if(!reportEnabled||reportPaused){settingsReply(409,"Turn on and resume sharing before sending saved readings.");return;}
    const bool valid=!savedPreview.tailBytes||(f&&f.size()>=savedPreview.tailBytes);if(f)f.close();
    if(!valid||newestSavedArchive()!=savedPreview.lastArchive){
      failSavedUpload("The current log changed. Review saved readings again.");
      settingsReply(409,savedMessage);return;
    }
    SavedUpload job=savedPreview;job.archive=1;job.offset=0;job.already=0;
    // Only the reviewed byte prefix of the active file is eligible.
    if(!storeSavedUpload(job)){settingsReply(500,"Could not save upload progress. Local readings are intact.");return;}
    savedUpload=job;savedPhase=SAVED_UPLOAD;reportNext=millis();settingsReply(202,"Saved upload started.");return;
  }
  settingsReply(409,"Review the date range and count before starting a saved upload.");
}

bool savedUploadUsesLog(){
  return savedPhase==SAVED_SCAN||(savedPhase==SAVED_REVIEW&&savedPreview.total)||savedPhase==SAVED_UPLOAD||savedInFlight;
}
void serviceReporting(){
  if(reportInFlight||pairBusy){
    ReportResult result;
    if(xQueueReceive(reportResults,&result,0)!=pdTRUE)return;
    if(result.pairing){
      pairBusy=false;
      if(result.paired){
        Preferences p;
        if(p.begin(REPORT_PREFS,false)){
          const bool disabled=p.putBool("enabled",false)==1;
          receiverPaired=disabled&&p.putBool("paired",true)==1;
          p.end();
        }
        reportEnabled=false;
        pairMessage=receiverPaired?"Paired. Save your private dashboard link, then enable sharing.":"Pairing confirmed online but not saved locally. Retry the same code.";
      }else pairMessage=result.code==409?"Code expired, invalid, or already used.":"Pairing not confirmed. Retry the same code.";
      return;
    }
    reportInFlight=false;
    if(savedChecking){finishSavedCheck(result);return;}
    if(savedInFlight){finishSavedReading(result);return;}
    if(result.storedAt[0]){
      reportLast=result.storedAt;
      Preferences p;if(p.begin(REPORT_PREFS,false)){p.putString("last",reportLast);p.end();}
      if(!LittleFS.remove(reportFile)){
        reportError="Stored online; could not remove pending copy. Will retry safely.";
        reportNext=millis()+60000;
      }else{
        if(reportPending)--reportPending;reportError="";reportDelay=15000;reportNext=millis()+2000;
        Serial.printf("Reporting: stored online at %s\n",reportLast.c_str());
      }
    }else{
      reportError=result.code==200?"Missing or mismatched receipt; keeping reading pending.":
        result.code<0?"HTTPS connection failed; keeping reading pending.":"Upload returned HTTP "+String(result.code)+". Reading retained.";
      reportPaused=result.code==400||result.code==401||result.code==403||result.code==409;
      reportNext=millis()+reportDelay;reportDelay=reportDelay<150000U?reportDelay*2:300000U;
    }
    return;
  }
  if(savedPhase==SAVED_SCAN){serviceSavedUpload();return;}
  if(!reportPending&&!reportScanRetry){if(savedPhase==SAVED_UPLOAD)serviceSavedUpload();return;}
  if(!reportEnabled||reportPaused||!flashOk||wifiSetupIsActive()||
     WiFi.status()!=WL_CONNECTED||time(nullptr)<1735689600||(int32_t)(millis()-reportNext)<0)return;
  if(!startReportingWorker()){reportError="Could not start upload worker.";reportNext=millis()+60000;return;}
  reportFile=reportScan();if(!reportFile.length())return;
  File f=LittleFS.open(reportFile,FILE_READ);
  ReportJob& job=reportJobScratch;memset(&job,0,sizeof(job));
  const size_t size=f?f.size():0;
  const bool got=size>0&&size<sizeof(job.body)&&f.readBytes(job.body,size)==size;
  if(f)f.close();
  JsonDocument doc;
  if(!got||deserializeJson(doc,job.body)){
    reportError="Queued reading could not be read. Uploads paused; local logs are intact.";reportPaused=true;return;
  }
  JsonObject r=doc["readings"][0].as<JsonObject>();
  const char* node=r["node"]|"";
  if(!reportNodeOK(String(node))||!r["boot"].is<uint32_t>()||!r["sequence"].is<uint32_t>()){
    reportError="Queued reading ID is invalid. Uploads paused.";reportPaused=true;return;
  }
  snprintf(job.key,sizeof(job.key),"%s:%lu:%lu",node,(unsigned long)r["boot"].as<uint32_t>(),(unsigned long)r["sequence"].as<uint32_t>());
  (pairedOrigin+"/api/logger/readings").toCharArray(job.url,sizeof(job.url));
  uploadCredential.toCharArray(job.credential,sizeof(job.credential));
  if(xQueueSend(reportJobs,&job,0)==pdTRUE)reportInFlight=true;
}
bool pairingTokenOK(const String& key){
  if(key.length()!=43)return false;
  for(size_t i=0;i<key.length();++i){
    char c=key[i];
    if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
  }
  return true;
}
String pairingRandomKey(){
  const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  uint8_t bytes[32];esp_fill_random(bytes,sizeof(bytes));
  String out;uint32_t bits=0;unsigned n=0;
  for(uint8_t b:bytes){
    bits=(bits<<8)|b;n+=8;
    while(n>=6){n-=6;out+=alphabet[(bits>>n)&63];}
  }
  if(n)out+=alphabet[(bits<<(6-n))&63];
  return out;
}
String pairingHash(const String& key){
  uint8_t digest[32];char hex[65];
  if(mbedtls_sha256(reinterpret_cast<const uint8_t*>(key.c_str()),key.length(),digest,0)!=0)return "";
  for(unsigned i=0;i<32;++i)snprintf(hex+2*i,3,"%02x",(unsigned)digest[i]);
  return String(hex);
}
void handlePairing(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before pairing.");return;}
  const String code=server.arg("code");
#ifdef LOGGER_PAIRING_PREVIEW
  String host=server.arg("preview");host.trim();
  const String suffix=".publii-cloudfare.pages.dev";
  if(host.length()!=8+suffix.length()||!host.endsWith(suffix)){
    settingsReply(400,"Enter the deployment hostname from Cloudflare, without https:// or a path.");return;
  }
  for(unsigned i=0;i<8;++i)if(!((host[i]>='0'&&host[i]<='9')||(host[i]>='a'&&host[i]<='f'))){
    settingsReply(400,"Enter the deployment hostname from Cloudflare.");return;
  }
  const String origin=String("https://")+host;
#else
  const String origin="https://thesouthernbluegill.com";
#endif
  if(receiverPaired){settingsReply(409,"This receiver is already paired.");return;}
  if(pairBusy||reportInFlight||wifiSetupIsActive()||WiFi.status()!=WL_CONNECTED||time(nullptr)<1735689600){
    settingsReply(409,"Wait for Wi-Fi and clock sync, then retry.");return;
  }
  if(!pairingTokenOK(code)){settingsReply(400,"Enter the complete pairing code.");return;}
  if(!pairingTokenOK(uploadCredential)||!pairingTokenOK(ownerCredential)){
    uploadCredential=pairingRandomKey();ownerCredential=pairingRandomKey();
  }
  Preferences p;
  if(!p.begin(REPORT_PREFS,false)){settingsReply(500,"Pairing storage unavailable.");return;}
  bool saved=p.putString("origin",origin)==origin.length()&&p.putString("upload",uploadCredential)==43&&p.putString("owner",ownerCredential)==43;
  p.end();
  if(!saved||!startReportingWorker()){settingsReply(500,"Could not prepare pairing. Retry the same code.");return;}
  pairedOrigin=origin;
  ReportJob& job=reportJobScratch;memset(&job,0,sizeof(job));job.pairing=true;
  (origin+"/api/logger/pair").toCharArray(job.url,sizeof(job.url));code.toCharArray(job.credential,sizeof(job.credential));
  JsonDocument doc;doc["uploadHash"]=pairingHash(uploadCredential);doc["ownerHash"]=pairingHash(ownerCredential);
  if(!serializeJson(doc,job.body,sizeof(job.body))||xQueueSend(reportJobs,&job,0)!=pdTRUE){
    settingsReply(500,"Could not start pairing.");return;
  }
  pairBusy=true;pairMessage="Pairing…";
  settingsReply(202,"Pairing…");
}
void handleOwnerLink(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before opening the link.");return;}
  if(!receiverPaired){settingsReply(409,"Pair this receiver first.");return;}
  settingsReply(200,pairedOrigin+"/observations/logger/#key="+ownerCredential);
}
const char* reportingReadiness(){
  if(!flashOk||deploymentHistoryWarning)return "Reporting storage is unavailable.";
  if(!reportNodeOK(lastNode)&&(!deploymentSet||!reportNodeOK(String(deployment.node))))
    return "Wait for the receiver to get its first reading from the transmitter before enabling sharing.";
  if(!deploymentSet||!deployment.location[0]||deployment.waterType<1||deployment.waterType>4)
    return "Save a water location and water type before enabling sharing.";
  if(!reportNodeOK(String(deployment.node)))return "Save the water settings again to link them to the transmitter.";
  return "";
}
void handleDisplayGet(){
  if(!dashboardAllowed())return;
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json",String("{\"mode\":")+String(displayMode)+",\"present\":"+(oledReady?"true":"false")+",\"token\":"+jsonText(clearToken)+"}");
}
void handleDisplaySave(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before saving.");return;}
  const String mode=server.arg("mode");
  if(mode!="0"&&mode!="1"&&mode!="2"){settingsReply(400,"Choose Normal, Dim or Off.");return;}
  Preferences p;
  if(!p.begin("logger-display",false)){settingsReply(500,"Could not open display settings.");return;}
  const uint8_t next=mode[0]-'0';const bool saved=p.putUChar("mode",next)==1;p.end();
  if(!saved){settingsReply(500,"Could not save display settings.");return;}
  displayMode=next;
  if(oledReady)oled.setContrast(displayMode==1?16:127);
  screen(displayMode!=2||setupActive);
  settingsReply(200,"Display setting saved.");
}
void handleReportingGet(){
  if(!dashboardAllowed())return;
  JsonDocument doc;
  doc["canRequestCode"]=sizeof(LOGGER_PAIRING_REQUEST_EMAIL)>1;
  doc["paired"]=receiverPaired;doc["pairing"]=pairBusy;doc["pairMessage"]=pairMessage;
#ifdef LOGGER_PAIRING_PREVIEW
  doc["preview"]=true;
#endif
  doc["enabled"]=reportEnabled;doc["pending"]=reportPending;doc["last"]=reportLast;
  doc["error"]=reportError;doc["paused"]=reportPaused;doc["sending"]=reportInFlight;
  doc["warning"]=reportQueueWarning;
  const char* readiness=reportingReadiness();
  doc["ready"]=!*readiness;
  doc["readiness"]=readiness;
  doc["token"]=clearToken;
  String out;serializeJson(doc,out);server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",out);
}
void handleReportingSave(){
  if(!dashboardAllowed())return;
  if(server.header("X-Settings-Token")!=clearToken){settingsReply(403,"Reload Settings before saving.");return;}
  const String value=server.arg("enabled");
  if(value!="0"&&value!="1"){settingsReply(400,"Choose whether to share readings.");return;}
  const bool enable=value=="1";
  if(enable&&!receiverPaired){settingsReply(409,"Pair this receiver first.");return;}
  const char* readiness=reportingReadiness();
  if(enable&&*readiness){settingsReply(409,readiness);return;}
  Preferences p;
  if(!p.begin(REPORT_PREFS,false)){settingsReply(500,"Could not open reporting settings.");return;}
  const bool saved=p.putBool("enabled",enable)==1;p.end();
  if(!saved){settingsReply(500,"Could not save reporting settings.");return;}
  reportEnabled=enable;reportPaused=false;reportError="";reportNext=millis();reportDelay=15000;
  settingsReply(200,enable?"Live sharing is on. Use Check for missing readings to send anything recorded while sharing was off.":"Sharing paused. Pending readings remain on this receiver.");
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
</section><section id="display"><h2>Display</h2>
<form id="displayForm"><label for="displayMode">OLED mode</label><select id="displayMode" disabled><option value="0">Normal</option><option value="1">Dim</option><option value="2">Off</option></select>
<p class="hint">Normal and Dim both rotate through water temperature, weather, the local dashboard address and the logo. Dim lowers brightness. Off turns off the screen only; logging and uploads continue.</p><button id="displaySave" disabled>Save display setting</button></form>
<p id="displayFeedback" role="status"></p></section><section id="reporting"><h2>Online reporting</h2>
<p class="hint">Share new, timestamped water readings and the saved site name, location and depth with The Southern Bluegill. Local logging continues. Turning sharing off pauses uploads; a request already underway may finish.</p>
<form id="pairForm"><p id="pairRequest" hidden><a href="mailto:)settingshtml" LOGGER_PAIRING_REQUEST_EMAIL R"settingshtml(?subject=Water%20logger%20pairing%20code%20request&amp;body=Hello%2C%0D%0AI%27d%20like%20a%20pairing%20code%20for%20my%20water%20logger%20receiver.%0D%0A%0D%0APreferred%20receiver%20name%3A%20">Request a pairing code</a><br><span class="hint">Opens an email draft. Review it and send it from your email app.</span></p><div id="previewHost" hidden><label for="pairPreview">Preview deployment hostname</label><input id="pairPreview" maxlength="36" placeholder="1234abcd.publii-cloudfare.pages.dev"></div><label for="pairCode">One-time pairing code</label><input id="pairCode" type="password" autocomplete="off" maxlength="43" required><button id="pairSave" disabled>Connect to The Southern Bluegill</button></form>
<p id="pairState" role="status"></p><button id="ownerLink" type="button" hidden>Show private dashboard link</button><p><a id="privateLink" hidden target="_blank" rel="noopener noreferrer">Open and bookmark your private dashboard</a></p>
<form id="reportForm"><label><input id="reportEnabled" type="checkbox" style="width:auto;min-height:0" disabled> Share readings with The Southern Bluegill</label>
<button id="reportSave" disabled>Save sharing setting</button></form>
<p id="reportState" role="status"></p><p id="reportFeedback" role="status"></p>
<h3>Catch up on missed readings</h3>
<p class="hint">Check for saved water readings missing online, even while live sharing is off. The check sends reading IDs only. Turn on sharing and approve the send to upload the readings.</p>
<button id="savedReview" type="button" disabled>Check for missing readings</button>
<button id="savedStart" type="button" hidden>Send missing readings</button>
<button id="savedCancel" type="button" hidden>Cancel</button>
<p id="savedState" role="status"></p><p id="savedFeedback" role="alert"></p>
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
let displayToken='';
async function loadDisplay(){
 const r=await fetch('/display',{cache:'no-store'});if(!r.ok)throw Error('Display settings unavailable.');
 const d=await r.json();displayToken=d.token;$('displayMode').value=d.mode;
 $('displayMode').disabled=$('displaySave').disabled=false;
 if(!d.present)$('displayFeedback').textContent='No OLED detected. The setting can still be saved.';
}
$('displayForm').onsubmit=async e=>{
 e.preventDefault();$('displaySave').disabled=true;
 try {
  const r=await fetch('/display',{method:'POST',headers:{'X-Settings-Token':displayToken},body:new URLSearchParams({mode:$('displayMode').value})});
  const d=await r.json();$('displayFeedback').textContent=d.message;
 }catch(e){$('displayFeedback').textContent='Could not save display setting.';}finally{$('displaySave').disabled=false;}
};
loadDisplay().catch(e=>$('displayFeedback').textContent=e.message);
let reporting=null,pairSubmitting=false,pairFeedback="";
async function loadReporting(populate=false){
 const r=await fetch('/reporting',{cache:'no-store'});if(!r.ok)throw Error('Reporting status unavailable.');
 reporting=await r.json();
 if(populate)$('reportEnabled').checked=reporting.enabled;
 $('previewHost').hidden=!reporting.preview;$('pairPreview').required=!!reporting.preview;
 $('pairRequest').hidden=!reporting.canRequestCode;
 $('pairForm').hidden=reporting.paired;$('pairSave').disabled=pairSubmitting||reporting.pairing;
 if(reporting.paired)$('pairCode').value='';
 $('pairState').textContent=(reporting.paired||reporting.pairing?'':pairFeedback) ||
   reporting.pairMessage || (reporting.paired?'Receiver paired.':'');
 $('ownerLink').hidden=!reporting.paired;
 const sharingBlocked=!reporting.paired||(!reporting.enabled&&!reporting.ready);
 $('reportEnabled').disabled=sharingBlocked;$('reportSave').disabled=sharingBlocked;

 const state=!reporting.enabled?'Sharing off':reporting.paused?'Uploads paused':reporting.sending?'Sending…':
   reporting.pending?'Waiting to sync':reporting.last?'Live queue up to date':'Waiting for a new reading';
 $('reportState').textContent=state+' · '+reporting.pending+' live pending · Last confirmed: '+
   (reporting.last?new Date(reporting.last).toLocaleString():'none')+
   (reporting.error?' · '+reporting.error:'')+
   (reporting.warning?' · Some readings were saved locally only because the upload queue was unavailable.':'')+
   (!reporting.enabled?(reporting.paired?(reporting.readiness?' · '+reporting.readiness:''):' · Pair this receiver before enabling sharing.'):'');
}
$('pairForm').onsubmit=async e=>{
 e.preventDefault();if(!reporting||$('pairSave').disabled)return;
 pairSubmitting=true;pairFeedback='';$('pairSave').disabled=true;
 try{
  const body=new URLSearchParams({code:$('pairCode').value.trim(),preview:$('pairPreview').value.trim()});
  const r=await fetch('/pairing',{method:'POST',headers:{'X-Settings-Token':reporting.token},body});
  const result=await r.json();if(!r.ok)throw Error(result.message);
  reporting.pairing=true;
  $('pairState').textContent=result.message;
 }catch(e){pairFeedback=e.message;$('pairState').textContent=pairFeedback;}
 finally{pairSubmitting=false;$('pairSave').disabled=reporting.pairing||reporting.paired;}
};
$('ownerLink').onclick=async()=>{
 try{
  const r=await fetch('/owner-link',{method:'POST',headers:{'X-Settings-Token':reporting.token}});
  const result=await r.json();if(!r.ok)throw Error(result.message);
  $('privateLink').href=result.message;$('privateLink').hidden=false;
 }catch(e){$('pairState').textContent=e.message;}
};
$('reportForm').onsubmit=async e=>{
 e.preventDefault();if(!reporting||$('reportSave').disabled)return;$('reportSave').disabled=true;
 try{
  const body=new URLSearchParams({enabled:$('reportEnabled').checked?'1':'0'});
  const r=await fetch('/reporting',{method:'POST',headers:{'X-Settings-Token':reporting.token},body});
  const result=await r.json();if(!r.ok)throw Error(result.message);
  $('reportFeedback').textContent=result.message;await loadReporting(true);await loadSaved();
 }catch(e){$('reportFeedback').textContent=e.message;}finally{$('reportSave').disabled=!reporting.paired||(!reporting.enabled&&!reporting.ready);}
};
let savedStatus=null,savedBusy=false,savedUpdating=false;
async function loadSaved(){
 if(savedBusy||savedUpdating)return;
 savedUpdating=true;
 try{
  const r=await fetch('/saved-upload',{cache:'no-store'});if(!r.ok)throw Error('Could not load catch-up status.');
  const d=await r.json();if(savedBusy)return;savedStatus=d;
  const checking=d.stage==='scanning',review=d.stage==='review',upload=d.stage==='uploading';
  $('savedReview').hidden=checking||upload;
  $('savedReview').textContent=review||d.stage==='done'?'Check again':'Check for missing readings';
  $('savedReview').disabled=!d.canCheck||!d.online;
  $('savedStart').hidden=!review||!d.count;$('savedStart').disabled=!d.canStart||d.paused;
  $('savedStart').textContent=d.count===1?'Send 1 missing reading':'Send '+d.count+' missing readings';
  $('savedCancel').hidden=!checking&&!upload&&!(review&&d.count>0);
  $('savedCancel').textContent=upload?'Stop sending':checking?'Cancel check':'Dismiss';
  $('savedCancel').disabled=false;
  const time=t=>new Date(t).toLocaleString();
  const progress=d.sent+' newly shared'+(d.already?' · '+d.already+' already online':'');
  const missing=d.count===1?'1 reading missing online':d.count+' readings missing online';
  $('savedState').textContent=checking?(!d.online?'Waiting for Wi-Fi to check.':(d.message||'Checking what is already online…'))
   :review?(d.count?missing+' · '+time(d.first)+' to '+time(d.last):'No missing water readings to send.')+
      ' · '+d.already+' already online'+(d.omitted?' · '+d.omitted+' kept local':'')+(d.count&&!d.canStart?' · Turn on sharing to send these readings.':'')
   :upload?(!d.canStart||d.paused?'Catch-up paused':!d.online?'Waiting for Wi-Fi':'Sending missing readings')+' · '+progress+' · '+(d.count-d.sent-d.already)+' remaining'
   :d.stage==='done'?'Catch-up complete: '+progress+'.'
   :d.message||(!d.canCheck?'Pair the receiver and check that internal flash is available.':!d.online?'Connect the receiver to Wi-Fi to check.':'');
 }finally{savedUpdating=false;}
}
async function savedAction(action){
 if(savedBusy||!reporting||!savedStatus)return;
 savedBusy=true;
 $('savedFeedback').textContent='';
 for(const id of ['savedReview','savedStart','savedCancel'])$(id).disabled=true;
 try{
  const body=new URLSearchParams({action,ticket:savedStatus.ticket});
  const r=await fetch('/saved-upload',{method:'POST',headers:{'X-Settings-Token':reporting.token},body});
  const d=await r.json();if(!r.ok)throw Error(d.message);
 }catch(e){$('savedFeedback').textContent=e.message;}
 finally{savedBusy=false;loadSaved().catch(e=>$('savedState').textContent=e.message);}
}
$('savedReview').onclick=()=>savedAction('review');
$('savedStart').onclick=()=>savedAction('start');
$('savedCancel').onclick=()=>savedAction('cancel');
loadSaved().catch(e=>$('savedState').textContent=e.message);
setInterval(()=>loadSaved().catch(e=>$('savedState').textContent=e.message),3000);
loadReporting(true).catch(e=>$('reportState').textContent=e.message);
setInterval(()=>loadReporting().catch(e=>$('reportState').textContent=e.message),10000);

load().catch(e=>{$('feedback').textContent=e.message+'. Reload to try again.';});
</script></html>
)settingshtml";
void handleSettingsPage(){
  if(!dashboardAllowed())return;
  server.sendHeader("Cache-Control","no-store");server.send(200,"text/html",FPSTR(SETTINGS_PAGE));
}

// The web page. The browser downloads the CSV and draws the graphs itself.
const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water temperature logger</title>
<style>
:root{color-scheme:light;--ink:#142e3d;--muted:#45616e;--teal:#176975;--line:#d9e5e8;--paper:#edf4f5}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.5 system-ui,sans-serif}
[hidden]{display:none!important}
.shell{width:min(1060px,calc(100% - 32px));margin-inline:auto}
.top{background:#102c3a;color:#fff;border-bottom:4px solid #29949a}
.top-inner{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;gap:16px;padding:22px 0}
.brand{margin:0;color:#afdfe0;font-size:.85rem;font-weight:700}
h1{font-size:clamp(1.6rem,4vw,2.2rem);line-height:1.15;margin:.15em 0 0}
nav,.actions,.range{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button,.nav-link{display:inline-flex;align-items:center;justify-content:center;min-height:44px;padding:8px 14px;border-radius:8px;border:1px solid #92b3b9;background:#fff;color:var(--teal);font:inherit;font-weight:650;text-decoration:none;cursor:pointer}
.nav-link{background:#1c4251;color:#fff;border-color:#507784}
button[aria-pressed="true"],#save{background:var(--teal);border-color:var(--teal);color:white}
button:disabled{opacity:.6;cursor:wait}
button:focus-visible,a:focus-visible,svg:focus-visible,summary:focus-visible{outline:3px solid #ac6723;outline-offset:3px}
a{color:var(--teal)}main{padding:24px 0 36px}
.panel{min-width:0;margin:0 0 16px;padding:clamp(16px,3vw,26px);border:1px solid var(--line);border-top:4px solid var(--teal);border-radius:14px;background:#fff;box-shadow:0 2px 5px #142e3d0c}
h2{margin:0;font-size:1.2rem;line-height:1.25}p{margin:.55em 0 0;overflow-wrap:anywhere}
.muted,.feedback,footer{color:var(--muted);font-size:.9rem}.warn{color:#9c2e25;font-weight:700}
.overview{display:grid;grid-template-columns:minmax(0,1.1fr) minmax(0,1fr);gap:16px}
.hero{background:linear-gradient(140deg,#102f3e,#18545e);color:#f5ffff;border-color:#216774}
.hero h2{color:#b9e0e1;font-size:.95rem}.hero p{color:#e1f1f2}
#now{font-size:clamp(1.6rem,4vw,2.4rem);font-weight:780;line-height:1.05;margin:12px 0;letter-spacing:-.04em}
#now[data-reading]{font-size:clamp(3rem,9vw,5.2rem)}
.overview-status p+p{margin-top:10px}.overview-status #storage{border-top:1px solid var(--line);padding-top:9px}
.help summary,.diagnostics summary{cursor:pointer;font-weight:700;color:var(--teal);padding:8px 0}
.range-panel h2{margin-bottom:12px}.range button{flex:0 1 auto}
.stats{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px;margin:12px 0 0}
.stats>div{min-width:0;padding:8px 10px;background:#edf5f5;border-radius:8px}
.stats dt{color:var(--muted);font-size:.83rem}.stats dd{margin:0;font-size:1.12rem;font-weight:750;font-variant-numeric:tabular-nums}
.chart-card{--series:#167a89;--tint:#edf7f7;border-top-color:var(--teal)}
.battery{--series:#916022;--tint:#faf4e8}.radio{--series:#4f598f;--tint:#f1f2f9}
.plot{display:block;width:100%;height:auto;touch-action:pan-y;cursor:crosshair;margin-top:12px;background:#f9fbfc;border-radius:8px}
.plot text{fill:#45616e;font:12px system-ui,sans-serif}.plot .series{stroke:var(--series)}.plot .sample{fill:var(--series)}
.plot .cursor{stroke:var(--series);stroke-dasharray:4 4}.plot .picked{fill:white;stroke:var(--series);stroke-width:2.5}
.chart-detail{display:block;margin-top:10px;padding:9px 12px;min-height:44px;background:var(--tint);border-radius:8px;color:var(--ink);font-size:.92rem;overflow-wrap:anywhere}
.chart-detail strong{color:var(--series);font-size:1rem}.chart-detail span{display:inline-block}.chart-detail span+span::before{content:" · ";white-space:pre}
.diagnostics{margin-top:12px;border-top:1px solid var(--line)}.log-links{display:flex;flex-wrap:wrap;gap:4px 16px;margin-top:10px}.log-links a{display:inline-flex;align-items:center;min-height:44px}
#logs h2{margin-bottom:12px}#clear{color:#8b3128;border-color:#c29086}#actionFeedback:empty{display:none}
footer{margin-top:12px}.help{margin-top:10px}
@media(max-width:610px){
 .shell{width:calc(100% - 24px)}main{padding-top:16px}.overview{grid-template-columns:minmax(0,1fr);gap:0}
 .top-inner{padding:16px 0}.stats{grid-template-columns:repeat(2,minmax(0,1fr))}
}
</style>
<header class="top"><div class="shell top-inner"><div><p class="brand">The Southern Bluegill</p><h1>Water temperature logger</h1></div><nav aria-label="Receiver"><a class="nav-link" href="/settings">Settings</a><a class="nav-link" href="#logs">Logs</a></nav></div></header>
<main class="shell">
<div class="overview">
<section class="panel hero"><h2>Latest water temperature</h2><p id="now">Waiting for data</p><p id="fresh"></p><p id="change"></p></section>
<section class="panel overview-status"><h2>Receiver status</h2><p id="health">Checking receiver…</p><p id="storage"></p><p id="receiverInfo" class="muted"></p><p id="notice" class="muted" role="status">Loading receiver data…</p></section>
</div>
<section class="panel range-panel"><h2>Reading history</h2><div class="range" role="group" aria-label="History period"><button data-h="24" aria-pressed="true">24 hours</button><button data-h="168" aria-pressed="false">7 days</button><button data-h="0" aria-pressed="false">All current log</button></div>
<p id="rangeFeedback" class="feedback" role="status">Showing last 24 hours</p>
<dl id="stats" class="stats" aria-label="Water temperature in the selected period" hidden><div><dt>Valid readings</dt><dd id="statCount"></dd></div><div><dt>Low</dt><dd id="statLow"></dd></div><div><dt>High</dt><dd id="statHigh"></dd></div><div><dt>Sample mean</dt><dd id="statMean"></dd></div></dl><p id="statsEmpty" class="muted"></p></section>
<section class="panel chart-card"><h2 id="temp-title">Water temperature (°F)</h2><svg id="temp" class="plot" role="img" aria-labelledby="temp-title" aria-describedby="temp-detail"></svg><div id="temp-detail" class="chart-detail" role="status">Tap the chart to inspect a reading.</div></section>
<section class="panel chart-card battery"><h2 id="batt-title">Transmitter battery (V)</h2><svg id="batt" class="plot" role="img" aria-labelledby="batt-title" aria-describedby="batt-detail"></svg><div id="batt-detail" class="chart-detail" role="status">Tap the chart to inspect a reading.</div><p class="muted">Voltage is not a calibrated battery percentage.</p></section>
<section class="panel chart-card radio"><h2 id="rssi-title">Received signal strength (dBm)</h2><svg id="rssi" class="plot" role="img" aria-labelledby="rssi-title" aria-describedby="rssi-detail"></svg><div id="rssi-detail" class="chart-detail" role="status">Tap the chart to inspect a reading.</div>
<p id="radioLatest">Waiting for a packet received this receiver boot.</p><details class="diagnostics"><summary>Radio link diagnostics</summary><p id="radioMargin" class="muted"></p><p id="radioCounters"></p><p id="reportGaps"></p><p id="reportTiming"></p><p class="muted">Gap statistics count missing sequence numbers between the first and last observed reports for each transmitter and boot in the selected log period. Receiver outages and failed saves can also cause gaps. RSSI describes received packets and cannot reliably determine distance or delivery rate.</p></details></section>
<section class="panel" id="logs"><h2>Logs</h2><div class="actions"><button id="save">Download CSV</button><button id="clear">Clear current log…</button></div><div class="log-links"><a href="/archives">Browse / save archives</a><a href="/pre-calibration.csv">Pre-calibration backup</a><a href="/previous.csv" download="pre_settings_log.csv">Save pre-settings log</a><a href="/legacy.csv" download="legacy.csv">Save original-format log</a></div><p class="muted">Save a copy before clearing. Clearing affects the current log on the active storage only. Archives and logs on another storage device remain intact.</p><p id="actionFeedback" class="feedback" role="status"></p></section>
<footer><p>Receiver revision 20 · Seeed ESP32S3 · Local logging</p>
<details class="help"><summary>Wi-Fi and receiver button</summary><p>Hold USER for 5 seconds, then join the Bluegill setup network to change Wi-Fi. Logs are retained. A short press toggles the optional SH1106 in Normal or Dim mode. Display weather uses modeled air conditions from Open-Meteo / GeoNames.</p></details></footer>
</main>
<script>
let rows=[],hours=24,busy=false,status=null,loadedLogVersion=null;
const $=id=>document.getElementById(id),finite=Number.isFinite;
function parse(csv){return csv.slice(0,csv.lastIndexOf('\n')+1).trim().split(/\r?\n/).slice(1).filter(l=>!l.endsWith(",#INCOMPLETE_WRITE")).map(l=>{const c=l.split(',');return {t:Date.parse(c[0]),temp:c[1]===''?NaN:Number(c[1]),rssi:c[2]===''?NaN:Number(c[2]),batt:c[3]===''?NaN:Number(c[3]),quality:c[9],node:c[6],boot:c[7],seq:/^\d+$/.test(c[8]||'')?Number(c[8]):NaN};}).filter(r=>finite(r.t)).sort((a,b)=>a.t-b.t);}
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
 $('radioCounters').textContent=status.radio_irqs===undefined?'':'This boot: '+status.radio_irqs+' IRQs · '+status.radio_decoded+' decoded · '+status.radio_read_errors+' radio read errors ('+status.radio_crc_errors+' CRC/header) · '+status.radio_rearm_errors+' rearm failures'+(status.radio_last_error?' · last radio error '+status.radio_last_error:'');
 $('receiverInfo').textContent='Receiver uptime: '+duration(status.receiver_uptime_s)+' · reset reason: '+(status.receiver_reset||'unavailable')+' · LoRa '+(status.radio_ready?'listening':'initialization failed; retrying')+' · 907.8 MHz / 500 kHz / SF9';
 const valid=status.received&&finite(status.rssi),stale=!status.received||status.age_s>22.5*60;
 $('radioLatest').textContent=valid?'Latest received packet: RSSI '+status.rssi.toFixed(1)+' dBm · SNR '+(finite(status.snr)?status.snr.toFixed(1)+' dB':'unavailable')+' · transmitter '+status.radio_node+' · '+duration(status.age_s)+' ago'+(stale?' — STALE':''):'Waiting for a packet received this receiver boot. Historical RSSI remains in the graph.';
 $('radioLatest').className=stale?'warn':'';
 $('radioMargin').textContent=valid&&finite(status.snr)?'SNR margin above the approximate SF9 decoding threshold (−12.5 dB): '+(status.snr+12.5).toFixed(1)+' dB. This is an indicative margin, not a delivery guarantee. SNR is live only; it is not stored in this CSV.':'RSSI: less negative is stronger. SNR: higher is better. Radio settings: 907.8 MHz / SF9 / 500 kHz. SNR is live only; it is not stored in this CSV.';
}
const chartSpecs=[
 {id:'temp',key:'temp',unit:'°F',digits:1},
 {id:'batt',key:'batt',unit:'V',digits:2},
 {id:'rssi',key:'rssi',unit:'dBm',digits:1}
];
const pickedRows=new Map();
const selectedWindow=()=>hours?Date.now()-hours*3600000:-Infinity;
const rowIdentity=r=>JSON.stringify([r.t,r.node,r.boot,r.seq]);

function draw({id,key,unit,digits},source){
 const svg=$(id),detail=$(id+'-detail');
 const width=Math.max(220,Math.round(svg.clientWidth||700)),height=width<480?212:270;
 const left=width<480?48:58,right=width-14,top=16,bottom=height-30;
 const pts=source.filter(r=>finite(r[key])&&(key!=='temp'||r.quality==='ok'));
 svg.setAttribute('viewBox','0 0 '+width+' '+height);
 svg.replaceChildren();
 const add=(tag,attrs,text)=>{
  const el=document.createElementNS('http://www.w3.org/2000/svg',tag);
  for(const [name,value] of Object.entries(attrs))el.setAttribute(name,value);
  if(text!==undefined)el.textContent=text;
  svg.append(el);
  return el;
 };
 if(!pts.length){
  svg.removeAttribute('tabindex');
  svg.onclick=svg.onkeydown=null;
  add('text',{x:width/2,y:height/2,'text-anchor':'middle'},'No readings in this period');
  detail.hidden=true;
  return;
 }
 detail.hidden=false;
 svg.setAttribute('tabindex','0');
 const x0=source[0].t,x1=source.at(-1).t,span=x1-x0;
 const vals=pts.map(r=>r[key]),min=Math.min(...vals),max=Math.max(...vals);
 const pad=Math.max((max-min)*.08,key==='batt'?.025:.5),y0=min-pad,y1=max+pad;
 const sx=t=>span?left+(t-x0)/span*(right-left):(left+right)/2;
 const sy=v=>bottom-(v-y0)/(y1-y0)*(bottom-top);
 const stamp=t=>new Date(t).toLocaleString([],{month:'short',day:'numeric',hour:'numeric',minute:'2-digit'});
 for(let i=0;i<5;i++){
  const v=y0+(y1-y0)*i/4,y=sy(v);
  add('line',{x1:left,x2:right,y1:y,y2:y,stroke:'#d9e5e8'});
  add('text',{x:left-7,y:y+4,'text-anchor':'end'},v.toFixed(digits));
 }
 const ticks=span?(width<520?[0,1]:[0,.5,1]):[.5];
 for(const fraction of ticks){
  const t=x0+span*fraction;
  add('text',{x:sx(t),y:height-8,'text-anchor':fraction===0?'start':fraction===1?'end':'middle'},stamp(t));
 }
 // Keep extrema and segment boundaries. Break at faults and gaps > 22.5 minutes.
 let segments=[],seg=[],prev=null;
 for(const r of source){
  if(!finite(r[key])||(key==='temp'&&r.quality!=='ok')){
   if(seg.length)segments.push(seg);
   seg=[];prev=null;continue;
  }
  if(prev&&r.t-prev.t>22.5*60000){segments.push(seg);seg=[];}
  seg.push(r);prev=r;
 }
 if(seg.length)segments.push(seg);
 for(const segment of segments){
  const reduced=[],step=Math.max(1,Math.ceil(segment.length/350));
  for(let i=0;i<segment.length;i+=step){
   const group=segment.slice(i,i+step);
   const low=group.reduce((a,r)=>r[key]<a[key]?r:a),high=group.reduce((a,r)=>r[key]>a[key]?r:a);
   const picks=[group[0],low,high,group.at(-1)];
   reduced.push(...[...new Set(picks)].sort((a,b)=>a.t-b.t));
  }
  add('polyline',{points:reduced.map(r=>sx(r.t)+','+sy(r[key])).join(' '),class:'series',fill:'none','stroke-width':2,'stroke-linejoin':'round'});
  if(segment.length===1)add('circle',{cx:sx(segment[0].t),cy:sy(segment[0][key]),r:3,class:'sample'});
 }
 const guide=add('line',{class:'cursor',y1:top,y2:bottom,visibility:'hidden'});
 const marker=add('circle',{class:'picked',r:5,visibility:'hidden'});
 let index=pts.findIndex(r=>rowIdentity(r)===pickedRows.get(id));
 function showReading(next){
  index=next;
  const r=pts[index],x=sx(r.t);
  pickedRows.set(id,rowIdentity(r));
  for(const [el,attrs] of [[guide,{x1:x,x2:x}],[marker,{cx:x,cy:sy(r[key])}]]){
   for(const [name,value] of Object.entries(attrs))el.setAttribute(name,value);
   el.setAttribute('visibility','visible');
  }
  const value=document.createElement('strong'),time=document.createElement('span'),node=document.createElement('span');
  value.textContent=r[key].toFixed(digits)+' '+unit;
  time.textContent=' · '+new Date(r.t).toLocaleString();
  node.textContent=r.node?'Transmitter '+r.node:'';
  detail.replaceChildren(value,time);
  if(r.node)detail.append(node);
 }
 if(index>=0)showReading(index);
 else {
  pickedRows.delete(id);
  detail.textContent='Tap the chart or use the arrow keys to inspect a reading.';
 }
 svg.onclick=e=>{
  const box=svg.getBoundingClientRect();
  const x=(e.clientX-box.left)/box.width*width,y=(e.clientY-box.top)/box.height*height;
  let nearest=0;
  for(let i=1;i<pts.length;i++){
   const dx=Math.abs(sx(pts[i].t)-x),best=Math.abs(sx(pts[nearest].t)-x);
   if(dx<best||(dx===best&&Math.abs(sy(pts[i][key])-y)<Math.abs(sy(pts[nearest][key])-y)))nearest=i;
  }
  showReading(nearest);
 };
 svg.onkeydown=e=>{
  if(!['ArrowLeft','ArrowRight','Home','End'].includes(e.key))return;
  e.preventDefault();
  const next=e.key==='Home'?0:e.key==='End'?pts.length-1:
   index<0?(e.key==='ArrowLeft'?pts.length-1:0):index+(e.key==='ArrowLeft'?-1:1);
  showReading(Math.min(pts.length-1,Math.max(0,next)));
 };
}

function renderCharts(source=rows.filter(r=>r.t>=selectedWindow())){
 for(const chart of chartSpecs)draw(chart,source);
}
function render(){
 const good=rows.filter(r=>finite(r.temp)&&r.quality==='ok'),last=good.at(-1);
 $('now').toggleAttribute('data-reading',Boolean(last));
 if(last){
  $('now').textContent=last.temp.toFixed(1)+' °F';
  $('fresh').textContent='Recorded '+new Date(last.t).toLocaleString();
  const target=last.t-86400000;
  const old=good.filter(r=>Math.abs(r.t-target)<=22.5*60000).sort((a,b)=>Math.abs(a.t-target)-Math.abs(b.t-target))[0];
  $('change').textContent=old?'Change since about 24 hours earlier: '+(last.temp-old.temp).toFixed(1)+' °F':'24-hour comparison not yet available';
 }else{
  $('now').textContent='No valid timestamped temperature';
  $('fresh').textContent='Pre-sync records remain in the CSV.';
  $('change').textContent='';
 }
 const source=rows.filter(r=>r.t>=selectedWindow());
 const values=source.filter(r=>finite(r.temp)&&r.quality==='ok').map(r=>r.temp);
 $('stats').hidden=!values.length;
 $('statsEmpty').textContent=values.length?'':'No valid temperatures in the selected period.';
 if(values.length){
  $('statCount').textContent=values.length;
  $('statLow').textContent=Math.min(...values).toFixed(1)+' °F';
  $('statHigh').textContent=Math.max(...values).toFixed(1)+' °F';
  $('statMean').textContent=(values.reduce((a,b)=>a+b,0)/values.length).toFixed(1)+' °F';
 }
 renderCharts(source);
 renderDiagnostics();
}
async function refresh(){if(busy)return;busy=true;try{const s=await fetch('/status',{cache:'no-store'});if(!s.ok)throw Error('Status request failed');status=await s.json();renderDiagnostics();let stale=!status.received||status.age_s>22.5*60;$('health').className=stale||status.storage_error?'warn':'';$('health').textContent=(stale?'No recent packet':'Packet received '+Math.floor(status.age_s/60)+' min ago')+' · '+(status.storage_error?'LOG WRITE ERROR':'Last save status OK')+' · saved this boot '+status.saved+' · rejected '+status.invalid;$('storage').className=status.near_full?'warn':'';$('storage').textContent=(status.near_full?'LOW STORAGE — save logs and free space. ':'')+status.storage+': '+(status.used/1024).toFixed(0)+' KiB used / '+(status.total/1024).toFixed(0)+' KiB total · '+((status.total-status.used)/1024).toFixed(0)+' KiB free';if(status.log_version===undefined||status.log_version!==loadedLogVersion){const r=await fetch('/log.csv',{cache:'no-store'});if(!r.ok)throw Error('Log unavailable');rows=parse(await r.text());loadedLogVersion=status.log_version;}render();$('notice').className='muted';$('notice').textContent='Updated '+new Date().toLocaleTimeString();}catch(e){$('notice').textContent=e.message+'; displayed data may be old';$('notice').className='warn';}finally{busy=false;}}
document.querySelectorAll('[data-h]').forEach(b=>b.onclick=()=>{
 const period=Number(b.dataset.h);
 if(period!==hours)pickedRows.clear();
 hours=period;
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
let chartWidth=$('temp').clientWidth;
if(typeof ResizeObserver==='function')new ResizeObserver(()=>{
 const width=$('temp').clientWidth;
 if(width!==chartWidth){chartWidth=width;renderCharts();}
}).observe($('temp'));
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
  handledRadioIrqs=radioIrqs;
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
  logVersion=esp_random();
  Serial.begin(115200);
  delay(300); // USB CDC startup; no indefinite wait for a connected computer.
  Serial.println("Bluegill receiver revision 20 / XIAO ESP32S3 / SH1106");
  Preferences displayPrefs;
  if(displayPrefs.begin("logger-display",true)){
    displayMode=displayPrefs.getUChar("mode",0);displayPrefs.end();
    if(displayMode>2)displayMode=0;
  }
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
  receiverSession=esp_random();loadDeployment();loadCalibration();loadReporting();loadSavedUpload();
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
  server.on("/display",HTTP_GET,handleDisplayGet);
  server.on("/display",HTTP_POST,handleDisplaySave);
  server.on("/pairing",HTTP_POST,handlePairing);
  server.on("/owner-link",HTTP_POST,handleOwnerLink);
  server.on("/reporting",HTTP_GET,handleReportingGet);
  server.on("/reporting",HTTP_POST,handleReportingSave);
  server.on("/saved-upload",HTTP_GET,handleSavedStatus);
  server.on("/saved-upload",HTTP_POST,handleSavedAction);
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

void serviceRadio(){
  const uint32_t events=radioIrqs;
  if(!radioReady||events==handledRadioIrqs)return;
  handledRadioIrqs=events;
  String msg;
  const int16_t readState=radio.readData(msg);
  float rssi=NAN,snr=NAN;
  if(readState==RADIOLIB_ERR_NONE){
    rssi=radio.getRSSI();
    snr=radio.getSNR();
  }else{
    ++radioReadErrors;
    if(readState==RADIOLIB_ERR_CRC_MISMATCH)++radioCrcErrors;
    radioLastError=readState;
  }
  const int16_t rearm=radio.startReceive();
  if(rearm!=RADIOLIB_ERR_NONE){
    ++radioRearmErrors;radioLastError=rearm;
    Serial.printf("RX start failed: %d\n",rearm);
    radioReady=false;
  }
  if(readState==RADIOLIB_ERR_NONE){
    logReading(msg,rssi,snr);
    updateDisplay();
  }
}
void loop() {
  serviceRadio();
  serviceButton();
  server.handleClient();
  serviceRadio();

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
  serviceReporting();
  serviceOLED();
  if(!radioReady && millis()-lastRadioRetry>=30000UL){lastRadioRetry=millis();radioReady=startRadio();}

  // Current-log clearing uses the confirmed webpage action. INIT NEW FLASH is accepted only when storage cannot mount.
  if (millis() - lastDisplayMillis > 30000) updateDisplay();   // keeps "heard X min ago" fresh
}
