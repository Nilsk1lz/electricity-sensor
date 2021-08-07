
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

#define DIGITAL_INPUT_SENSOR D1  // The digital input you attached your light sensor.  (Only 2 and 3 generates interrupt!)
#define PULSE_FACTOR 1000       // Number of blinks per kWh of your meter. Normally 1000.
#define MAX_WATT 10000          // Max watt value to report. This filters outliers.
#define CHILD_ID 1              // Id of the sensor child

//My meter flashes 1000 times per kWh
const float w_per_pulse = 1;
const unsigned long ms_per_hour = 3600000UL;

const char* devicename = "main"; // sets MQTT topics and hostname for ArduinoOTA

const char* ssid = "SSID";     //wi-fi netwrok name
const char* password = "PASSWORD";  //wi-fi network password
const char* mqtt_server = "IP_ADDRESS";   // mqtt broker ip address (without port)
const int mqtt_port = 1883;                   // mqtt broker port
const char *mqtt_user = "USERNAME";
const char *mqtt_pass = "PASSWORD";

uint32_t SEND_FREQUENCY =
    20000; // Minimum time between send (in milliseconds). We don't want to spam the gateway.
double ppwh = ((double)PULSE_FACTOR) / 1000; // Pulses per watt hour
volatile uint32_t pulseCount = 0;
volatile uint32_t lastBlinkmicros = 0;
volatile uint32_t lastBlinkmillis = 0;
volatile uint32_t watt = 0;
uint32_t oldPulseCount = 0;
uint32_t oldWatt = 0;
double oldkWh;
uint32_t lastSend;

//*******************************************************************************************************************
// all the code from this point and onwards doesn't have to be touched in order to have everything working (hopefully)

char mqtt_serial_publish_usage_cache[50];

int mqtt_usage = sprintf(mqtt_serial_publish_usage_cache,"%s%s%s","electricity_sensor/", devicename,"/usage");
const PROGMEM char* mqtt_serial_publish_ch = mqtt_serial_publish_usage_cache;

WiFiClient espClient;
PubSubClient client(espClient);

char usageArray[50];


#define IRQ_HANDLER_ATTR ICACHE_RAM_ATTR

void IRQ_HANDLER_ATTR onPulse()
{
  uint32_t newBlinkmicros = micros();
  uint32_t newBlinkmillis = millis();
  uint32_t intervalmicros = newBlinkmicros - lastBlinkmicros;
  uint32_t intervalmillis = newBlinkmillis - lastBlinkmillis;
  if (intervalmicros < 10000L && intervalmillis < 10L) { // Sometimes we get interrupt on RISING
    return;
  }
  if (intervalmillis < 360000) { // Less than an hour since last pulse, use microseconds
    watt = (3600000000.0 / intervalmicros) / ppwh;
  } else {
    watt = (3600000.0 / intervalmillis) /
           ppwh; // more thAn an hour since last pulse, use milliseconds as micros will overflow after 70min
  }
  lastBlinkmicros = newBlinkmicros;
  lastBlinkmillis = newBlinkmillis;

  pulseCount++;
}

void setup()
{
  Serial.begin(115200);
  // Use the internal pullup to be able to hook up this sketch directly to an energy meter with S0 output
  // If no pullup is used, the reported usage will be too high because of the floating pin
  pinMode(DIGITAL_INPUT_SENSOR, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(DIGITAL_INPUT_SENSOR), onPulse, RISING);
  lastSend = millis();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }

  client.setServer(mqtt_server, mqtt_port);
  reconnect();  

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Create a random client ID
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str(),mqtt_user,mqtt_pass)) {
      Serial.println("connected");
//      client.subscribe(mqtt_serial_receiver_ch);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void send(int data){
  data = max(0, data);
  if (!client.connected()) {
    reconnect();
  }
  String stringaUsage = String(data);
  stringaUsage.toCharArray(usageArray, stringaUsage.length() +1);
  client.publish(mqtt_serial_publish_ch, usageArray);
}

void loop()
{
  ArduinoOTA.handle();

  if (!client.connected()) 
  {
    reconnect();
  }

  client.loop();
   
  uint32_t now = millis();
  // Only send values at a maximum frequency or woken up from sleep
  bool sendTime = now - lastSend > SEND_FREQUENCY;
  if (sendTime) {
    // New watt value has been calculated
    if (watt != oldWatt) {
      // Check that we don't get unreasonable large watt value, which
      // could happen when long wraps or false interrupt triggered
      if (watt < ((uint32_t)MAX_WATT)) {
        send(watt);  // Send watt value to gw
      }
      Serial.print("Watt:");
      Serial.println(watt);
      oldWatt = watt;
    }

    lastSend = now;
  }
}
