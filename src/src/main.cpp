#include <Arduino.h>
#include <WiFi.h>
#include "DFRobotDFPlayerMini.h"
#include <PubSubClient.h>
#include <Preferences.h>
#include "credentials.h" // Extracted credentials layer

// MQTT Topics Configuration
const char* MQTT_PLAY_TOPIC   = "halloween/play";
const char* MQTT_STOP_TOPIC   = "halloween/stop";
const char* MQTT_VOLUME_TOPIC = "halloween/volume";
const char* MQTT_TOPIC        = "halloween";

// Pin Map Assignments
#define PIN_BUSY        22
#define PIN_PIR         18
#define DEFAULT_VOLUME  17
#define PIR_INTERVAL    30000 // Cooldown window in milliseconds

// Class Declarations
WiFiClient espClient;
PubSubClient MQTTClient(espClient);
Preferences settings;
HardwareSerial hwSerial(2); // Utilize ESP32 UART2
DFRobotDFPlayerMini dfPlayer;

// State Monitors
String systemState       = "NOONE";
String audioStatus      = "NOTPLAYING";
unsigned long lastPIRTrigger       = 0;
unsigned long lastReconnectAttempt = 0;

// Function Prototypes
void setupWifi();
void setupMQTTClient();
void setupMP3player();
void handleMQTTReconnection();
void publishMQTTMessage(const String& msg);
void mqttCallback(char* topic, byte* payload, unsigned int length);

void setup() {
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_PIR, INPUT);

  // Fast logging interface to prevent execution bottlenecks
  Serial.begin(115200);
  Serial.println("Booting Halloween Controller Node...");

  // Open NVS namespace
  settings.begin("settings", false);

  setupWifi();
  setupMQTTClient();
  setupMP3player();

  publishMQTTMessage("Connected to SSID: " + String(WIFI_SSID));
  publishMQTTMessage("IP address: " + WiFi.localIP().toString());
  publishMQTTMessage("Connected to MQTT server: " + String(MQTT_SERVER));

  // Initialize PRNG using floating analog white noise
  randomSeed(analogRead(0));
}

void loop() {
  // Asynchronous non-blocking network watchdogs
  if (!MQTTClient.connected()) {
    handleMQTTReconnection();
  } else {
    MQTTClient.loop();
  }

  unsigned long currentMillis = millis();
  int pirState = digitalRead(PIN_PIR);
  int isBusy = digitalRead(PIN_BUSY); // DFPlayer low active: 0 = Busy, 1 = Idle

  // Set local state based on hardware pin responses
  if (isBusy == 1) {
    audioStatus = "NOTPLAYING";
  } else {
    audioStatus = "PLAYING";
  }

  // Motion automation runtime loop
  if (pirState == HIGH && (currentMillis - lastPIRTrigger > PIR_INTERVAL)) {
    lastPIRTrigger = currentMillis;
    systemState = "PEOPLE";
    publishMQTTMessage("People detected at front door footprint.");

    if (audioStatus == "NOTPLAYING") {
      int nextTrack = random(1, 45); // Generates track selections from index 1-44
      dfPlayer.stop();
      delay(200);
      dfPlayer.play(nextTrack);
      publishMQTTMessage("Automation playing random track: " + String(nextTrack));
    }
  } else if (pirState == LOW && audioStatus == "NOTPLAYING") {
    systemState = "NOONE";
  }

  delay(10); // Maintain stability and handle FreeRTOS clock background cycles
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Initializing WiFi Link");
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("\nLink Negotiation Failed! System Restarting...");
    delay(5000);
    ESP.restart();
  }
  Serial.println("\nWiFi Link Up. IP: " + WiFi.localIP().toString());
}

void setupMQTTClient() {
  MQTTClient.setServer(MQTT_SERVER, 1883);
  MQTTClient.setCallback(mqttCallback);
}

void handleMQTTReconnection() {
  unsigned long now = millis();
  // Attempt network connection every 5000ms without blocking background logic loops
  if (now - lastReconnectAttempt > 5000) {
    lastReconnectAttempt = now;
    Serial.print("Attempting background MQTT session initialization...");

    if (MQTTClient.connect(MQTT_CLIENTID, MQTT_USERNAME, MQTT_KEY)) {
      Serial.println("Established.");
      MQTTClient.publish(MQTT_TOPIC, "Reconnected successfully.");
      MQTTClient.subscribe(MQTT_PLAY_TOPIC);
      MQTTClient.subscribe(MQTT_VOLUME_TOPIC);
      MQTTClient.subscribe(MQTT_STOP_TOPIC);
    } else {
      Serial.printf("Failed, Code (rc=%d). Retrying context later.\n", MQTTClient.state());
    }
  }
}

void setupMP3player() {
  // Remap hardware UART2 parameters: Baud=9600, Config=8N1, TX=GPIO19, RX=GPIO5
  hwSerial.begin(9600, SERIAL_8N1, 19, 5);
  Serial.println("Spawning DFPlayer Mini connection interface...");

  if (!dfPlayer.begin(hwSerial)) {
    Serial.println("Fatal Configuration Error: DFPlayer Mini initialization failure.");
    return;
  }
  delay(1000);

  int transientVolume = settings.getInt("volume", DEFAULT_VOLUME);
  publishMQTTMessage("Volume configuration retrieved from NVS: " + String(transientVolume));
  dfPlayer.volume(transientVolume);
  delay(200);

  dfPlayer.play(1); // Execute initialization sound bite
  delay(200);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.printf("Inbound Message Event [%s]: %s\n", topic, message.c_str());

  // Memory optimized stack comparison omitting temporary std::string construction overhead
  if (strcmp(topic, MQTT_STOP_TOPIC) == 0) {
    if (message.equalsIgnoreCase("stop")) {
      dfPlayer.stop();
      delay(200);
      publishMQTTMessage("Playback termination forced over MQTT.");
    }
  } 
  else if (strcmp(topic, MQTT_PLAY_TOPIC) == 0) {
    int targetTrack = message.toInt();
    dfPlayer.stop();
    delay(200);
    dfPlayer.play(targetTrack);
    publishMQTTMessage("Playing specified track ID: " + message);
  } 
  else if (strcmp(topic, MQTT_VOLUME_TOPIC) == 0) {
    int requestedVolume = message.toInt();
    requestedVolume = constrain(requestedVolume, 0, 30); // Boundaries mapping protection
    
    dfPlayer.volume(requestedVolume);
    delay(200);
    publishMQTTMessage("Audio output volume set to: " + message);

    // Flash Memory protection layer: verify value delta state change
    int storedVolume = settings.getInt("volume", DEFAULT_VOLUME);
    if (requestedVolume != storedVolume) {
      settings.putInt("volume", requestedVolume);
      publishMQTTMessage("NVS storage registry updated with target value change.");
    }
  }
}

void publishMQTTMessage(const String& msg) {
  if (MQTTClient.connected()) {
    MQTTClient.publish(MQTT_TOPIC, msg.c_str());
  }
}
