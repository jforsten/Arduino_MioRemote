#include "FS.h"
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ESP8266WiFiMulti.h>
#include <WebSocketsServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <Hash.h>

#define APPLEMIDI_DEBUG 1
#include "AppleMidi.h"

// Globals

ESP8266WiFiMulti WiFiMulti;
ESP8266WiFiAPClass WifiAP;

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WebSocketsServer webSocket = WebSocketsServer(81);

// -- Config ---------------------------------------------------------------------------
// -------------------------------------------------------------------------------------

#include "WifiConfig.h" // Used for Wifi secrets SSID / Password

IPAddress MIO_IP(192, 168, 0, 85);
int MIO_PORT = 5004;

// Keep-a-live seconds
int KEEP_A_LIVE = 5;

const char *host = "MioController";
const char *update_path = "/firmware";
const char *update_username = "admin";
const char *update_password = "admin";

// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------

// Web socket MessageTypes
//------------------------
//  <- Received
#define SET_PORTS 0
#define GET_PORTS 1
// Sent ->
#define CONNECTED 100
#define PORT_MAP  101
//------------------------

unsigned long t0 = millis();
bool isConnected = false;
bool isConnecting = false;

int pendingSourcePort = -1;
int pendingDestinationPort = -1;
bool pendingPortEnable = false;

#define MIO_PORT_COUNT 56
#define MIO_PORTMAP_TABLE_SIZE 37

byte portMapTable[MIO_PORT_COUNT][MIO_PORTMAP_TABLE_SIZE]; 

APPLEMIDI_CREATE_INSTANCE(WiFiUDP, AppleMIDI); // see definition in AppleMidi_Defs.h

bool waitingOTA = false;
String indexHtml;

// -- Setup -----------------------------------------------------------------------------

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);

  // Clear port table
  for(int i=0; i < MIO_PORT_COUNT; i++) {
    for(int j=0; j < MIO_PORTMAP_TABLE_SIZE; j++) {
      portMapTable[i][j] = 0;
    }
  }
  analogWrite(LED_BUILTIN, 0);

  // Serial communications and wait for port to open:
  Serial.begin(115200);

  // Flash filesystem
  SPIFFS.begin();

  /*
  File root = SPIFFS.open("/","r");
  File file = root.openNextFile();
 
  while(file){ 
      Serial.print("FILE: ");
      Serial.println(file.name());
      file = root.openNextFile();
  }
  file.close();
  root.close();
  */
  
  // Disable AP
  Serial.println("Disabling Wifi AccessPoint for now...");
  WifiAP.softAPdisconnect(true);

  Serial.println("Connecting Wifi...");
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);

  while (WiFiMulti.run() != WL_CONNECTED)
  {
    delay(100);
  }

  Serial.println("CONNECTED!");
  Serial.print("IP address is ");
  Serial.println(WiFi.localIP());

  analogWrite(LED_BUILTIN, 700);

  // start webSocket server
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // handle index
  server.on("/", handleRoot);
  server.on("/ota", setupOTA);
  server.onNotFound(handleWebRequests);
  server.begin();

  // Updater
  httpUpdater.setup(&server);
  httpUpdater.setup(&server, update_path, update_username, update_password);

  // RTP MIDI - Create a session
  Serial.println("Start RTP-MIDI Session...");
  Serial.print(WiFi.localIP());
  Serial.println(":5004");

  AppleMIDI.begin("ESP8266_MidiController");
 
  AppleMIDI.OnConnected(OnAppleMidiConnected);
  AppleMIDI.OnDisconnected(OnAppleMidiDisconnected);
  AppleMIDI.OnReceiveControlChange(OnControlChange);
  AppleMIDI.OnReceiveSysEx(OnSysEx);

  Serial.println("Sending active sensing every 5 seconds!");

  AppleMIDI.invite(MIO_IP, MIO_PORT);

  analogWrite(LED_BUILTIN, 900);

  /*
  File f = SPIFFS.open("/bank0.txt", "r");
  
  for (int i = 1; i <= 10; i++)
  {
    String s = f.readStringUntil('\n');
    Serial.print(i);
    Serial.print(":");
    Serial.println(s);
  }
  f.close();
  */

  File f = SPIFFS.open("/index.html", "r");
  indexHtml = f.readString();
  f.close();
}

// -- Loop ---------------------------------------------------------------------------

int warningLedCount = 0;

unsigned long last_10sec = 0;
unsigned int counter = 0;

void loop()
{

  if (waitingOTA)
  {
    ArduinoOTA.handle();
    Serial.println("Waiting OTA..");
    return;
  }

  unsigned long t = millis();

  AppleMIDI.read();
  webSocket.loop();
  server.handleClient();

  if ((t - last_10sec) > 10 * 1000)
  {
    counter++;
    bool ping = (counter % 2);
    int i = webSocket.connectedClients(ping);
    //Serial.printf("%d Connected websocket clients ping: %d\n", i, ping);
    last_10sec = millis();
  }

  // (dont call delay as it will stall the pipeline)
  if (isConnected && (millis() - t0) > KEEP_A_LIVE * 1000)
  {
    t0 = millis();
    analogWrite(LED_BUILTIN, 1010);
    AppleMIDI.activeSensing();
    //Serial.println("ActiveSensing...");
  }

  if (isConnected && (millis() - t0) > 50)
  {
    analogWrite(LED_BUILTIN, 1023);
  }

  if (!isConnected)
  {
    analogWrite(LED_BUILTIN, (warningLedCount++) % 1024);
    delay(1);

    if (warningLedCount > 10000)
    {
      Serial.println("ReConnect...");
      printws("Reconnecting...");

      AppleMIDI.invite(MIO_IP, MIO_PORT);
      warningLedCount = 0;
    }
  }
}

// ====================================================================================
// Event handler for websocket
// ====================================================================================

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{

  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] Disconnected!\n", num);
    break;
  case WStype_CONNECTED:
  {
    IPAddress ip = webSocket.remoteIP(num);
    Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

    // send message to client
    SendConnectedToWebSocket();
  }
  break;
  case WStype_TEXT:
    Serial.printf("[%u] get Text: %s\n", num, payload);

    if (payload[0] != '{')
    {
      Serial.println("NO JSON PAYLOAD!");
      return;
    }

    DynamicJsonDocument obj(1024);
    deserializeJson(obj, payload);

    int message_type = obj["message_type"];

    switch (message_type)
    {
    // SetPortMap
    case SET_PORTS:
      {
        const byte sourcePort = obj["source"];
        const byte destinationPort = obj["destination"];
        const byte enable = obj["enable"];

        analogWrite(LED_BUILTIN, 700);
        
        Serial.printf("SetPendingPortMap: src: %d, dst: %d, enable: %d\n", sourcePort, destinationPort, enable);
        SetPendingPortMap(sourcePort, destinationPort, enable);
      }
      break;
    // GetPortMap  
    case GET_PORTS:
      const byte sourcePort = obj["source"];
      SendGetPorts(sourcePort);
      break;
    }
    break;
  }
}

// WebSocket Helpers
void SendPortStatusToWebSocket(const byte *data, byte size) 
{
  StaticJsonDocument<1024> doc;
  char output[1024];

  doc["message_type"] = PORT_MAP;
  doc["source"] = data[20];

  JsonArray destinations = doc.createNestedArray("destinations");
  for(int i=0; i<MIO_PORT_COUNT;i++) {
    if (GetPortMap(i, data, (byte)size) == 1) destinations.add(i);
  }

  serializeJson(doc, output);

  Serial.printf("JSON:%s\n", output);
  printws(output);
}

void SendConnectedToWebSocket() {
  StaticJsonDocument<256> doc;
  char output[256];

  doc["message_type"] = CONNECTED;
 
  serializeJson(doc, output);

  Serial.printf("JSON:%s\n", output);
  printws(output);
}

void printws(String msg)
{
  webSocket.broadcastTXT(msg);
}

// ====================================================================================
// Event handlers for incoming MIDI messages
// ====================================================================================

void OnAppleMidiConnected(uint32_t ssrc, char *name)
{
  isConnected = true;
  isConnecting = false;
  Serial.print("Connected to session ");
  Serial.println(name);
}

void OnAppleMidiDisconnected(uint32_t ssrc)
{
  isConnected = false;
  isConnecting = false;
  Serial.println("Disconnected");
}

void OnControlChange(byte channel, byte controller, byte value)
{

  Serial.print("Received control change - channel:");
  Serial.print(channel);
  Serial.print(" controller:");
  Serial.print(controller);
  Serial.print(" value:");
  Serial.print(value);
  Serial.println(" ");
}

void OnSysEx(const byte *data, uint16_t size)
{

  Serial.print("Received data:");
  for (int i = 0; i < size; i++)
  {
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println("");

  // GetPortMapRet
  if (size == 37 && data[14] == 0x00 && data[15] == 0x29) {
    memcpy(portMapTable[data[20]], data, size);

    // No pending port map requests, so do not send SetPortMap
    // 
    if (pendingSourcePort == -1 && pendingDestinationPort == -1)
    {
      Serial.printf("Get port map - response -> websocket: sourcePort=%d\n", data[20]);
      SendPortStatusToWebSocket(data,(byte)size);
      analogWrite(LED_BUILTIN, 1023);
      return;
    }

    Serial.printf("state: %d\n", (int)GetPortMap(pendingDestinationPort, data, (byte)size));

    SetPortMap(pendingSourcePort, pendingDestinationPort, data, (byte)size);
  }

  //  ACK for SetPortMap
  if (size == 23 && data[18] == 0x40 && data[19] == 0x29) {
    byte sourcePort = pendingSourcePort;

    pendingSourcePort = -1;
    pendingDestinationPort = -1;

    // No errors
    if (data[20] == 0)
    {
      SendGetPorts(sourcePort);
    }

  }
}

// ====================================================================================
// Port mapping functions
// ====================================================================================

void SetPendingPortMap(byte srcPort, byte destPort, bool enable)
{
  pendingSourcePort = srcPort;
  pendingDestinationPort = destPort;
  pendingPortEnable = enable;
  SendGetPorts(srcPort);
}

void ResetPortMap(byte srcPort)
{
  SendSetPorts(srcPort, nullptr);
}

void SetPortMap(byte srcPort, byte destPort, const byte *data, byte size)
{
  int offset = 21;
  byte *tmpData;

  tmpData = new byte[size];
  memcpy(tmpData, data, size);

  if (destPort >= 0 && size == 37 && data[14] == 0x00 && data[15] == 0x29)
  {

    if (pendingPortEnable)
      tmpData[offset + destPort / 4] = data[offset + destPort / 4] | (1 << (destPort % 4));
    else
      tmpData[offset + destPort / 4] = data[offset + destPort / 4] & ~(1 << (destPort % 4));

    SendSetPorts(srcPort, tmpData + offset);
  }

  delete tmpData;
}

int GetPortMap(byte destPort, const byte *data, byte size) {
  int offset = 21;
  bool enabled;

  enabled = (((data[offset + destPort / 4]) >> (destPort % 4)) & 0x01);

  return(enabled);
}

//////////////////////////////////////////////////////////////////////
// Mio functions
 //////////////////////////////////////////////////////////////////////
void ResetMio() {

  byte data[20] = {
    0xF0, 
    0x00, 0x01, 0x73, 0x7E, // header
    0x00, 0x00, // product ID
    0x00, 0x00, 0x00, 0x00, // serial number
    0x00, 0x00, // transaction ID
    0x40, 0x10, // command flags and ID
    0x00, 0x01, // data length
    0x00, // reset ID 
    0x00, // checksum
    0xF7 // footer
  };
     
  data[19] = Checksum(data + 1, 18);

  AppleMIDI.sysEx(data, sizeof(data));
}

void SendGetPorts(byte port)
{

  byte data[22] = {
      0xF0,
      0x00, 0x01, 0x73, 0x7E,       // header
      0x00, 0x00,                   // product ID (all)
      0x00, 0x00, 0x00, 0x00, 0x00, // serial number (all)
      0x00, 0x00,                   // transaction ID
      0x40, 0x28,                   // command flags and ID
      0x00, 0x02,                   // data length
      0x00, 0x2D,
      0x0A, // checksum
      0xF7};

  data[19] = port;
  data[20] = Checksum(data + 1, 20);

  AppleMIDI.sysEx(data, sizeof(data));

  Serial.printf("MIO: SendGetPorts\n");
}

void SendSetPorts(byte srcPort, byte *mapData)
{

  byte data[37] = {
      0xF0,
      0x00, 0x01, 0x73, 0x7E,                                                             // header
      0x00, 0x00,                                                                         // product ID (all)
      0x00, 0x00, 0x00, 0x00, 0x00,                                                       // serial number (all)
      0x00, 0x00,                                                                         // transaction ID
      0x40, 0x29,                                                                         // command flags and ID
      0x00, 0x11,                                                                         // data length
      0x01,                                                                               // command version
      0x00, 0x2B,                                                                         // source port
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // port map (default)
      0x00,                                                                               // checksum
      0xF7};

  data[20] = srcPort;

  if (mapData != nullptr)
  {
    for (int i = 0; i < 14; i++)
    {
      data[21 + i] = mapData[i];
    }
  }

  data[35] = Checksum(data + 1, 35);

  AppleMIDI.sysEx(data, sizeof(data));

  Serial.printf("MIO: SendSetPorts\n");
}

byte Checksum(byte *data, int len)
{
  int sum = 0;
  for (int i = 4; i < len - 1; i++)
  {
    sum += data[i];
  }
  return (0x80 - (sum % 0x80));
}

///////////////
// Helpers
///////////////

byte *HexStringToByteArray(String hexString, int *arrayLength)
{

  String sParams[100];
  byte *data;

  *arrayLength = StringSplit(hexString, ' ', sParams, 100);

  data = new byte[*arrayLength];

  for (int i = 0; i < *arrayLength; i++)
  {
    data[i] = (byte)hexToDec(sParams[i]);
  }

  return data;
}

int StringSplit(String sInput, char cDelim, String sParams[], int iMaxParams)
{
  int iParamCount = 0;
  int iPosDelim, iPosStart = 0;

  do
  {
    // Searching the delimiter using indexOf()
    iPosDelim = sInput.indexOf(cDelim, iPosStart);
    if (iPosDelim > (iPosStart + 1))
    {
      // Adding a new parameter using substring()
      sParams[iParamCount] = sInput.substring(iPosStart, iPosDelim);
      iParamCount++;
      // Checking the number of parameters
      if (iParamCount >= iMaxParams)
      {
        return (iParamCount);
      }
      iPosStart = iPosDelim + 1;
    }
  } while (iPosDelim >= 0);
  if (iParamCount < iMaxParams)
  {
    // Adding the last parameter as the end of the line
    sParams[iParamCount] = sInput.substring(iPosStart);
    iParamCount++;
  }

  return (iParamCount);
}

byte hexToDec(String hexString)
{
  byte val = 0;
  val = hexCharToByte(hexString.charAt(0)) << 4;
  val += hexCharToByte(hexString.charAt(1));
  return val;
}

byte hexCharToByte(char c)
{
  if (c >= '0' && c <= '9')
  {
    return c - '0';
  }
  else if (c >= 'a' && c <= 'f')
  {
    return c - 'a' + 10;
  }
  else if (c >= 'A' && c <= 'F')
  {
    return c - 'A' + 10;
  }
  else
    return 0;
}

// Web server /////////////////////////////////////////////////////////////////////////////

void handleRoot()
{
  server.send(200, "text/html", indexHtml); 
}

void returnOK()
{
  server.sendHeader("Connection", "close");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "OK\r\n");
}

void returnFail(String msg)
{
  server.sendHeader("Connection", "close");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(500, "text/plain", msg + "\r\n");
}

void handleWebRequests()
{
  if (loadFromSpiffs(server.uri()))
    return;
  String message = "File Not Detected\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " NAME:" + server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  Serial.println(message);
}

bool loadFromSpiffs(String path)
{
  String dataType = "text/plain";
  if (path.endsWith("/"))
    path += "index.html";
  if (path.endsWith(".src"))
    path = path.substring(0, path.lastIndexOf("."));
  else if (path.endsWith(".html"))
    dataType = "text/html";
  else if (path.endsWith(".htm"))
    dataType = "text/html";
  else if (path.endsWith(".css"))
    dataType = "text/css";
  else if (path.endsWith(".js"))
    dataType = "application/javascript";
  else if (path.endsWith(".png"))
    dataType = "image/png";
  else if (path.endsWith(".gif"))
    dataType = "image/gif";
  else if (path.endsWith(".jpg"))
    dataType = "image/jpeg";
  else if (path.endsWith(".ico"))
    dataType = "image/x-icon";
  else if (path.endsWith(".xml"))
    dataType = "text/xml";
  else if (path.endsWith(".pdf"))
    dataType = "application/pdf";
  else if (path.endsWith(".zip"))
    dataType = "application/zip";

  File dataFile = SPIFFS.open(path.c_str(), "r");

  if (server.hasArg("download"))
    dataType = "application/octet-stream";
  if (server.streamFile(dataFile, dataType) != dataFile.size())
  {
  }

  dataFile.close();
  return true;
}

// OTA ///////////////////////////////////////////////////////////////////////////////

void setupOTA()
{

  returnOK();
  server.stop();

  WiFi.mode(WIFI_STA);
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
  });

  ArduinoOTA.onEnd([]() { // do a fancy thing with our board led at end
    waitingOTA = false;
    ESP.restart();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    waitingOTA = false;
    ESP.restart();
  });
  ArduinoOTA.begin();

  waitingOTA = true;
}
