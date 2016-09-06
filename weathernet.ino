/*
 * Sketch to run on Adafruit Huzzah (ESP 8266) to pull pin high if substantial rain forecast.
 * Low-power sleep functionality built in.
 * 
 * Borrows code from @thisoldgeek (https://github.com/thisoldgeek/ESP8266-Weather-Display/blob/master/ESP8266_Weather_Feather_Huzzah.ino)
 * for WiFi connection and precipitation data acquisiton and Michael Margolis and Tom Igoe (Udp NTP Client) for reading Internet time.
*/

// include ESP8266, JSON parsing, serial, and Internet data logging libraries
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <SoftwareSerial.h>
#include <ThingSpeak.h>

// this is for the RTC memory read/write functions
extern "C" {
#include "user_interface.h"
}

// time of day (seconds, UTC) to be stored in RTC memory
typedef struct {
  uint32_t magicNumber;
  uint32_t tod;
}
rtcStore;
rtcStore todValue;

#define SSID ""                                                 // insert your SSID
#define PASS ""                                                 // insert your password

#define LOCATIONID ""                                           // location id


// define hosts
const char* HOST[] = {
  "api.wunderground.com",
  "api.thingspeak.com",
  "time.nist.gov"
};

// API license keys (Wunderground [read], Thingspeak [write])
const char* YOUR_KEY[] = {
  "",
  ""
};

const uint32_t TS_CHANNEL = ;                                   // ThingSpeak channel to which we wish to post our beloved data

const int GPIO_PIN = 4; 
const int BUFFER_SIZE = 300;                                    // length of json buffer

const uint32_t ONE_HOUR = 3600000000;

const int READ_TOD = ((5 + 4) * 3600) % 86400;                  // time of day (in seconds UTC) before which weather read should occur
                                                                // 5 AM EDT (-0400 UTC) * 3600 s/hr
                                                    
const int RAIN_THRESHOLD = 10;                                  // amount of rain (in mm) above which we'll consider it to rain
ADC_MODE(ADC_VCC);
const float ADJ_VCC = 0.95;

int rain; // rain variable (0 = no rain, 1 = rain)

/* 
 * Array of desired weather conditions. 
 * These must be in the order received from wunderground!
 * Also, watch out for repeating field names in returned JSON structures 
 * and fields with embedded commas (used as delimiters).
*/

// weather conditions to keep track of
char* conds[] = {
  "\"fahrenheit\":",
  "\"mm\":"
};

const int NUM_ELEMENTS = 2;                                     // number of conditions you are retrieving, count of elements in conds

void setup() {
  Serial.begin(115200);                                         // baudrate of monitor
  system_rtc_mem_read(64, &todValue, 8);
  
  // if it's time to check the time (nominally once a day), check the Internet time 
  if(todValue.magicNumber != 1337) {
    beginInternet();
    Serial.print("Setting time to: ");
    todValue.tod = getTime() % 86400;
    Serial.print(todValue.tod);
    todValue.magicNumber = 1337;
    system_rtc_mem_write(64, &todValue, 8);
  }
    
  pinMode(GPIO_PIN, OUTPUT);

  // read time of day from RTC memory and calculate time remaining until time is checked against the Internet
  system_rtc_mem_read(64, &todValue, 8);
  Serial.print("Time of day: ");
  Serial.println(todValue.tod);
  int timeToRead;
  timeToRead = READ_TOD - todValue.tod;
  Serial.print("Time to read: ");
  Serial.println(timeToRead);
  
  // if it's close to the time of day to read the time, read the time
  if (timeToRead < (ONE_HOUR / 1000000) && (timeToRead > 0)) {
      
    // however, if the time to read (the Internet time) is not quite close enough
    // (more than half the maximum sleep time of the ESP8266), take a quick nap
    if(timeToRead > (ONE_HOUR / 2000000)) {
      uint32_t diff = timeToRead - ONE_HOUR / 2000000;
      todValue.tod += 60 + diff;
      system_rtc_mem_write(64, &todValue, 8);
      ESP.deepSleep((60 + diff) * 1000000, WAKE_RFCAL);         // duration of nap time
    }
    beginInternet();
    wunderground();                                             // get new data
    
    // read the voltage at the chip (to determine battery state)
    Serial.print("Vcc: ");
    Serial.println(ESP.getVcc());
    float Vcc = ESP.getVcc() * ADJ_VCC / 1000;
    float post[NUM_ELEMENTS] = {rain, Vcc};
    postThingspeak(post);                                       // post the expected rain state and battery state to Internet
    
    // go to sleep, sweet microcontroller
    Serial.print("Going to sleep for this many seconds: ");
    uint32_t nap = (ONE_HOUR / 2 + timeToRead * 1000000);
    Serial.println(nap);
    todValue.tod += nap;
    system_rtc_mem_write(64, &todValue, 8);
    ESP.deepSleep(nap, WAKE_RF_DISABLED);
  }

  // if we wake up nowhere near the time of day to take our measurements and connect to the Internet,
  // go back to sleep for the maximum possible time allowable by the ESP8266 microcontroller
  else {
    todValue.tod += ONE_HOUR / 1000000;
    system_rtc_mem_write(64, &todValue, 8);
    if (timeToRead < (ONE_HOUR / 1000000 * 1.5))
      ESP.deepSleep(ONE_HOUR, WAKE_RFCAL);                      // go to sleep, sweet microcontroller
    else
      ESP.deepSleep(ONE_HOUR, WAKE_RF_DISABLED);                // go to sleep, sweet microcontroller (and wake with WiFi off)
  }
}

// nothing to loop, since we always restart when we go to sleep
void loop() {
}

void wunderground() {
  WiFiClient client;
  setupClient(client, 0);
  
  String cmd = "GET /api/";  cmd += YOUR_KEY[0];                // build request_string cmd
  cmd += "/forecast/q/";  cmd += LOCATIONID;  cmd +=".json";
  cmd += " HTTP/1.1\r\nHost: api.wunderground.com\r\n\r\n"; 
  delay(500);
  client.print(cmd);                                            // connect to api.wunderground.com with request_string
  delay(500);
  unsigned int i = 0;                                           // timeout counter
                              
  String json = "{";                                            // first character for json-string is begin-bracket
  
  for (int j = 0; j < NUM_ELEMENTS; j++) {                      // do the loop for every element/condition
    boolean quote = false; int nn = false;                      // if quote=false means no quotes so comma means break
    while (!client.find(conds[j])){}                            // (wait while we) find the part we are interested in
                                                       
    json += conds[j];

    while (i < 5000) {                                          // timer/counter
      if(client.available()) {                                  // if character found in receive-buffer
        char c = client.read();                                 // read that character
                                                  
// ************************ construction of json string converting commas inside quotes to dots ********************        
        if ((c == '"') && (quote == false))                     // there is a " and quote=false, so start of new element
          quote = true;                                         // make quote=true and notice place in string
        if ((c == '{') && (quote == false))
          c = ' ';
        if ((c == ',')&&(quote == true))
          c = '.';                                              // if there is a comma inside quotes, comma becomes a dot.
        if ((c == '"') && (quote = true))                       // if there is a " and quote=true and on different position
          quote = false;                                        // quote=false meaning end of element between ""
        if(((c == ',') || (c == '\n')) && (quote == false))
          break;                                                // if comma delimiter outside "" then end of this element
 //****************************** end of construction ******************************************************
          
        json += c;                                              // fill json string with this character
        i = 0;                                                  // timer/counter + 1
      }
      i++;                                                      // add 1 to timer/counter
    }                                                           // end while i<5000

    if (j == NUM_ELEMENTS-1)                                    // if last element
        {json += '}';}                                          // add end bracket of json string
    else                                                        // else
        {json += ',';}                                          // add comma as element delimiter
  }                                                             // end for j loop

  int params[NUM_ELEMENTS];
  parseJSON(json, params);                                      // extract the conditions

  // if it's going to rain, pull pin high
  if (params[1] >= RAIN_THRESHOLD) {
    digitalWrite(GPIO_PIN, HIGH);
    Serial.print("GPIO pin ");
    Serial.print(GPIO_PIN);
    Serial.println(" pulled high.");
    delay(100);
    digitalWrite(GPIO_PIN, LOW);
    rain = 1;
  }
  else{
    Serial.println("No rain today.");
    rain = 0;
  }
}


void parseJSON(String json, int params[])
{
  StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
  JsonObject& root = jsonBuffer.parseObject(json);
 
  String temp_f = root["fahrenheit"];
  String qpf_allday = root["mm"];

  params[0] = temp_f.toInt();
  params[1] = qpf_allday.toInt();

  Serial.print("high predicted (deg F): ");
  Serial.println(params[0]);
  Serial.print("rain predicted (mm): ");
  Serial.println(params[1]);
}

// serial print and post values to the Internet
void postThingspeak(float value[]){
  WiFiClient client;
  ThingSpeak.begin(client);

  for (int i = 0; i < NUM_ELEMENTS; i++) { 
    Serial.print("Logging field: ");
    Serial.print(i+1);
    Serial.print(" with value ");
    Serial.println(value[i]);
    ThingSpeak.setField((i+1),value[i]);
  }
  ThingSpeak.writeFields(TS_CHANNEL,YOUR_KEY[1]);
}

int getTime() {
  int localPort = 2390;                                           // local port to listen for UDP packets
  const int NTP_PACKET_SIZE = 48;                                 // NTP time stamp is in the first 48 bytes of the message
  byte packetBuffer[NTP_PACKET_SIZE];                             // buffer to hold incoming and outgoing packets
  IPAddress timeServerIP;                                         // time.nist.gov NTP server address

  WiFiUDP udp;                                                    // A UDP instance to let us send and receive packets over UDP

  Serial.println("Starting UDP");
  udp.begin(localPort);
  Serial.print("Local port: ");
  Serial.println(udp.localPort());
  
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;                                   // LI, Version, Mode
  packetBuffer[1] = 0;                                            // Stratum, or type of clock
  packetBuffer[2] = 6;                                            // Polling Interval
  packetBuffer[3] = 0xEC;                                         // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  int i = 0;                                                      // counter variable

  // timeout loop that looks for UDP time packet
  while(i < 100) {

    WiFi.hostByName(HOST[2], timeServerIP);                       // get a random server from the pool

    Serial.println("sending NTP packet...");
  
    // send a packet requesting a timestamp:
    udp.beginPacket(timeServerIP, 123);                           // NTP requests are to port 123
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
  
    delay(1000);                                                  // wait to see if a reply is available
  
    int cb = udp.parsePacket();
    if (!cb)
      Serial.println("no packet yet");
    else {
      Serial.print("packet received, length = ");
      Serial.println(cb);
      
      udp.read(packetBuffer, NTP_PACKET_SIZE);                    // read the received packet into the buffer
    
      // timestamp starts at byte 40 of the received packet and is
      // four bytes (two words) long. First, extract the two words:
    
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
      Serial.print("Seconds since Jan 1 1900 = " );
      Serial.println(secsSince1900);
    
      // now convert NTP time into everyday time:
      Serial.print("Unix time = ");
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
      const unsigned long seventyYears = 2208988800UL;
      // subtract seventy years:
      unsigned long epoch = secsSince1900 - seventyYears;
      uint32_t time_of_day = epoch % 86400;
      // print Unix time:
      Serial.print("UNIX time: ");
      Serial.println(epoch);
      // print (and return) time of day (in secs)
      Serial.print("time of day (s) UTC : ");
      Serial.println(time_of_day);
      return time_of_day;                                       // pass time and exit function
    }
  i++;
  }
}

void setupClient(WiFiClient& client, int host){
  Serial.print("connecting to ");
  Serial.println(HOST[host]);
  
  const int httpPort = 80;
  
  if (!client.connect(HOST[host], httpPort)) {
    Serial.println("connection failed");
    return;
  }
}

void beginInternet() {
  WiFi.begin(SSID,PASS);                                        // your WiFi network's SSID & Password
  while (WiFi.status() != WL_CONNECTED) {                       // do until connected
    delay(500);
    Serial.print(".");                                          // print a few dots
  }
  Serial.println();
  Serial.println("WiFi connected");  
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}
