// Flash sleep fix revision 2 — 2026-11-02
// Southern Bluegill / XIAO nRF52840 + THROUGH-HOLE Wio-SX1262 kit (102010710).
// See README.md for pinned core/libraries, wiring and verification limits.
// Enabled point-to-point LoRa. These settings are NOT a compliance certification.
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_SPIFlash.h>
#include <math.h>

#if !defined(ARDUINO_ARCH_NRF52) || !defined(NRF52840_XXAA)
#error Select Seeed XIAO nRF52840 in the NON-MBED Seeed nRF52 Boards package.
#endif

constexpr uint8_t PROBE_PIN = D6; // labelled D6 on the XIAO, NOT Heltec GPIO4
constexpr uint32_t REPORT_INTERVAL_MS = 900000UL;
constexpr float RADIO_MHZ = 907.8f, RADIO_BW_KHZ = 500.0f;
constexpr uint8_t RADIO_SF = 9, RADIO_CR = 7;
constexpr int8_t RADIO_DBM = 10; // 10 mW requested; no amplifier
constexpr float BATTERY_CALIBRATION = 1.0f;
constexpr float BATTERY_DIVIDER = (1000000.0f + 510000.0f) / 510000.0f;

// Kit radio connections: NSS=D4, IRQ=D1, reset=D2, busy=D3, RX enable=D5.
SX1262 radio = new Module(D4, D1, D2, D3);
OneWire oneWire(PROBE_PIN);
DallasTemperature sensors(&oneWire);
Adafruit_FlashTransport_QSPI flashTransport;
// The flash is unused; only bounded low-level control commands are needed.
char nodeID[17]; // full 64-bit factory identifier, 16 hex digits; unchanged packet schema
uint32_t bootID=0, sequence=0, nextReport=0;
bool radioReady=false, identityReady=false;

void sleepExternalFlash() {
  // Adafruit SPIFlash 5.1.1's default high-level device list omits P25Q16H.
  // Use its transport only: no writes, erase, filesystem, or status-register changes.
  flashTransport.begin();
  bool woke=flashTransport.runCommand(0xAB); // release any previous deep power-down
  delay(10); // P25Q16H wake-up time; also handles reset while flash was asleep
  uint8_t id[3]={0,0,0};
  bool readOK=woke && flashTransport.readCommand(0x9F,id,sizeof(id));
  Serial.print("External flash JEDEC ID: 0x");
  for(unsigned i=0;i<3;i++){if(id[i]<16)Serial.print('0');Serial.print(id[i],HEX);}
  Serial.println();
  bool recognized=readOK && id[0]==0x85 && id[1]==0x60 && id[2]==0x15;
  bool slept=false;
  if(recognized){
    // Do not command power-down during a write left by a previous application.
    uint32_t started=millis();
    uint8_t status=0xFF;
    bool statusOK=false;
    do{
      statusOK=flashTransport.readCommand(0x05,&status,1);
      if(!statusOK || !(status&0x01))break;
      delay(1);
    }while(millis()-started<100);
    if(statusOK && !(status&0x01)){
      slept=flashTransport.runCommand(0xB9);
      delayMicroseconds(10); // tDP before releasing the peripheral
    }
  }
  // ALWAYS release QSPI, including unknown ID or command failure paths.
  flashTransport.end();
  // Keep chip select inactive; do not let floating CS negate the sleep command.
  digitalWrite(PIN_QSPI_CS,HIGH);pinMode(PIN_QSPI_CS,OUTPUT);
  if(slept)Serial.println("External flash: sleep command sent; QSPI released.");
  else Serial.println("External flash: sleep NOT confirmed; QSPI released. Save the JEDEC ID for diagnosis.");
  // A successful control transfer is not a measurement of sleep current.
}

uint32_t randomBootID() {
  uint32_t value=0;
  NRF_RNG->EVENTS_VALRDY=0;
  NRF_RNG->TASKS_START=1;
  for(unsigned i=0;i<4;i++) {
    while(!NRF_RNG->EVENTS_VALRDY) yield();
    value=(value<<8) | (NRF_RNG->VALUE & 0xFF);
    NRF_RNG->EVENTS_VALRDY=0;
  }
  NRF_RNG->TASKS_STOP=1;
  return value;
}

float batteryVolts() {
  // nRF52840 uses its own ADC/reference and divider; Heltec's 4.9 factor is wrong here.
  pinMode(VBAT_ENABLE,OUTPUT); digitalWrite(VBAT_ENABLE,LOW);
  analogReference(AR_INTERNAL_3_0); analogReadResolution(12);
  delay(20); (void)analogRead(PIN_VBAT);
  uint32_t sum=0;
  for(unsigned i=0;i<16;i++){sum+=analogRead(PIN_VBAT);delay(2);}
  digitalWrite(VBAT_ENABLE,HIGH);
  return (sum/16.0f) * (3.0f/4095.0f) * BATTERY_DIVIDER * BATTERY_CALIBRATION;
}

bool startRadio() {
  // RX off in standby/transmit/sleep; SX1262 DIO2 drives the transmit switch.
  pinMode(D5,OUTPUT);digitalWrite(D5,LOW);
  SPI.begin();
  int16_t state=radio.begin(RADIO_MHZ,RADIO_BW_KHZ,RADIO_SF,RADIO_CR,
      RADIOLIB_SX126X_SYNC_WORD_PRIVATE,RADIO_DBM,8,1.8);
  if(state==RADIOLIB_ERR_NONE)state=radio.setDio2AsRfSwitch(true);
  if(state==RADIOLIB_ERR_NONE)state=radio.setCRC(true);
  if(state!=RADIOLIB_ERR_NONE){Serial.print("Radio initialization failed: ");Serial.println(state);return false;}
  return true;
}

void reportReading() {
  if(!identityReady){Serial.println("Factory identifier invalid; report skipped.");return;}
  sensors.requestTemperatures(); // waits for a real conversion; no power-up default reading
  float c=sensors.getTempCByIndex(0);
  if(c==DEVICE_DISCONNECTED_C || !isfinite(c)) {
    delay(100); sensors.requestTemperatures();c=sensors.getTempCByIndex(0);
  }
  bool valid=isfinite(c) && c!=DEVICE_DISCONNECTED_C && c>=-55.0f && c<=125.0f;
  float v=batteryVolts();
  char temperature[16], voltage[16], message[112];
  if(valid)snprintf(temperature,sizeof(temperature),"%.1f",DallasTemperature::toFahrenheit(c));
  else temperature[0]=0;
  snprintf(voltage,sizeof(voltage),"%.2f",v);
  ++sequence;
  // Decimal boot/sequence fields and seven-column protocol match the existing receiver.
  snprintf(message,sizeof(message),"1,%s,%lu,%lu,%s,%s,%s",nodeID,
      (unsigned long)bootID,(unsigned long)sequence,temperature,voltage,
      valid?"ok":"sensor_error");
  if(!radioReady)radioReady=startRadio(); // bounded: one initialization attempt per report
  if(radioReady) {
    int16_t result=radio.transmit(message);
    Serial.print("TX result ");Serial.print(result);Serial.print(": ");Serial.println(message);
    int16_t sleepResult=radio.sleep();
    if(sleepResult!=RADIOLIB_ERR_NONE){Serial.print("Radio sleep failed: ");Serial.println(sleepResult);radioReady=false;}
    if(result!=RADIOLIB_ERR_NONE)radioReady=false;
  } else {Serial.print("Not sent: ");Serial.println(message);}
  // Leave the 1-Wire bus pulled HIGH and DS18B20 in powered standby.
  oneWire.depower();
}

void setup() {
  Serial.begin(115200);
  uint32_t started=millis();while(!Serial && millis()-started<2000)delay(10);
  pinMode(LED_RED,OUTPUT);digitalWrite(LED_RED,HIGH);
  pinMode(LED_GREEN,OUTPUT);digitalWrite(LED_GREEN,HIGH);
  pinMode(LED_BLUE,OUTPUT);digitalWrite(LED_BLUE,HIGH);
  pinMode(VBAT_ENABLE,OUTPUT);digitalWrite(VBAT_ENABLE,HIGH);
  // Keep the board's conservative 50 mA charge selection; do not drive HICHG low.
  pinMode(PIN_CHARGING_CURRENT,INPUT);
  snprintf(nodeID,sizeof(nodeID),"%08lX%08lX",
      (unsigned long)NRF_FICR->DEVICEID[1],(unsigned long)NRF_FICR->DEVICEID[0]);
  identityReady=!((NRF_FICR->DEVICEID[0]==0 && NRF_FICR->DEVICEID[1]==0) ||
    (NRF_FICR->DEVICEID[0]==UINT32_MAX && NRF_FICR->DEVICEID[1]==UINT32_MAX));
  bootID=randomBootID();
  Serial.print("Transmitter ID: ");Serial.println(nodeID);
  Serial.println("LoRa ENABLED: 907.8 MHz / 500 kHz / SF9 / CR4/7 / +10 dBm");
  sleepExternalFlash();
  sensors.begin();sensors.setWaitForConversion(true);sensors.setResolution(12);
  Serial.print("DS18B20 devices found: ");Serial.println(sensors.getDeviceCount());
  if(sensors.getDeviceCount()!=1)Serial.println("Use one probe per transmitter for this revision.");
  radioReady=startRadio();
  nextReport=millis();
}

void loop() {
  uint32_t now=millis();
  if((int32_t)(now-nextReport)>=0) {
    reportReading();
    do{nextReport+=REPORT_INTERVAL_MS;}while((int32_t)(millis()-nextReport)>=0);
  }
  // FreeRTOS task delay permits System ON idle sleep and keeps the timed wakeup.
  // Do NOT use SYSTEMOFF: it cannot wake itself every 15 minutes without extra hardware.
  int32_t remaining=(int32_t)(nextReport-millis());
  delay(remaining>10000?10000UL:(remaining>0?(uint32_t)remaining:1UL));
}