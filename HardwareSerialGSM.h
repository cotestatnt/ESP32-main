#include <Arduino.h>

//#define DEBUG
#ifdef DEBUG
#define log(msg) Serial.print(msg)
#define logln(msg) Serial.println(msg)
#else
#define log(msg)
#define logln(msg)
#endif

#define countof(a) (sizeof(a) / sizeof(a[0]))
#define A6_OK 0
#define A6_NOTOK 1
#define A6_TIMEOUT 2
#define A6_FAILURE 3

#define A6_CMD_TIMEOUT 2000
#define SERIAL1_RXPIN 33 
#define SERIAL1_TXPIN 4
#define BAUDRATE  115200

//Use UART1 for serial communication with A6 GSM module
HardwareSerial Serial1(1);


enum call_direction {
  DIR_OUTGOING = 0,
  DIR_INCOMING = 1
};

enum call_state {
  CALL_ACTIVE = 0,
  CALL_HELD = 1,
  CALL_DIALING = 2,
  CALL_ALERTING = 3,
  CALL_INCOMING = 4,
  CALL_WAITING = 5,
  CALL_RELEASE = 7
};

enum call_mode {
  MODE_VOICE = 0,
  MODE_DATA = 1,
  MODE_FAX = 2,
  MODE_VOICE_THEN_DATA_VMODE = 3,
  MODE_VOICE_AND_DATA_VMODE = 4,
  MODE_VOICE_AND_FAX_VMODE = 5,
  MODE_VOICE_THEN_DATA_DMODE = 6,
  MODE_VOICE_AND_DATA_DMODE = 7,
  MODE_VOICE_AND_FAX_FMODE = 8,
  MODE_UNKNOWN = 9
};

struct SMSmessage {
  String number;
  String date;
  String message;
};

struct callInfo {
  int index;
  call_direction direction;
  call_state state;
  call_mode mode;
  int multiparty;
  String number;
  int type;
};


byte A6begin();
byte blockUntilReady(long baudRate);

void powerCycle(int pin);
void powerOn(int pin);
void powerOff(int pin);

void dial(String number);
void redial();
void answer();
void hangUp();
callInfo checkCallStatus();
int getSignalStrength();

byte sendSMS(String number, String text);
int getUnreadSMSLocs(int* buf, int maxItems);
int getSMSLocs(int* buf, int maxItems);
int getSMSLocsOfType(int* buf, int maxItems, String type);
SMSmessage readSMS(int index);
byte deleteSMS(int index);
byte setSMScharset(String charset);

String read();
byte A6command(const char *command, const char *resp1, const char *resp2, int timeout, int repetitions, String *response, bool log);
byte A6waitFor(const char *resp1, const char *resp2, int timeout, String *response, bool log);



// Block until the module is ready.
byte blockUntilReady(long baudRate) {

  byte response = A6_NOTOK;
  while (A6_OK != response) {
    response = A6begin();
    // This means the modem has failed to initialize and we need to reboot
    // it.
    if (A6_FAILURE == response) {
      return A6_FAILURE;
    }
    delay(1000);
    logln("Waiting for module to be ready...");
  }
  return A6_OK;
}


// Initialize the software serial connection and change the baud rate from the default (autodetected) to the desired speed.
byte A6begin() {

  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RXPIN, SERIAL1_TXPIN);

  // Factory reset.
  A6command("AT&F", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);

  // Echo off.
  A6command("ATE0", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);

  // Set caller ID on.
  A6command("AT+CLIP=1", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);

  // Set SMS to text mode.
  A6command("AT+CMGF=1", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);

  // Turn SMS indicators off.
  A6command("AT+CNMI=1,0", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);

  // Set SMS storage to the GSM modem.
  if (A6_OK != A6command("AT+CPMS=ME,ME,ME", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false))
    // This may sometimes fail, in which case the modem needs to be
    // rebooted.
  {
    return A6_FAILURE;
  }

  // Set SMS character set.
  setSMScharset("UCS2");

  return A6_OK;
}


// Reboot the module by setting the specified pin HIGH, then LOW. The pin should
// be connected to a P-MOSFET, not the A6's POWER pin.
void powerCycle(int pin) {
  logln("Power-cycling module...");
  powerOff(pin);
  delay(2000);
  powerOn(pin);
  // Give the module some time to settle.
  logln("Done, waiting for the module to initialize...");
  delay(20000);
  logln("Done.");

}


// Turn the modem power completely off.
void powerOff(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
}


// Turn the modem power on.
void powerOn(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
}


// Dial a number.
void dial(String number) {
  char buffer[50];

  logln("Dialing number...");

  sprintf(buffer, "ATD%s;", number.c_str());
  A6command(buffer, "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);
}


// Redial the last number.
void redial() {
  logln("Redialing last number...");
  A6command("AT+DLST", "OK", "CONNECT", A6_CMD_TIMEOUT, 2, NULL, false);
}


// Answer a call.
void answer() {
  A6command("ATA", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);
}


// Hang up the phone.
void hangUp() {
  A6command("ATH", "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);
}


// Check whether there is an active call.
callInfo checkCallStatus() {
  char number[50];
  String response = "";
  uint32_t respStart = 0, matched = 0;
  callInfo cinfo = (const struct callInfo) { 0 };

  // Issue the command and wait for the response.
  A6command("AT+CLCC", "OK", "+CLCC", A6_CMD_TIMEOUT, 2, &response, false);

  // Parse the response if it contains a valid +CLCC.
  respStart = response.indexOf("+CLCC");
  if (respStart >= 0) {
    matched = sscanf(response.substring(respStart).c_str(), "+CLCC: %d,%d,%d,%d,%d,\"%s\",%d", &cinfo.index, &cinfo.direction, &cinfo.state, &cinfo.mode, &cinfo.multiparty, number, &cinfo.type);
    cinfo.number = String(number);
  }

  uint8_t comma_index = cinfo.number.indexOf('"');
  if (comma_index != -1) {
    logln("Extra comma found.");
    cinfo.number = cinfo.number.substring(0, comma_index);
  }

  return cinfo;
}


// Get the strength of the GSM signal.
int getSignalStrength() {
  String response = "";
  uint32_t respStart = 0;
  int strength, error = 0;

  // Issue the command and wait for the response.
  A6command("AT+CSQ", "OK", "+CSQ", A6_CMD_TIMEOUT, 2, &response, false);

  respStart = response.indexOf("+CSQ");
  if (respStart < 0) {
    return 0;
  }

  sscanf(response.substring(respStart).c_str(), "+CSQ: %d,%d", &strength, &error);

  // Bring value range 0..31 to 0..100%, don't mind rounding..
  strength = (strength * 100) / 31;
  return strength;
}


// Send an SMS.
byte sendSMS(String number, String text) {
  char ctrlZ[2] = { 0x1a, 0x00 };
  char buffer[100];

  if (text.length() > 159) {
    // We can't send messages longer than 160 characters.
    return A6_NOTOK;
  }

  log("Sending SMS to ");
  log(number);
  logln("...");

  sprintf(buffer, "AT+CMGS=\"%s\"", number.c_str());
  A6command(buffer, ">", "yy", A6_CMD_TIMEOUT, 2, NULL, false);
  delay(100);
  Serial1.println(text.c_str());
  Serial1.println(ctrlZ);
  Serial1.println();
  return A6_OK;
}


// Retrieve the number and locations of unread SMS messages.
int getUnreadSMSLocs(int* buf, int maxItems) {
  return getSMSLocsOfType(buf, maxItems, "REC UNREAD");
}

// Retrieve the number and locations of all SMS messages.
int getSMSLocs(int* buf, int maxItems) {
  return getSMSLocsOfType(buf, maxItems, "ALL");
}

// Retrieve the number and locations of all SMS messages.
int getSMSLocsOfType(int* buf, int maxItems, String type) {
  String seqStart = "+CMGL: ";
  String response = "";

  String command = "AT+CMGL=\"";
  command += type;
  command += "\"";

  // Issue the command and wait for the response.
  byte status = A6command(command.c_str(), "\xff\r\nOK\r\n", "\r\nOK\r\n", A6_CMD_TIMEOUT, 2, &response, false);

  int seqStartLen = seqStart.length();
  int responseLen = response.length();
  int index, occurrences = 0;

  // Start looking for the +CMGL string.
  for (int i = 0; i < (responseLen - seqStartLen); i++) {
    // If we found a response and it's less than occurrences, add it.
    if (response.substring(i, i + seqStartLen) == seqStart && occurrences < maxItems) {
      // Parse the position out of the reply.
      sscanf(response.substring(i, i + 12).c_str(), "+CMGL: %u,%*s", &index);

      buf[occurrences] = index;
      occurrences++;
    }
  }
  return occurrences;
}

// Return the SMS at index.
SMSmessage readSMS(int index) {
  String response = "";
  char buffer[30];

  // Issue the command and wait for the response.
  sprintf(buffer, "AT+CMGR=%d", index);
  A6command(buffer, "\xff\r\nOK\r\n", "\r\nOK\r\n", A6_CMD_TIMEOUT, 2, &response, false);

  char number[50];
  char date[50];
  char type[10];
  int respStart = 0; // matched = 0;
  SMSmessage sms = (const struct SMSmessage) { "", "", "" };

  // Parse the response if it contains a valid +CLCC.
  respStart = response.indexOf("+CMGR");
  if (respStart >= 0) {
    // Parse the message header.
    sscanf(response.substring(respStart).c_str(), "+CMGR: \"REC %s\",\"%s\",,\"%s\"\r\n", type, number, date);
    sms.number = String(number);
    sms.date = String(date);
    // The rest is the message, extract it.
    sms.message = response.substring(strlen(type) + strlen(number) + strlen(date) + 24, response.length() - 8);
  }
  return sms;
}

// Delete the SMS at index.
byte deleteSMS(int index) {
  char buffer[20];
  sprintf(buffer, "AT+CMGD=%d", index);
  return A6command(buffer, "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);
}


// Set the SMS charset.
byte setSMScharset(String charset) {
  char buffer[30];
  sprintf(buffer, "AT+CSCS=\"%s\"", charset.c_str());
  return A6command(buffer, "OK", "yy", A6_CMD_TIMEOUT, 2, NULL, false);
}



// Read some data from the A6 in a non-blocking manner.
String read() {
  String reply = "";
  if (Serial1.available()) {
    reply = Serial1.readString();
  }

  // XXX: Replace NULs with \xff so we can match on them.
  for (int x = 0; x < reply.length(); x++) {
    if (reply.charAt(x) == 0) {
      reply.setCharAt(x, 255);
    }
  }
  return reply;
}


// Issue a command.
byte A6command(const char *command, const char *resp1, const char *resp2, int timeout, int repetitions, String *response, bool log) {
  byte returnValue = A6_NOTOK;
  byte count = 0;

  while (count < repetitions && returnValue != A6_OK) {
    log("Issuing command: ");
    logln(command);
	// Force serial log (form command sent manually)
	if (log) {
		Serial.print("Command: ");
		Serial.println(command);
	}

    Serial1.write(command);
    Serial1.write('\r');

    if (A6waitFor(resp1, resp2, timeout, response, log) == A6_OK) {
      returnValue = A6_OK;
    }
    else {
      returnValue = A6_NOTOK;
    }
    count++;
  }
  return returnValue;
}


// Wait for responses.
byte A6waitFor(const char *resp1, const char *resp2, int timeout, String *response, bool log) {
  unsigned long entry = millis();  
  String reply = "";
  byte retVal = 99;
  do {
    reply += read();
    yield();
  } while (((reply.indexOf(resp1) + reply.indexOf(resp2)) == -2) && ((millis() - entry) < timeout));

  if (reply != "") {
    log("Reply in ");
    log(millis() - entry);
    log(" ms: ");
    logln(reply);
	if (log) {
		Serial.print("Reply in ");
		Serial.print(millis() - entry);
		Serial.print(" ms: ");
		Serial.println(reply);
	}
  }

  if (response != NULL) {
    *response = reply;
  }

  if ((millis() - entry) >= timeout) {
    retVal = A6_TIMEOUT;
    logln("Timed out.");
  }
  else {
    if (reply.indexOf(resp1) + reply.indexOf(resp2) > -2) {
      logln("Reply OK.");
      retVal = A6_OK;
    }
    else {
      logln("Reply NOT OK.");
      retVal = A6_NOTOK;
    }
  }
  return retVal;
}
