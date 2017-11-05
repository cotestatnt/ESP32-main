#include <ArduinoJson.h>
#include <Crypto.h>
#include <Speck.h>
#include <SHA256.h>
#include <SPI.h>
#include <RF24.h>
#include <RF24Network.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <SPIFFSEditor.h>

#include "HardwareSerialGSM.h"
#include <ESPTelegramBOT.h>

// Inizializzo valori bot telegram
#define BOTtoken "488075445:AAG5S_I2MHDOtMK8v4U8QhcogrKef1Yltd8"  //token of TestBOT
#define BOTname "Allarme"
#define BOTusername "AllarmeCotesta_bot"

TelegramBOT bot(BOTtoken, BOTname, BOTusername);
long Bot_lasttime;

//Some useful bit manipulation macros
#define BIT_MASK(bit)             (1 << (bit))
#define SET_BIT(value,bit)        ((value) |= BIT_MASK(bit))
#define CLEAR_BIT(value,bit)      ((value) &= ~BIT_MASK(bit))
#define TEST_BIT(value,bit)       (((value) & BIT_MASK(bit)) ? 1 : 0)

#define TIMECHECK 30                  // In seconds
#define NTP_OFFSET 2 * 60 * 60        // In seconds
#define NTP_ADDRESS "time.google.com" // NTP Server to connect
#define N_BLOCK 17
const char *ssid = "";
const char *password = "";
const char *hostName = "esp-async";
String adminPswd = "admin";
String adminPIN = "12345";

enum States {
  SYS_DISABLED = 0,
  SYS_ENABLED = 10,
  RUNNING = 11,
  CHECK_NODES = 13,
  PAUSED = 20,
  TIMED = 30,
  ALARMED = 99
} systemStatus;
enum payloadPointer {
  _NodeH = 0,
  _NodeL = 1,
  _Enabled = 2,
  _MsgType = 3,
  _Battery = 4,
  _TimeStampH = 5
};
enum messageType {
  SET_DISABLE = 100,
  SET_ENABLE = 110,
  SEND_ALIVE = 120,
  SEND_REPLY = 130,
  GET_REQUEST = 140,
  SEND_ALARM = 199
};

const byte TEST = 14;     // Relay K1
const byte ALARM_ON = 13; // Led D5
const byte HORN_ON = 12;  // Relay K2
const byte REED_IN = 34;   // Reed sensor input (Pull-UP)
const byte UsedPin[] = {REED_IN, HORN_ON, ALARM_ON, TEST};

// Webservices
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

// Define UDP instance for NTP Client
WiFiUDP ntpClient;

// Handle ESP32 preferences in local memory
Preferences preferences;

// Instantiate a Speck block ciphering & SHA256
SHA256 sha256;
Speck myCipher;
byte encryptKey[N_BLOCK] = {0x5C, 0x39, 0x38, 0x7B, 0x36, 0x60, 0x68, 0x23,
                            0x34, 0x34, 0x76, 0x5C, 0x34, 0x22, 0x62, 0x55};
byte payload[N_BLOCK];
byte cipherText[N_BLOCK];

// Instance of the radio driver
RF24 radio(2, 5);				        // nRF24L01(+) radio attached
RF24Network network(radio);     // Network uses that radio
const uint16_t this_node = 00;  // Address of our node in Octal format

struct Node {
  uint16_t id;
  uint8_t state;
  uint8_t battery;
  long lastTS;
};
Node nodes[10]; // Master always 0 + n nodes

// Global variables
uint16_t NewGPIO, OldGPIO = 0;
uint8_t nodeNumber, actualNode, nodesOK, oldStatus, pauseSeconds = 0;
unsigned long start_seconds, stop_seconds, epochDay, epochTime = 0;
unsigned long checkTime, pauseTime = 0;
unsigned long updateTime, waitTime, hornTime = millis();
String logMessage = "";

// Interrupt service routine for REED_IN input
void handleInterrupt() {
  if (systemStatus >= 10)  {
    Serial.println("Interrupt");
    systemStatus = ALARMED;
    hornTime = millis() + 15000;
  }
}

// ***************************************************************************************************** //
// *****************************************    SETUP   ************************************************ //
// ***************************************************************************************************** //
void setup() {
  pinMode(TEST, OUTPUT);
  pinMode(ALARM_ON, OUTPUT);
  pinMode(HORN_ON, OUTPUT);
  digitalWrite(HORN_ON, HIGH);
  digitalWrite(ALARM_ON, LOW);
  pinMode(REED_IN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(REED_IN), handleInterrupt, RISING);

  // Start Serial for debug
  Serial.begin(115200);  
  Serial.println();  
  //Serial1.begin(115200, SERIAL_8N1, 33, 4);
  A6begin();
  
  // Load admin credits. If not present (first time?) use default admin/admin
  preferences.begin("SmartAlarm", false);
  adminPIN = preferences.getString("adminPIN", adminPIN);
  adminPswd = preferences.getString("adminPswd", adminPswd);
  systemStatus =  static_cast<States>(preferences.getUInt("systemStatus", systemStatus));
  preferences.end();

  // Init nRF24 Network
  SPI.begin();
  radio.begin();
  network.begin(/*channel*/ 90, /*node address*/ this_node);

  // Init SPECK cipher
  myCipher.setKey(encryptKey, sizeof(encryptKey));

  // Start SPIFFS filesystem
  SPIFFS.begin(true);

  // Start wifi connection
  if (!loadWifiConf()) {
    Serial.print(F("Warning: Failed to load Wifi configuration.\nConnect to "));
    Serial.print(hostName);
    Serial.println(F(" , load 192.168.4.1/auth and update with your SSID and password."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(hostName);
  }

  // Normal wifi connection check doesn't work togheter with RF24 lib
  // We check connection status with checkWiFi() in main loop
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Configure and start Webserver
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  events.onConnect([](AsyncEventSourceClient *client) {
    client->send("Hello Client!", NULL, millis(), 1000);
  });
  server.addHandler(&events);
  server.addHandler(new SPIFFSEditor(SPIFFS, "admin", adminPswd.c_str()));
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");
  server.serveStatic("/auth/", SPIFFS, "/auth/")
		    .setDefaultFile("settings.htm")
		    .setAuthentication("admin", adminPswd.c_str());
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404); });
  server.begin();
}

// ***************************************************************************************************** //
// *****************************************    LOOP   ************************************************* //
// ***************************************************************************************************** //

bool RisingEdge = false;
long waitDial = millis();

void loop() {
  checkWiFiConnection();

  // Check the nRF24 network regularly
  network.update();
  getRadioData();

  // Check telegram message is present
  checkBotMessages();
  // Check if new unread SMS is present
  checkSMS(false);
  // Send AT command from terminal ()
  String inputString = "";
  while (Serial.available() > 0) {
	  char inChar = (char)Serial.read();	  
	  inputString += inChar;
	  if (inChar == '\n') {
		  A6command(inputString.c_str(), "OK", "yy", A6_CMD_TIMEOUT, 1, NULL, true);
	  }
  }


  /*
  if(systemStatus != oldStatus){
  Serial.print(" ."); Serial.print(systemStatus);
  oldStatus = systemStatus;
  delay(500);
  }
  */

  
  if ((digitalRead(TEST) == HIGH) && (!RisingEdge)) {
    RisingEdge = true;	
    dial("3934191877");
    waitDial = millis();    
  }
  if (millis() - waitDial > 2000) {
    RisingEdge = false;
    hangUp();	
  } 

  switch (systemStatus) {
  // Dummy replay (in order to put nodes in deep sleep);
  case SYS_DISABLED:
    digitalWrite(HORN_ON, HIGH);
    digitalWrite(ALARM_ON, HIGH);
    break;

  // Run alarm system
  case SYS_ENABLED:
    // Update timestamp before re-enable the system
    digitalWrite(HORN_ON, HIGH);
    digitalWrite(ALARM_ON, HIGH);
    for (byte i = 0; i < 7; i++)
      nodes[i].lastTS = epochDay;
    systemStatus = RUNNING;
    break;

  // Wait for node alive messages and check nodes
  case RUNNING:
    if (epochDay == stop_seconds) {
      digitalWrite(HORN_ON, HIGH);
      digitalWrite(ALARM_ON, HIGH);
      systemStatus = TIMED;
      break;
    }
    // Send random message to the alive node
    if (payload[_MsgType] == SEND_REPLY) {
      uint16_t fromNode = (payload[_NodeH] << 8) | payload[_NodeL];
      unsigned long TimeStamp = (payload[_TimeStampH] << 16) | (payload[_TimeStampH + 1] << 8) | payload[_TimeStampH + 2];
      nodes[fromNode].id = fromNode;
      nodes[fromNode].state = payload[_Enabled];
      nodes[fromNode].battery = payload[_Battery];
      nodes[fromNode].lastTS = TimeStamp;
      systemStatus = CHECK_NODES;
      waitTime = millis();
    }
    break;

  // Check if all nodes has sent message in last TIMECHECK seconds
  case CHECK_NODES:
    systemStatus = RUNNING;
    nodesOK = 0;
    for (byte i = 0; i < 7; i++) {
      long elapsedtime = epochDay - nodes[i].lastTS;
      if (elapsedtime > TIMECHECK) {
        // if sensor is disabled dont't set alarm
        if (nodes[i].state != 0) {
          logMessage = "\nSensor " + String(nodes[i].id) + " not respond since " + String(elapsedtime) + " seconds";
          Serial.println(logMessage);
          systemStatus = ALARMED;
          delay(1000);
        }
      } else
        nodesOK++;
    }
    // Something wrong -> set Alarm state
    if (nodesOK < nodeNumber) {
      systemStatus = ALARMED;
      digitalWrite(ALARM_ON, LOW);
      logMessage = "\nALARM! Sensors active " + String(nodesOK) + "/" + String(nodeNumber);
      Serial.println(logMessage);
      delay(200);
      hornTime = millis();
    }
    break;

  // Only to store the actual status and use it after if restart
  case TIMED:
    // timeClient.update();
    if (epochDay == start_seconds) {
      // Update nodes timestamp before re-enable the system
      for (byte i = 0; i < 7; i++)
        nodes[i].lastTS = millis();
      Serial.println("System ENABLED");
      systemStatus = SYS_ENABLED;
      logMessage = "\nTime: " + String(epochDay) + " seconds\nStart: " + String(start_seconds) + " seconds\nStop: " + String(stop_seconds) + " seconds\n";
      Serial.println(logMessage);
    }
    break;

  // wait a defined time and after re-enable the system
  case PAUSED:
    if (millis() - pauseTime > pauseSeconds * 1000) {
      pauseTime = millis();
      // Update timestamp before re-enable the system
      for (byte i = 0; i < 7; i++)
        nodes[i].lastTS = pauseTime;
      systemStatus = SYS_ENABLED;
    }
    break;

  // Ops, we have an alarm: send message to clients and turn on the horn (after 15seconds) 
  // At this point, status will be changed only after authorized user action
  case ALARMED:
    digitalWrite(ALARM_ON, LOW);
    // Activate the horn after user defined time elapsed
    if (millis() - hornTime > 15000) {
      hornTime = millis();
      digitalWrite(HORN_ON, LOW);
      Serial.println("Acustic segnalator ON");
    }
    break;
  }

  // Update client about our status
  if (millis() - checkTime > 1000) {
    checkTime = millis();
    char str[2];
    sprintf(str, "%d", systemStatus);
    sendDataWs((char *)"status", str);
  }

  // Read status of all used pins and store in NewGPIO var
  NewGPIO = 0;
  for (byte i = 0; i < 16; i++)
    for (byte j = 0; j < sizeof(UsedPin); j++)
      if((UsedPin[j] == i)&&(digitalRead(i) == HIGH))
          SET_BIT(NewGPIO, i);
  // Use virtual GPIO15 for REED_IN (Normal Closed)
  if(digitalRead(REED_IN) == LOW)
    SET_BIT(NewGPIO, 15);
      

  // Check if some of pins has changed and sent message to clients
  if (NewGPIO != OldGPIO) {
    delay(50);
    OldGPIO = NewGPIO;
    char str[2];
    sprintf(str, "%d", systemStatus);
    sendDataWs((char *)"status", str);
  }
}

// ***************************************************************************************************** //
// *****************************************    RF24    ************************************************ //
// ***************************************************************************************************** //

// Wait for a message addressed to Master from one of nodes
void getRadioData(void) {
  
  network.update();
  while (network.available()) {
    RF24NetworkHeader header;
    network.read(header, &cipherText, sizeof(cipherText));
    myCipher.decryptBlock(payload, cipherText);   
        
    Serial.print(F("Node "));
    Serial.print(header.from_node);
    Serial.print(F(": "));
    printHex(payload, sizeof(payload));   
        
    // Check if this node has to be disabled or enabled
    /*
    if (nodes[header.from_node].state != payload[_Enabled]) {
      payload[_MsgType] = 100 + nodes[header.from_node].state * 10; // SET_DISABLE = 100, SET_ENABLE = 110,
      Serial.print("Set sensor n° ");
      Serial.print(header.from_node);
      Serial.print(" :");
      Serial.print(nodes[header.from_node].state ? F(" ENABLED\n") : F(" DISABLED\n"));
      Serial.print(sendRadioData(header.from_node) ? F(" OK \n") : F(" Fail \n"));      
    }
    */

    if (payload[_MsgType] == SEND_ALIVE) {
      payload[_TimeStampH + 0] = (byte)((epochDay >> 16) & 0xff);
      payload[_TimeStampH + 1] = (byte)((epochDay >> 8) & 0xff);
      payload[_TimeStampH + 2] = (byte)(epochDay & 0xff);
      payload[_MsgType] = GET_REQUEST;
      Serial.print(sendRadioData(header.from_node) ? F(" ..OK\n") : F(" ..Fail\n"));      
    }

    if (payload[_MsgType] == SEND_REPLY)
      Serial.println();

    if (payload[_MsgType] == SEND_ALARM)
      systemStatus = ALARMED;
  }
  
}

// Send message to one of nodes
bool sendRadioData(uint16_t toNode) {
  bool TxOK = false;
  Serial.print(F("\nMaster: "));
  printHex(payload, sizeof(payload));
    
  myCipher.encryptBlock(cipherText, payload);
  radio.stopListening();
  radio.flush_tx();
  RF24NetworkHeader header(toNode);
  if (network.write(header, &cipherText, sizeof(cipherText)))
    TxOK = true;
  radio.startListening();
  return TxOK;
}

// ***************************************************************************************************** //
// ***************************************    WebSocket    ********************************************* //
// ***************************************************************************************************** //

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type){
    case WS_EVT_CONNECT:
         Serial.printf("Client %u connected. Send ping\n", client->id());
         client->printf("{\"ts\":%lu,\"cmd\":\"status\",\"msg\":\"Client id %u connected\"}", epochTime, client->id());
         client->ping();
         break;
    case WS_EVT_DISCONNECT:
          Serial.printf("Client %u disconnected\n", client->id());
          break;
    case WS_EVT_ERROR:
          Serial.printf("Client error(%u): %s\n", client->id(), (char*)data);
          break;
    case WS_EVT_PONG:
          Serial.printf("Client %u pong\n", client->id());
          client->printf("{\"ts\":%lu,\"cmd\":\"jsVars\",\"timePause\":\"%u\",\"timeHorn\":\"%u\"}", epochTime, pauseSeconds, 15);
          break;
    case WS_EVT_DATA:
          AwsFrameInfo *info = (AwsFrameInfo *)arg;
          String msg = "";
          if (info->final && info->index == 0 && info->len == len) {
            Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(),  client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);
            for (size_t i = 0; i < info->len; i++) 
              msg += (char)data[i];     
            Serial.println(msg);           
          }
          // A message from browser, take action according to our purpose
          processWsMsg(msg);
          break;  
  } // end of switch
}

// Encodes JSON Object and Sends it to All WebSocket Clients
void sendDataWs(char *msgType, char *msg) {
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.createObject();
  JsonArray &nodeIds = root.createNestedArray("nodeid");
  JsonArray &stat = root.createNestedArray("state_battery");
  root["ts"] = epochTime;
  root["cmd"] = msgType;
  root["msg"] = msg;
  root["gpio"] = NewGPIO;
  for (byte i = 0; i < 7; i++) {
    int _stat = nodes[i].state * 100 + nodes[i].battery;
    nodeIds.add(nodes[i].id);
    stat.add(_stat);
  }
  size_t len = root.measureLength();
  AsyncWebSocketMessageBuffer *buffer =  ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    root.printTo((char *)buffer->get(), len + 1);
    ws.textAll(buffer);
  }
}

// Web Browser sends some commands, check which command is given
void processWsMsg(String msg) {
  // We got a JSON object from browser, parse it
  DynamicJsonBuffer jsonBuffer;
  JsonObject &root = jsonBuffer.parseObject(msg);
  if (!root.success()) {
    Serial.println("Error parsing JSON message");
    return;
  }

  const char *command = root["command"];
  // Activate ore deactivete an output pin
  if (strcmp(command, "setOutput") == 0) {
    String pinName = root["pinName"];
    int pin = root["pin"];
    int state = root["state"];
    digitalWrite(pin, state);
    Serial.printf("\nSet pin %s %u", pinName.c_str(), state);
  }
  // Check if the PIN provided is correct
  else if (strcmp(command, "checkThisHash") == 0) {
    String testHash = root["testHash"];
    if (hashThis(adminPIN, testHash)) {
      sendDataWs((char *)"checkThisHash", (char *)"true");
      // PIN is correct -> disable acustic segnalation
      digitalWrite(HORN_ON, HIGH);
      Serial.println(F("Hash result: OK"));
    } else {
      sendDataWs((char *)"checkThisHash", (char *)"false");
      Serial.println(F("Hash result: not OK"));
    }

  }
  // Change system status with user actions
  else if (strcmp(command, "pause") == 0) {
    pauseTime = millis();
    systemStatus = PAUSED;
  } 
  else if (strcmp(command, "timer") == 0)
    systemStatus = TIMED;
  else if (strcmp(command, "alarmOn") == 0)
    systemStatus = SYS_ENABLED;
  else if (strcmp(command, "alarmOff") == 0)
    systemStatus = SYS_DISABLED;

  // Enable/disable single sensor
  else if (strcmp(command, "sensorToggle") == 0) {
    uint16_t _node = root["sensor"];
    Serial.print("Sensor n° ");
    Serial.print(_node);
    Serial.print(" is");
    Serial.print(nodes[_node].state ? F(" ENABLED\n") : F(" DISABLED\n"));
    bool enabled = nodes[_node].state;
    nodes[_node].state = !enabled;
  }

  // Manage admin and configuration data
  else if (strcmp(command, "saveconfig") == 0) {
    Serial.println(F("Saving config.json..."));
    File configFile = SPIFFS.open("/auth/config.json", "w+");
    if (!configFile)
      Serial.println(F("\nFailed to open for write config.json."));
    configFile.print(msg);
    configFile.close();
    delay(1000);
    ESP.restart();
  }

  // Save admin credits into internal memory for security reason
  else if (strcmp(command, "saveAdmin") == 0) {
    adminPswd = root["adminPswd"].as<String>();
    adminPIN = root["pin"].as<String>();
    preferences.begin("SmartAlarm", false);
    preferences.putString("adminPIN", adminPIN);
    preferences.putString("adminPswd", adminPswd);
    preferences.end();
    // Wait 5 seconds and restart ESP
    Serial.println("Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }

  // Provide config.json to admin settings page
  else if (strcmp(command, "getconf") == 0) {
    File configFile = SPIFFS.open("/auth/config.json", "r");
    if (configFile) {
      size_t len = configFile.size();
      AsyncWebSocketMessageBuffer *buffer =  ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
      if (buffer) {
        configFile.readBytes((char *)buffer->get(), len + 1);
        ws.textAll(buffer);
      }
      configFile.close();
    }
  }

  // Save new status in case of reboot
  Serial.print(F("\nAlarm system: "));
  Serial.println(systemStatus);
  preferences.begin("SmartAlarm", false);
  preferences.putUInt("systemStatus", systemStatus);
  preferences.end();
}

// ***************************************************************************************************** //
//************************************  OTHER FUNCTION   *********************************************** //
// ***************************************************************************************************** //

// Check if provided hash is correct (used for PIN)
bool hashThis(String Text, String testHash) {
#define HASH_SIZE 32
#define PIN_SIZE 5
  byte hashBuf[HASH_SIZE];
  char myHash[HASH_SIZE + 1];
  Serial.println("Test HASH");

  sha256.reset();
  sha256.update(Text.c_str(), PIN_SIZE);
  sha256.finalize(hashBuf, HASH_SIZE);

  for (int i = 0; i < HASH_SIZE; i++)
    sprintf(myHash + 2 * i, "%02X", hashBuf[i]);
  Serial.println(String(myHash));
  if (String(myHash) == testHash)
    return true;
  else
    return false;
}

// Try to load Wifi configuration from config.json
bool loadWifiConf(void) {
  File configFile = SPIFFS.open("/auth/config.json", "r");
  if (!configFile)
    return false;
  size_t size = configFile.size();
  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  DynamicJsonBuffer jsonBuffer;
  JsonObject &json = jsonBuffer.parseObject(buf.get());
  if (!json.success())
    return false;

  // Parse json config file
  nodeNumber = json["nodeNumber"];
  String startTime, stopTime = "";
  json["startTime"].printTo(startTime);
  json["stopTime"].printTo(stopTime);
  start_seconds = startTime.substring(1, 3).toInt() * 3600 + startTime.substring(4, 6).toInt() * 60;
  stop_seconds = stopTime.substring(1, 3).toInt() * 3600 + stopTime.substring(4, 6).toInt() * 60;
  pauseSeconds = json["pauseTime"];
  ssid = json["ssid"];
  password = json["pswd"];
  bool dhcp = json["dhcp"];
  if (!dhcp) {
    IPAddress ip, gateway, subnetmask;
    const char *strIpAddress = json["ipAddress"]; // IP Address from config file
    const char *strGateway = json["gateway"]; // Set gateway to match your network (router)
    const char *strSubnetmask = json["subnetmask"]; // Set subnet mask to match your network
    ip.fromString(strIpAddress);
    gateway.fromString(strGateway);
    subnetmask.fromString(strSubnetmask);
    if (!WiFi.config(ip, gateway, subnetmask))
      Serial.println("Warning: Failed to manual setup wifi connection. I'm going to use dhcp");
  }
  return true;
}

void checkWiFiConnection() {
  static volatile bool wifi_connected = false;
  static uint32_t wifi_timeout = millis();
  // If wifi connected print local IP and start NTP client
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifi_connected) {
      Serial.print("\nWiFi connected. IP address: ");
      Serial.println(WiFi.localIP());
      ntpClient.begin(2390);
      delay(100);
      ntpUpdateTime();
      // Set all node to known state
      for (byte i = 0; i < 7; i++) {
        nodes[i].id = 0;
        nodes[i].state = 0;
        nodes[i].lastTS = epochDay;
      }
    }
    wifi_connected = true;
    ntpUpdateTime();
  } 
  else {
    wifi_connected = false;
    if (millis() - wifi_timeout > 10000) {
      Serial.println("STA: Failed!");
      WiFi.disconnect(false);
      WiFi.mode(WIFI_AP);
      WiFi.softAP("ESP32_AP");
      delay(1000);
      WiFi.begin(ssid, password);
      wifi_connected = true;
      Serial.print("\nWiFi Access Point. IP address: ");
      Serial.println(WiFi.localIP());
    }
  }
}

void ntpUpdateTime(void) {
  static uint32_t ntp_timeout, OneSecond = 0;
  const int NTP_PACKET_SIZE = 48;
  byte ntpPacketBuffer[NTP_PACKET_SIZE];

  uint32_t Now = millis();
  if (Now - OneSecond >= 1000) {
    OneSecond = Now;
    epochDay++;
    epochTime++;
  }

  // Update time from NTP server every 60 seconds
  if ((Now - ntp_timeout > 60000) || (epochDay <= 1)) {
    ntp_timeout = Now;
    IPAddress address;
    WiFi.hostByName(NTP_ADDRESS, address);
    memset(ntpPacketBuffer, 0, NTP_PACKET_SIZE);
    ntpPacketBuffer[0] = 0b11100011; // LI, Version, Mode
    ntpPacketBuffer[1] = 0;          // Stratum, or type of clock
    ntpPacketBuffer[2] = 6;          // Polling Interval
    ntpPacketBuffer[3] = 0xEC;       // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    ntpPacketBuffer[12] = 49;
    ntpPacketBuffer[13] = 0x4E;
    ntpPacketBuffer[14] = 49;
    ntpPacketBuffer[15] = 52;
    ntpClient.beginPacket(address, 123); // NTP requests are to port 123
    ntpClient.write(ntpPacketBuffer, NTP_PACKET_SIZE);
    ntpClient.endPacket();
  }
  int packetLength = ntpClient.parsePacket();
  if (packetLength) {
    if (packetLength >= NTP_PACKET_SIZE) {
      ntpClient.read(ntpPacketBuffer, NTP_PACKET_SIZE);
    }
    ntpClient.flush();
    uint32_t secsSince1900 = (uint32_t)ntpPacketBuffer[40] << 24 |
                             (uint32_t)ntpPacketBuffer[41] << 16 |
                             (uint32_t)ntpPacketBuffer[42] << 8 |
                             ntpPacketBuffer[43];
    // Serial.printf("Seconds since Jan 1 1900: %u\n", secsSince1900);
    epochTime = secsSince1900 - 2208988800UL + NTP_OFFSET;
    // Serial.printf("EPOCH: %u\n", epoch);
    uint8_t h = (epochTime % 86400L) / 3600;
    uint8_t m = (epochTime % 3600) / 60;
    uint8_t s = (epochTime % 60);
    // Serial.printf("UTC: %02u:%02u:%02u (GMT)\n", h, m, s);
    epochDay = h + m + s;
  }
}


// Helper routine to dump a byte array as hex values to Serial.
void printHex(byte *buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], HEX);
  }
}


// Helper routine to dump a byte array as dec values to Serial.
void printDec(byte *buffer, byte bufferSize) {
  for (byte i = 0; i < bufferSize; i++) {
    Serial.print(buffer[i] < 0x10 ? " 0" : " ");
    Serial.print(buffer[i], DEC);
  }
}



// ***************************************************************************************************** //
// *********************************  TELEGRAM MESSAGES  *********************************************** //
// ***************************************************************************************************** //

void checkBotMessages() {
	String message = "";
	if (millis() - Bot_lasttime > 10000) {
		int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
		while (numNewMessages) {
			Serial.print("Bot: ");
			for (int i = 0; i<numNewMessages; i++) {
				message = bot.messages[i].text;
				Serial.println(message);
				// parse received message
				if (message != ""){
					if (message == "Antifurto ON") {
						systemStatus = SYS_ENABLED;
						bot.sendMessage(bot.messages[i].chat_id, "Imposto Antifurto ON", "");
					}

					else if (message == "Antifurto OFF") {
						systemStatus = SYS_DISABLED;
						bot.sendMessage(bot.messages[i].chat_id, "Imposto Antifurto OFF", "");
					}

					else if (message == "/start") {
						bot.sendMessage(message, "Ciao Tolentino, cosa posso fare?", "");
					}          
					else 
						bot.sendMessage(bot.messages[i].chat_id, "Hai scritto: " + message, "");
					
				}
				
			}
			numNewMessages = bot.getUpdates(bot.last_message_received + 1);
		}
		Bot_lasttime = millis();
	}

	
  
}

// ***************************************************************************************************** //
// *************************************  SMS MESSAGES  ************************************************ //
// ***************************************************************************************************** //

void checkSMS(bool deleteAfterRead) {
	int unreadSMSLocs[30] = { 0 };
	int unreadSMSNum = 0;
	SMSmessage sms;
	static long smsLastTime = millis();
	if (millis() - smsLastTime > 5000) {
		smsLastTime = millis();
		// Get the memory locations of unread SMS messages.
		unreadSMSNum = getUnreadSMSLocs(unreadSMSLocs, 30);
		for (int i = 0; i < unreadSMSNum; i++) {
			Serial.print("New message at index: ");
			Serial.println(unreadSMSLocs[i], DEC);

			sms = readSMS(unreadSMSLocs[i]);
			Serial.println(sms.number);
			Serial.println(sms.date);
			Serial.println(sms.message);

			if(deleteAfterRead)
				deleteSMS(i);
		}
	}
}