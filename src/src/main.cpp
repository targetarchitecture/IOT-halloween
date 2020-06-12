/*
Sources:
http://tilman.de/projekte/wifi-doorbell/
http://tilman.de/projekte/wifi-doorbell/images/Doorbell-Speaker_electronics.png
https://docs.zerynth.com/latest/_images/doitesp32pin.jpg
https://github.com/pcbreflux/espressif/blob/master/esp32/arduino/sketchbook/ESP32_DFPlayer_full/ESP32_DFPlayer_full.ino
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "credentials.h"
#include "DFRobotDFPlayerMini.h"
#include <PubSubClient.h>
#include <Preferences.h>

//WiFiClient espClient;
PubSubClient MQTTClient;
Preferences settings;
HardwareSerial hwSerial(2);
DFRobotDFPlayerMini dfPlayer;

void callback(char *topic, byte *payload, unsigned int length);
void publishMQTTmessage(String msg);
int MQTT_PLAY_TRACK = 0;

#define DefaultVolume 17

void printDetail(uint8_t type, int value);
void setupWifi();
void setupOTA();
void setupMP3player();
void setupMQTTClient();
void reconnect();

#define PIN_BUSY 22
#define PIN_PIR 18

void setup()
{
  pinMode(PIN_BUSY, INPUT);
  pinMode(PIN_PIR, INPUT);
  btStop(); // turn off bluetooth

  Serial.begin(9600);
  Serial.println("Booting");

  // initialize NVS
  settings.begin("settings", false);

  setupWifi();
  setupOTA();

  setupMQTTClient();

  String SSID = WIFI_SSID;
  String mDNS = MDNS_HOSTNAME;
  String MQTTSVR = MQTT_SERVER;
  publishMQTTmessage("Connected to SSID: " + SSID);
  publishMQTTmessage("IP address: " + WiFi.localIP().toString());
  publishMQTTmessage("mDNS: " + mDNS);
  publishMQTTmessage("Connected to MQTT server: " + MQTTSVR);

  setupMP3player();

  randomSeed(analogRead(0));
}

String STATE = "NOONE";
String AUDIO = "NOTPLAYING";
unsigned long interval = 30000;
unsigned long previousMillis = 0;

void loop()
{
  ArduinoOTA.handle();

  //MQTT section
  if (!MQTTClient.connected())
  {
    reconnect();
  }
  MQTTClient.loop();

  unsigned long currentMillis = millis();
  int sensorValue = digitalRead(PIN_PIR);

  if ((sensorValue == 1) && (currentMillis - previousMillis > interval))
  {
    previousMillis = currentMillis;

    STATE = "PEOPLE";

    publishMQTTmessage("People at the front door!");
  }
  else
  {
    STATE = "NOONE";
  }

  int busy = digitalRead(PIN_BUSY);

  if (busy == 1) //it's not playing any audio
  {
    AUDIO = "NOTPLAYING";
  }
  else
  {
    AUDIO = "PLAYING";
  }

  if (STATE == "PEOPLE" && AUDIO == "NOTPLAYING")
  {
    int nextTrack = random(1, 45);

    dfPlayer.play(nextTrack);

    publishMQTTmessage("Playing track " + String(nextTrack));
  }

  delay(50);
}

void setupWifi()
{
  //sort out WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD); // Connect to the network

  while (WiFi.waitForConnectResult() != WL_CONNECTED)
  {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  Serial.println("Ready on the local network");
  Serial.println("IP address: " + WiFi.localIP().toString());
}

void setupOTA()
{
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();
}

void setupMP3player()
{
  hwSerial.begin(9600, SERIAL_8N1, 19, 5); // speed, type, TX, RX

  Serial.println();
  Serial.println(F("DFRobot DFPlayer Mini"));

  dfPlayer.begin(hwSerial);
  delay(1000);

  int nvsVolume = settings.getInt("volume", DefaultVolume);
  publishMQTTmessage("Volume from NVS: " + String(nvsVolume));

  dfPlayer.volume(nvsVolume); // Set volume value (0~30).
  delay(200);

  Serial.println(F("Play track 1"));
  dfPlayer.play(1); // Play the first mp3
  delay(200);
}

void setupMQTTClient()
{
  Serial.println("Connecting to MQTT server");

  MQTTClient.setClient(espClient);
  MQTTClient.setServer(MQTT_SERVER, 1883);

  // setup callbacks
  MQTTClient.setCallback(callback);

  Serial.println("connect mqtt...");

  String clientId = MQTT_CLIENTID;

  if (MQTTClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_KEY))
  {
    Serial.println("Connected");
    MQTTClient.publish(MQTT_TOPIC, "Connected to MQTT server");

    Serial.println("subscribe");
    MQTTClient.subscribe(MQTT_PLAY_TOPIC);
    MQTTClient.subscribe(MQTT_VOLUME_TOPIC);
    MQTTClient.subscribe(MQTT_STOP_TOPIC);
  }
}

void reconnect()
{
  // Loop until we're reconnected
  while (!MQTTClient.connected())
  {
    yield();

    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = MQTT_CLIENTID;

    // Attempt to connect
    if (MQTTClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_KEY))
    {
      Serial.println("connected");
      // Once connected, publish an announcement...
      MQTTClient.publish(MQTT_TOPIC, "Reconnected");
      // ... and resubscribe
      MQTTClient.subscribe(MQTT_PLAY_TOPIC);
    }
    else
    {
      Serial.print("failed, rc=");
      Serial.print(MQTTClient.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  String message = "";

  for (int i = 0; i < length; i++)
  {
    message += (char)payload[i];
  }

  Serial.println(message);

  if (std::string(topic) == std::string(MQTT_STOP_TOPIC))
  {
    if (message.equalsIgnoreCase("stop") == true)
    {
      dfPlayer.stop(); // Stop the track
      delay(200);
      publishMQTTmessage("Stopped play");
    }
  }

  if (std::string(topic) == std::string(MQTT_PLAY_TOPIC))
  {
    int playTrack = message.toInt();

    dfPlayer.stop(); // Stop the track
    delay(200);

    dfPlayer.play(playTrack); // Play the track specified

    publishMQTTmessage("Playing track " + message);
  }

  if (std::string(topic) == std::string(MQTT_VOLUME_TOPIC))
  {
    int nvsVolume = message.toInt();

    dfPlayer.volume(nvsVolume);
    delay(200);

    publishMQTTmessage("Volume set to " + message);

    settings.putInt("volume", nvsVolume);
    nvsVolume = settings.getInt("volume", DefaultVolume);
    publishMQTTmessage("Volume from NVS: " + String(nvsVolume));
  }
}

void publishMQTTmessage(String msg)
{
  MQTTClient.publish(MQTT_TOPIC, msg.c_str());
}
