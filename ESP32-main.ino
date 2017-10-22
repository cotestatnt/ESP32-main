#include <RHReliableDatagram.h>
#include <RH_NRF24.h>
#include <SPI.h>
#include <Crypto.h>
#include <Speck.h>
#include <SHA256.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>  
#include <ESPAsyncWebServer.h>
#include <SPIFFSEditor.h>
#include <NTPClient.h>    
#include <Preferences.h>

const char* ssid = "";
const char* password = "";
const char* hostName = "esp-async";
String adminPswd = "admin";
String adminPIN = "12345";

//Some useful bit manipulation macros
#define BIT_MASK(bit)             (1 << (bit))
#define SET_BIT(value,bit)        ((value) |= BIT_MASK(bit))
#define CLEAR_BIT(value,bit)      ((value) &= ~BIT_MASK(bit))
#define TEST_BIT(value,bit)       (((value) & BIT_MASK(bit)) ? 1 : 0)

#define TIMECHECK		20					  // In seconds
#define NTP_OFFSET		2*60*60               // In seconds
#define NTP_ADDRESS		"time.google.com"     // NTP Server to connect
#define N_BLOCK			17
#define SERVER_ADDRESS	0

enum States { SYS_DISABLED = 0, SYS_ENABLED = 10, RUNNING = 11, WAIT_NODE = 12, CHECK_NODES = 13, PAUSED = 20, TIMED = 30, ALARMED = 99 } systemStatus;
const byte TEST = 14;			// Relay K1
const byte ALARM_ON = 13;		// Led D5
const byte HORN_ON = 12;		// Relay K2
const byte REED_IN = 2;			// Reed sensor input (Pull-UP)
const byte UsedPin[] = { REED_IN , HORN_ON, ALARM_ON, TEST };


// Webservices
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncEventSource events("/events");

// Define UDP instance for NTP Client and instance for the Webserver, websocket and eventsource
WiFiUDP   ntpUDP;
NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET);

// Handle ESP32 preferences in local memory
Preferences preferences;

// Instantiate a Speck block ciphering and SHA256
Speck myCipher;
byte encryptKey[N_BLOCK] = { 0x5C, 0x39, 0x38, 0x7B, 0x36, 0x60, 0x68, 0x23, 
							 0x34, 0x34, 0x76, 0x5C, 0x34, 0x22, 0x62, 0x55 };
byte plainText[N_BLOCK];
byte cipherText[N_BLOCK];
SHA256 sha256;

// Singleton instance of the radio driver
RH_NRF24 nrf24(27, 5);
RHReliableDatagram manager(nrf24, SERVER_ADDRESS);
struct Node { uint16_t id; uint8_t state; uint8_t battery; long lastTS; };
Node nodes[7]; // Master always 0 + n nodes

// Global variables
bool service, calling = false;
uint16_t NewGPIO, OldGPIO = 0;
uint8_t nodeNumber, actualNode, nodesOK, oldStatus = 0;
unsigned long start_seconds, stop_seconds, actual_time_sec, epochTime = 0;
unsigned long checkTime, pauseTime, pauseSeconds = 0;
unsigned long updateTime, waitTime, hornTime = millis();
String RxData, servMsg = "";

int hours(unsigned long epochTime) { return ((epochTime % 86400L) / 3600); }
int minutes(unsigned long epochTime) { return ((epochTime % 3600) / 60); }
int seconds(unsigned long epochTime) { return (epochTime % 60); }

// Interrupt service routine for REED_IN input
void handleInterrupt() {
	if ((systemStatus >= 10) && (systemStatus < 20)) {
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
	attachInterrupt(digitalPinToInterrupt(REED_IN), handleInterrupt, FALLING);

	// Start Serial for debug
	Serial.begin(115200);
	Serial.println();
	// Start Serial for A6 GSM module
	//A6GSM.begin(115200);


	// Load admin credits. If not present (first time?) use default admin/admin
	preferences.begin("SmartAlarm", false);
	adminPIN = preferences.getString("adminPIN", adminPIN);
	adminPswd = preferences.getString("adminPswd", adminPswd);	
	systemStatus = static_cast<States>(preferences.getUInt("systemStatus", systemStatus));
	preferences.end();

	// Init nRF24 Manager 
	if (!manager.init())
		Serial.println(F("nRF24 init failed"));
	else
		Serial.println(F("nRF24 init OK"));
	manager.setRetries(15);
	manager.setTimeout(30);

	// Change nRF24 default DataRate and Transmit power
	if (!nrf24.setRF(RH_NRF24::DataRate250kbps, RH_NRF24::TransmitPower0dBm))
		Serial.println(F("nRF24 setRF failed"));


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

	WiFi.mode(WIFI_STA);	
	WiFi.begin(ssid, password);
	if (WiFi.waitForConnectResult() != WL_CONNECTED) {
		Serial.printf("STA: Failed!\n");
		WiFi.disconnect(false);
		WiFi.mode(WIFI_AP);
		WiFi.softAP(hostName);
		delay(1000);
		WiFi.begin(ssid, password);
	}
	else {
		Serial.println("WiFi connected. IP address: ");
		Serial.println(WiFi.localIP());
	}

	// Configure and start Webserver
	ws.onEvent(onWsEvent);
	server.addHandler(&ws);
	events.onConnect([](AsyncEventSourceClient *client) { client->send("Hello Client!", NULL, millis(), 1000); });
	server.addHandler(&events);
	server.addHandler(new SPIFFSEditor(SPIFFS, "admin", adminPswd.c_str()));
	server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");
	server.serveStatic("/auth/", SPIFFS, "/auth/").setDefaultFile("settings.htm").setAuthentication("admin", adminPswd.c_str());
	server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) { request->send(200, "text/plain", String(ESP.getFreeHeap())); });
	server.onNotFound([](AsyncWebServerRequest *request) { request->send(404); });	
	server.begin();
	
	epochTime = timeClient.getEpochTime();
	actual_time_sec = hours(epochTime) * 3600 + minutes(epochTime) * 60 + seconds(epochTime);

	// Set all node to known state
	for (byte i = 0; i<7; i++) {
		nodes[i].id = 0;
		nodes[i].state = 0;
		nodes[i].lastTS = actual_time_sec;
	}
}


void loop() {
	getRadioData();
	/*
	if(systemStatus != oldStatus){
	Serial.print(" ."); Serial.print(systemStatus);
	oldStatus = systemStatus;
	delay(200);
	}
	*/

	switch (systemStatus) {
	// Dummy replay (in order to put nodes in deep sleep);
	case SYS_DISABLED:
		checkAlive();
		digitalWrite(HORN_ON, HIGH);
		digitalWrite(ALARM_ON, HIGH);	
		break;

	// Run alarm system
	case SYS_ENABLED:
		// Update timestamp before re-enable the system
		digitalWrite(HORN_ON, HIGH);
		digitalWrite(ALARM_ON, HIGH);
		for (byte i = 0; i<7; i++)
			nodes[i].lastTS = millis();
		systemStatus = RUNNING;		
		break;

	// Wait for node alive messages and check nodes
	case RUNNING:
		epochTime = timeClient.getEpochTime();
		actual_time_sec = hours(epochTime) * 3600 + minutes(epochTime) * 60 + seconds(epochTime);
		if (actual_time_sec == stop_seconds) {
			digitalWrite(HORN_ON, HIGH);
			digitalWrite(ALARM_ON, HIGH);
			systemStatus = TIMED;
			break;
		}
		// Send random message to the alive node
		if (checkAlive()) {
			systemStatus = WAIT_NODE;
			waitTime = millis();
		}
		else
			systemStatus = CHECK_NODES;
		break;

	// Get the timestamped reply message from the specific node
	case WAIT_NODE:
		// Timeoout -> something get wrong
		if (millis() - waitTime > 700) {
			waitTime = millis();
			systemStatus = CHECK_NODES;
		}
		if (RxData.substring(0, 1) == "9") {
			uint16_t actualId = RxData.substring(1, 2).toInt();
			uint8_t state = RxData.substring(2, 3).toInt();
			uint8_t battery = RxData.substring(3, 5).toInt();
			long timestamp = RxData.substring(5).toInt();
			nodes[actualId].id = actualId;
			nodes[actualId].state = state;
			nodes[actualId].battery = battery;
			nodes[actualId].lastTS = timestamp;
			RxData = "";			
			systemStatus = CHECK_NODES;
		}
		break;

	// Check if all nodes has sent message in last TIMECHECK seconds
	case CHECK_NODES:
		systemStatus = RUNNING;
		nodesOK = 0;
		epochTime = timeClient.getEpochTime();
		actual_time_sec = hours(epochTime) * 3600 + minutes(epochTime) * 60 + seconds(epochTime);
		for (byte i = 0; i<7; i++) {			
			long elapsedtime = actual_time_sec - nodes[i].lastTS;
			if (elapsedtime > TIMECHECK) {
				// if sensor is disabled dont't set alarm
				if (nodes[i].state != 0) {
					Serial.printf("\nSensor %u not respond since %u seconds\n", nodes[i].id, elapsedtime);
					systemStatus = ALARMED;
					delay(1000);
				}
			}
			else
				nodesOK++;
		}
		// Something wrong -> set Alarm state
		if (nodesOK < nodeNumber) {
			systemStatus = ALARMED;
			digitalWrite(ALARM_ON, LOW);
			Serial.printf("\nALARM! Sensors active %u/%u\n", nodesOK, nodeNumber);
			delay(200);
			hornTime = millis();
		}
		break;

	// Only to store the actual status and use it after if restart
	case TIMED:
		//timeClient.update();		
		if (actual_time_sec == start_seconds) {
			// Update nodes timestamp before re-enable the system
			for (byte i = 0; i<7; i++)
				nodes[i].lastTS = millis();
			Serial.println("System ENABLED");
			systemStatus = SYS_ENABLED;
			Serial.printf("\nTime: %u seconds\nStart: %u seconds\nStop: %u seconds\n", actual_time_sec, start_seconds, stop_seconds);
		}
		checkAlive();
		break;

	// wait a defined time and after re-enable the system
	case PAUSED:
		if (millis() - pauseTime > pauseSeconds * 1000) {
			pauseTime = millis();
			// Update timestamp before re-enable the system
			for (byte i = 0; i<7; i++)
				nodes[i].lastTS = pauseTime;
			systemStatus = SYS_ENABLED;
		}
		checkAlive();
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
		checkAlive();
		break;
	}

	// Update client time
	if (millis() - checkTime > 1000) {
		timeClient.update();
		checkTime = millis();
		char str[2];
		sprintf(str, "%d", systemStatus);
		sendDataWs((char *)"status", str);
		epochTime = timeClient.getEpochTime();
		actual_time_sec = hours(epochTime) * 3600 + minutes(epochTime) * 60 + seconds(epochTime);
	}

	// Read status of all used pins and store in NewGPIO var
	NewGPIO = 0;
	for (byte i = 0; i < 16; i++)
		for (byte j = 0; j < sizeof(UsedPin); j++)
			if (UsedPin[j] == i)
				if (digitalRead(i) == HIGH)
					SET_BIT(NewGPIO, i);

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

// Send back message to ALIVE nodes 
bool checkAlive(void) {
	if (RxData.indexOf("ALIVE") > 0) {
		uint8_t fromNode = RxData.substring(0, 2).toInt();
		if (!service)
			sprintf((char *)plainText, "%02u000%010u", fromNode, actual_time_sec);
		else {
			service = false;
			sprintf((char *)plainText, "%s", servMsg.c_str());
		}
		Serial.printf("Master: %s", (char *)plainText);
		delay(5);		
		sendRadioData(fromNode);
		RxData = "";
		return true;
	}
	else
		return false;
}

// Send message to one of nodes
void sendRadioData(uint8_t toNode) {
	myCipher.encryptBlock(cipherText, plainText);
	if (manager.sendtoWait(cipherText, sizeof(cipherText), toNode))
		Serial.print(F(" OK."));
	else
		Serial.print(F(" Failed"));
}

// Wait for a message addressed to Master from one of nodes
void getRadioData(void) {
	if (manager.available()) {
		uint8_t len = sizeof(cipherText);
		uint8_t fromNode;
		if (manager.recvfromAck(cipherText, &len, &fromNode)) {
			myCipher.decryptBlock(plainText, cipherText);
			RxData = String((char *)plainText);			
			Serial.printf("\nNode %u: %s; ", fromNode, RxData.c_str());
		}
	}
}



// ***************************************************************************************************** //
// ***************************************    WebSocket    ********************************************* //
// ***************************************************************************************************** //

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
	if (type == WS_EVT_CONNECT) {
		Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
		client->printf("{\"ts\":%u,\"cmd\":\"status\",\"msg\":\"Client id %u connected\"}", timeClient.getEpochTime(), client->id());
		client->ping();
	}
	else if (type == WS_EVT_DISCONNECT) {
		Serial.printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
	}
	else if (type == WS_EVT_ERROR) {
		Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
	}
	else if (type == WS_EVT_PONG) {
		Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char*)data : "");
		client->printf("{\"ts\":%u,\"cmd\":\"jsVars\",\"timePause\":\"%u\",\"timeHorn\":\"%u\"}", timeClient.getEpochTime(), pauseSeconds, 15);
	}
	else if (type == WS_EVT_DATA) {
		AwsFrameInfo * info = (AwsFrameInfo*)arg;
		String msg = "";
		if (info->final && info->index == 0 && info->len == len) {			
			Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT) ? "text" : "binary", info->len);			
			for (size_t i = 0; i < info->len; i++) {
				msg += (char)data[i];
			}						
			Serial.printf("%s\n", msg.c_str());
			// A message from browser, take action according to our purpose
			processWsMsg(msg);
		}		
	}

}


// Encodes JSON Object and Sends it to All WebSocket Clients
void sendDataWs(char* msgType, char* msg) {
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();

	JsonArray& nodeIds = root.createNestedArray("nodeid");
	JsonArray& stat = root.createNestedArray("state_battery");
	root["ts"] = timeClient.getEpochTime();
	root["cmd"] = msgType;
	root["msg"] = msg;
	root["gpio"] = NewGPIO;
	for (byte i = 0; i<7; i++) {
		int _stat = nodes[i].state * 100 + nodes[i].battery;
		nodeIds.add(nodes[i].id);
		stat.add(_stat);
	}

	size_t len = root.measureLength();
	AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
	if (buffer) {
		root.printTo((char *)buffer->get(), len + 1);
		ws.textAll(buffer);
	}
}

// Web Browser sends some commands, check which command is given
void processWsMsg(String msg) {
	// We got a JSON object from browser, parse it
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.parseObject(msg);
	if (!root.success()) {
		Serial.println("Error parsing JSON message");
		return;
	}
	
	const char* command = root["command"];
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
		}
		else {
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
		String  _node = root["sensor"];
		if (nodes[_node.toInt()].state == 0)
			servMsg = "0" + _node + "__NODE_ENABLE";
		else
			servMsg = "0" + _node + "_NODE_DISABLE";
		Serial.println(servMsg);
		service = true;
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
			AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
			if (buffer) {
				configFile.readBytes((char *)buffer->get(), len + 1);
				ws.textAll(buffer);
			}
			configFile.close();
		}
	}

	// Save new status in case of reboot
	Serial.print(F("\nAlarm system: ")); Serial.println(systemStatus);
	preferences.begin("SmartAlarm", false);
	preferences.putUInt("systemStatus", systemStatus);
	preferences.end();
}


// Check if provided hash is correct (used for PIN)
bool hashThis(String Text, String testHash) {
#define HASH_SIZE 32
#define PIN_SIZE  5
	byte hashBuf[HASH_SIZE];
	char myHash[HASH_SIZE + 1];
	Serial.println("Test HASH");

	sha256.reset();
	sha256.update(Text.c_str(), PIN_SIZE);
	sha256.finalize(hashBuf, HASH_SIZE);

	for (int i = 0; i<HASH_SIZE; i++)
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
	JsonObject& json = jsonBuffer.parseObject(buf.get());
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
		const char *  strIpAddress = json["ipAddress"];   // IP Address from config file
		const char * strGateway = json["gateway"];        // Set gateway to match your network (router)
		const char * strSubnetmask = json["subnetmask"];  // Set subnet mask to match your network
		ip.fromString(strIpAddress);
		gateway.fromString(strGateway);
		subnetmask.fromString(strSubnetmask);
		if (!WiFi.config(ip, gateway, subnetmask))
			Serial.println("Warning: Failed to manual setup wifi connection. I'm going to use dhcp");
	}
	
}
