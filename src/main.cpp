/*
TO DO:
* Find out why volume drops from 0% to 100%
* Split webserver to dedicated class
* Update UI
* Fix API-based volume (probably something in JSON or casting to)
*/

#include <Arduino.h>
#include "ESP32Encoder.h"
#include <driver/gpio.h>
#include <Wire.h>

#include <Preferences.h>

// THIS BLOCK NEEDED FOR WIFI / OTA UPDATES
#include <WiFi.h>
#include "ESPAsyncWebServer.h"
#include <ESPmDNS.h>
#include <Update.h>
#include <SPIFFS.h>
#include "esp_task_wdt.h"
#include "esp_int_wdt.h"
#include "ArduinoJson.h"


// OUR SYSTEM CONFIGURATION
#include "falk-pre-conf.h"

// FIRMWARE VERSION (THIS SW)
String fw_version = "0.1.16";

// OTHER INTERNAL CLASSES
#include "relays.h"
RelayController relays;

#include "display.h"
Display display;

Preferences preferences;
uint32_t FlashCommit = 0;

// for the volume rotary encoder
ESP32Encoder volEnc;

// for the input rotary encoder
ESP32Encoder inpEnc;

// to handle mute state
int muteState = 0;
int muteButtonState;
int lastmuteButtonState = HIGH;
unsigned long muteDebounceTime = 0;
uint32_t MuteRelayPulseTime = 0;

// wifi settings
uint32_t wifiButtonPressTime = 0;
uint32_t wifiConnectTimeout = 0;
uint8_t wifibuttonstate = HIGH;
AsyncWebServer server(80);
bool shouldReboot = false;

void configureServer() {
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // API CONTENT
  server.on("/api/status", HTTP_GET, [&](AsyncWebServerRequest *request){
    // create a JSON object for the response
    StaticJsonDocument<200> doc;
    JsonObject retObj = doc.to<JsonObject>();

    //generate the volume object
    JsonObject volObj = retObj.createNestedObject("volume");
    volObj["max"] = VOL_MAX;
    volObj["current"] = sysSettings.volume;
    
    //generate the inputs object
    JsonObject inpObj = retObj.createNestedObject("inputs");
    inpObj["selected"] = sysSettings.input;
    JsonArray inpList = inpObj.createNestedArray("list");
    for (int i = 0; i < INP_MAX; i++) {
      inpList.add(sysSettings.inputNames[i]);
    }
    //things to return:
    // settings?

    //generate the string
    String retStr;
    serializeJson(retObj, retStr);
    //return the request
    return request->send(200, "application/json", retStr);
  });

  server.on("/api/volume", HTTP_GET, [&](AsyncWebServerRequest *request){
    // create a JSON object for the response
    StaticJsonDocument<200> doc;
    JsonObject retObj = doc.to<JsonObject>();

    // write the volume variables
    retObj["max"] = VOL_MAX;
    retObj["current"] = sysSettings.volume;

    //generate the string
    String retStr;
    serializeJson(retObj, retStr);
    return request->send(200, "application/json", retStr);
  });
  server.on("/api/volume", HTTP_POST, [](AsyncWebServerRequest *request){
    // handle the instance where no data is provided
  }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    StaticJsonDocument<200> doc;
    deserializeJson(doc, data);
    if (doc.containsKey("volume")) {
      int vol = doc["volume"];
      sysSettings.volume = vol;
      relays.setVolume(sysSettings.volume);
    }
    // create a JSON object for the response
    doc.clear();
    JsonObject retObj = doc.to<JsonObject>();

    retObj["max"] = VOL_MAX;
    retObj["current"] = sysSettings.volume;

    //generate the string
    String retStr;
    serializeJson(retObj, retStr);
    return request->send(200, "application/json", retStr);
  });

  server.on("/api/firmware", HTTP_GET, [&](AsyncWebServerRequest *request){
    File appFile = SPIFFS.open("/version", "r");
    String app_version = appFile.readString();
    app_version = app_version.substring(0, app_version.length() -1);
    
    // create a JSON object for the response
    StaticJsonDocument<200> doc;
    JsonObject retObj = doc.to<JsonObject>();
    retObj["fw"] = fw_version;
    retObj["app"] = app_version;

    //generate the string
    String retStr;
    serializeJson(retObj, retStr);
    return request->send(200, "application/json", retStr);
  });
  
  server.on("/update", HTTP_POST, [&](AsyncWebServerRequest *request){
    shouldReboot = !Update.hasError();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot?"OK":"FAIL");
    response->addHeader("Connection", "close");
    request->send(response);
  },[&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
    if(!index){
      Serial.printf("Update Start: %s\n", filename.c_str());
      int cmd = (filename == "spiffs.bin") ? U_SPIFFS : U_FLASH;
      if(!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)){
        Update.printError(Serial);
        return request->send(200, "text/plain", "OTA could not begin");
      }
    }
    if(!Update.hasError()){
      if(Update.write(data, len) != len){
        Update.printError(Serial);
        return request->send(200, "text/plain", "OTA could not start");
      }
    }
    if(final){
      if(Update.end(true)){
        Serial.printf("Update Success: %uB\n", index+len);
        return request->send(200, "text/plain", "Success");
      } else {
        Update.printError(Serial);
        return request->send(200, "text/plain", "OTA could not end");
      }
    } else {
      return;
    }
  });


  server.serveStatic("/", SPIFFS, "/www/").setDefaultFile("index.html");

  server.begin();
}

const char* getSSID() {
  return "FALK-PRE";
}

void enableWifi() {
  WiFi.softAP(getSSID());
  IPAddress IP = WiFi.softAPIP();
  MDNS.begin(HOSTNAME);

  Serial.print("AP IP address: ");
  Serial.println(IP);

  display.wifiScreen(getSSID());

  configureServer();

  //wifiConnectTimeout = millis();
}


void setup(){	
	Serial.begin(9600);
  Serial.println("Booting...");
  //use the pullup resistors, this means we can connect ground to the encoders
	ESP32Encoder::useInternalWeakPullResistors=UP;

  //configure the MCP27013 ICs
  relays.begin();

  //start preferences
  preferences.begin("falk-pre", false);
  //preferences.clear();
  preferences.getBytes("settings", &sysSettings, sizeof(Settings));

  //add some default values if this is our first boot
  if (sysSettings.saved == 0) {
    sysSettings.saved = 1;
    for (int i = 0; i < INP_MAX; i++) {
      sysSettings.inputNames[i] = "Input " + (String)(i+1);
    }
  }

  //configure the input encoder
  inpEnc.attachSingleEdge(INP_ENCODER_A, INP_ENCODER_B);
  inpEnc.setCount(sysSettings.input);

  //configure the volume encoder
	volEnc.attachFullQuad(VOL_ENCODER_A, VOL_ENCODER_B);
  volEnc.setCount(sysSettings.volume);

  //configure the mute button
  pinMode(MUTE_BUTTON, INPUT);
  pinMode(MUTE_SET, OUTPUT);
  pinMode(MUTE_RESET, OUTPUT);

  //the relays *should* match our stored values (since they're latching) but we can't be sure
  //so we set them to these values so the screen and relays match
  relays.setVolume(sysSettings.volume);
  relays.setInput(sysSettings.input);

  display.begin();
  display.updateScreen(muteState);

  //this is the input rotary encoder button. Needed to handle wifi enable
  //pinMode(WIFI_BUTTON, INPUT_PULLUP);
  pinMode(WIFI_BUTTON, INPUT);

  enableWifi();
}

void muteHandler(uint32_t m) {
  int reading = digitalRead(MUTE_BUTTON);

  if (reading != lastmuteButtonState) {
    muteDebounceTime = m;
  }

  if ((m - muteDebounceTime) > BUTTON_DEBOUNCE_DELAY) {
    if (reading != muteButtonState) {
      muteButtonState = reading;

      if (muteButtonState == LOW) {
        if (muteState == 0) {
          muteState = 1;
          volEnc.pauseCount();
          display.updateScreen(muteState);
          digitalWrite(MUTE_SET, 1);
          MuteRelayPulseTime = millis();
        } else {
          muteState = 0;
          volEnc.resumeCount();
          display.updateScreen(muteState);
          digitalWrite(MUTE_RESET, 1);
          MuteRelayPulseTime = millis();
        }
      }
    }
  }
  lastmuteButtonState = reading;

  if ((MuteRelayPulseTime > 0) && (m > MuteRelayPulseTime + RELAY_PULSE)) {
    digitalWrite(MUTE_SET, 0);
    digitalWrite(MUTE_RESET, 0);
  }
}

void wifiLoop(uint32_t m) {
  int reading = digitalRead(WIFI_BUTTON);
  if (reading != wifibuttonstate) {
    Serial.println(reading);
    if (reading == LOW) {
      wifiButtonPressTime = m;
    } else {
      wifiButtonPressTime = 0;
    }
    wifibuttonstate = reading;
  } else if ((reading == LOW) && (wifiButtonPressTime > 0) && (m > wifiButtonPressTime + BUTTON_LONG_PRESS)) {
    enableWifi();
    wifiButtonPressTime = 0;
  }

  if ((wifiConnectTimeout > 0) && (m > wifiConnectTimeout + WIFI_TIMEOUT)) {
    wifiConnectTimeout = 0;
    WiFi.mode(WIFI_MODE_STA);
    display.updateScreen(muteState);
  }
}

void inputLoop(int m) {
  uint16_t count = inpEnc.getCount();
  if (count != sysSettings.input) {
    if (count > INP_MAX) {
      count = INP_MIN;
      inpEnc.setCount(INP_MIN);
    } else if (count < INP_MIN) {
      count = INP_MAX;
      inpEnc.setCount(INP_MAX);
    }
    relays.setInput(count);
    sysSettings.input = count;
    display.updateScreen(muteState);
    //set a delayed commit (this prevents us from wearing out the flash with each detent)
    FlashCommit = m;
  }
}

void volumeLoop(int m) {
  //gets the current volume, checks to see if it's changed and sets the new volume
  uint16_t count = volEnc.getCount();
  if (count != sysSettings.volume) {
    //if we're outside the limts, override with the limits
    if (count > VOL_MAX) {
      //restore back to the maximum volume
      volEnc.setCount(VOL_MAX);
      count = VOL_MAX;
    } else if (count < VOL_MIN) {
      //restore back to the minimum volume
      volEnc.setCount(VOL_MIN);
      count = VOL_MIN;
    }
    //check again (in case we reset to be inside the limits)
    if (count != sysSettings.volume) {
      //save the new volume
      sysSettings.volume = count;
      //write to the screen
      display.updateScreen(muteState);
      //set the relays
      relays.setVolume(count);
      //set a delayed commit (this prevents us from wearing out the flash with each detent)
      FlashCommit = m;
    } else {
      //just save the new volume
      sysSettings.volume = count;
    }
  }
}

void loop() {
  int m = millis();

  inputLoop(m);
  volumeLoop(m);
  muteHandler(m);
  wifiLoop(m);

  if ((FlashCommit > 0) && (m > FlashCommit + COMMIT_TIMEOUT)) {
      preferences.putBytes("settings", &sysSettings, sizeof(Settings));
      FlashCommit = 0;
  }

  if(shouldReboot) {
    yield();
    delay(1000);
    yield();
    esp_task_wdt_init(1,true);
    esp_task_wdt_add(NULL);
    while(true);
  }

  relays.loop();
  display.loop();
}