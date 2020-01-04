 #include <Arduino.h>
#include <ArduinoJson.h>
#include <TimeLib.h>
#include "WifiConfig.h"
#include "EmonConfig.h"
#include <NtpClientLib.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h> // Библиотека для OTA-прошивки

#ifndef YOUR_WIFI_SSID
#define YOUR_WIFI_SSID "YOUR_WIFI_SSID"
#define YOUR_WIFI_PASSWD "YOUR_WIFI_PASSWD"
#endif // !YOUR_WIFI_SSID

#define OTA_HOSNAME "living_room_r" 

// #ifndef EMON_CONFIG_H
// #define EMON_APIKEY "XXXXXXXXXXXXX"
// #endif // !EMON_CONFIG_H

String EMON_SEND_NODE_ID = OTA_HOSNAME;

#define EMON_GET_NODE_ID "65" //living_room_t
// #define EMON_SEND_NODE_ID ""
#define EMON_DOMAIN "udom.ua"
#define EMON_PATH "emoncms"
#define EMON_DATA_CHECK_PERIOD_MAX 300 //sec
#define EMON_GET_DATA_TIMEOUT 2000 //ms
#define EMON_UPLOAD_PERIOD_MAX 300 //sec

#define ONBOARDLED 2 // Built in LED on ESP-12/ESP-07

#define FIRST_RELAY D1
#define SECOND_RELAY D2
#define THIRD_RELAY D3
#define FOURTH_RELAY D5

#define SHOW_TIME_PERIOD 10       //sec
#define NTP_TIMEOUT 2000          // ms Response timeout for NTP requests //1500 говорят минимальное 2000
#define NTP_SYNC_PERIOD_MAX 86400 // 24*60*60  sec
#define LOOP_DELAY_MAX 50         // 24*60*60 sec

unsigned emon_upload_period = 120; //Upload period sec
unsigned emon_get_period = 120;     //sec
int temp_max = 22.5; //максимальная температура, при которой все реле отключаются
int temp_prev_switch = 0; //температура, при которой последний раз загружалось реле, нужо чтобы осуществлять гистерезис
int n_relays_to_turn_on_prev = -1; //число реле, включенных при последнем переключении 

int ntp_sync_period = 63;
int loop_delay = 1;

int8_t timeZone = 2;
int8_t minutesTimeZone = 0;
const PROGMEM char *ntpServer = "ua.pool.ntp.org"; //"europe.pool.ntp.org"; //"ua.pool.ntp.org"; //"time.google.com"; //"ua.pool.ntp.org";//"pool.ntp.org";
//pool1.ntp.od.ua
bool wifiFirstConnected = false;
bool FirstStart = true;
unsigned long time_last_data_check = 0;
unsigned long t_sent, t_get = 0;
unsigned long time_last_emon_data = 0;
int n_relays_to_turn_on = 0;
unsigned emon_data_check_period = 10;
float dat, corrected_dat, corrected_dat_prev, degree_to_add;
String ip;

WiFiClient Client;

void onSTAConnected(WiFiEventStationModeConnected ipInfo)
{
  Serial.printf("Connected to %s\r\n", ipInfo.ssid.c_str());
}

// Start NTP only after IP network is connected
void onSTAGotIP(WiFiEventStationModeGotIP ipInfo)
{
  Serial.printf("Got IP: %s\r\n", ipInfo.ip.toString().c_str());
  Serial.printf("Connected: %s\r\n", WiFi.status() == WL_CONNECTED ? "yes" : "no");
  digitalWrite(ONBOARDLED, LOW); // Turn on LED
  wifiFirstConnected = true;
}

// Manage network disconnection
void onSTADisconnected(WiFiEventStationModeDisconnected event_info)
{
  Serial.printf("Disconnected from SSID: %s\n", event_info.ssid.c_str());
  Serial.printf("Reason: %d\n", event_info.reason);
  digitalWrite(ONBOARDLED, HIGH); // Turn off LED
  //NTP.stop(); // NTP sync can be disabled to avoid sync errors
  WiFi.reconnect();
}

void processSyncEvent(NTPSyncEvent_t ntpEvent)
{
  if (ntpEvent < 0)
  {
    Serial.printf("Time Sync error: %d\n", ntpEvent);
    if (ntpEvent == noResponse)
      Serial.println("NTP server not reachable");
    else if (ntpEvent == invalidAddress)
      Serial.println("Invalid NTP server address");
    else if (ntpEvent == errorSending)
      Serial.println("Error sending request");
    else if (ntpEvent == responseError)
      Serial.println("NTP response error");
  }
  else
  {
    if (ntpEvent == timeSyncd)
    {
      Serial.print("Got NTP time: ");
      Serial.println(NTP.getTimeDateString(NTP.getLastNTPSync()));
    }
  }
}

boolean syncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent;            // Last triggered event

String get_emon_data()
{ //функция для получения данных из emoncms в формате json

  String json;
  Serial.print("connect to Server ");
  Serial.println(EMON_DOMAIN);
  Serial.print("GET /emoncms/feed/timevalue.json?id=");
  Serial.println(EMON_GET_NODE_ID);

  if (Client.connect(EMON_DOMAIN, 80))
  {
    Serial.println("connected");
    Client.print("GET /emoncms/feed/timevalue.json?id="); //http://udom.ua/emoncms/feed/feed/timevalue.json?id=18
    Client.print(EMON_GET_NODE_ID);
    Client.println();

    unsigned long tstart = millis();
    while (Client.available() == 0)
    {
      if (millis() - tstart > EMON_GET_DATA_TIMEOUT)
      {
        Serial.println(" --- Client Timeout !");
        Client.stop();
        return "0";
      }
    }

    // Read all the lines of the reply from server and print them to Serial
    while (Client.available())
    {
      json = Client.readStringUntil('\r');
      Serial.print("json = ");
      Serial.println(json);
    }

    Serial.println();
    Serial.println("closing connection");
  }
  return json;
}

// запрашиваем и извлекаем данные из json
void get_and_parse_json_data(
    unsigned long &time_last_data_check, //когда последний раз проверялись данные
    float &dat,                          //извлекаемые данные
    unsigned long &time_last_emon_data   //время последних данных котороые хранятся в emoncms
)
{

  if ((millis() - time_last_data_check) > emon_data_check_period)
  {
    String json = get_emon_data();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, json);

    dat = doc["value"];
    unsigned long time_last_emon_data = doc["time"];

    Serial.println();
    Serial.print("dat = ");
    Serial.print(dat);

    unsigned long dt_last_dat = (now() - time_last_emon_data); //разница во времени в секундах
    Serial.print(", time_last_emon_data = ");
    Serial.print(time_last_emon_data);
    Serial.print(", now = ");
    Serial.print(now());
    Serial.print(", dt_last_dat = ");
    Serial.println(dt_last_dat);

    time_last_data_check = millis();
  }
  else
  {
    Serial.print("dat data is fresh, next check in ");
    Serial.print(emon_data_check_period - (now() - time_last_data_check));
    Serial.println(" sec");
  }
}

void setup()
{
  delay(1000);
  static WiFiEventHandler e1, e2, e3;

  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  delay(500);
  Serial.flush();
  WiFi.mode(WIFI_STA);
  WiFi.begin(YOUR_WIFI_SSID, YOUR_WIFI_PASSWD);

  pinMode(ONBOARDLED, OUTPUT);    // Onboard LED
  digitalWrite(ONBOARDLED, HIGH); // Switch off LED

  NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
    ntpEvent = event;
    syncEventTriggered = true;
  });

  e1 = WiFi.onStationModeGotIP(onSTAGotIP); // As soon WiFi is connected, start NTP Client
  e2 = WiFi.onStationModeDisconnected(onSTADisconnected);
  e3 = WiFi.onStationModeConnected(onSTAConnected);

  pinMode(FIRST_RELAY, OUTPUT);
  pinMode(SECOND_RELAY, OUTPUT);

  ArduinoOTA.setHostname(OTA_HOSNAME); // Задаем имя сетевого порта
  //     ArduinoOTA.setPassword((const char *)"0000"); // Задаем пароль доступа для удаленной прошивки

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

bool startNTP()
{
  Serial.println();
  Serial.println("*** startNTP ***");
  NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
  //NTP.begin("pool.ntp.org", 2, true);
  delay(3000); // there seems to be a 1 second delay before first sync will be attempted, delay 2 seconds allows request to be made and received
  int counter = 1;
  Serial.print("NTP.getLastNTPSync() = ");
  Serial.println(NTP.getLastNTPSync());
  while (!NTP.getLastNTPSync() && counter <= 3)
  {
    NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
    Serial.print("NTP CHECK: #");
    Serial.println(counter);
    counter += 1;
    delay(counter * 2000);
  };
  NTP.setInterval(ntp_sync_period); // in seconds
  if (now() < 100000)
  {
    return false;
  }
  else
  {
    return true;
  }
}

void TimeValidator()
{ //проверяем время, если неправильное - перезагружаемся

  Serial.println("TimeValidator");
  for (int ectr = 1; ectr < 4; ectr++)
  {
    ip = WiFi.localIP().toString();
    if (now() < 100000 and (ip != "0.0.0.0"))
    {
      bool isntpok = startNTP();
      if (isntpok)
      {
        return;
      }
      Serial.print("Wrong UNIX time: now() = ");
      //Serial.println(NTP.getTimeStr());
      Serial.println(now());
      Serial.print("ip = ");
      Serial.println(ip);
      Serial.print("ectr = ");
      Serial.println(ectr);
      Serial.print("delay ");
      Serial.print(10000 * ectr);
      Serial.println(" sec");
      delay(30000 * ectr);
    }
    else
    {
      return;
    }
  }
  Serial.println("**** restart **** "); //перезагружаемся только при 3-х ошибках подряд
  delay(2000);

  //            WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while(1)wdt_reset();
  //            ESP.reset();
  ESP.restart();
}

void loop()
{

  if (FirstStart)
  {
    Serial.println("*** FirstStart ***");
    //Serial.println (" *** demo ***");
    //delay (1000);
    //демонстрируем, что работает
    //digitalWrite(FW, HIGH); delay(5000); digitalWrite(FW, LOW);
    //digitalWrite(BK, HIGH); delay(5000); digitalWrite(BK, LOW);
  }

  ArduinoOTA.handle(); // Всегда готовы к прошивке

  static int i = 0;
  static unsigned long last_show_time = 0;

  if (wifiFirstConnected)
  {
    Serial.println("*** wifiFirstConnected ***");
    wifiFirstConnected = false;
    NTP.setInterval(63);            //60 * 5 + 3    //63 Changes sync period. New interval in seconds.
    NTP.setNTPTimeout(NTP_TIMEOUT); //Configure response timeout for NTP requests milliseconds
    startNTP();
    //NTP.begin(ntpServer, timeZone, true, minutesTimeZone);
    NTP.getTimeDateString(); //dummy
  }

  if (syncEventTriggered)
  {
    processSyncEvent(ntpEvent);
    syncEventTriggered = false;
  }

  if ((millis() - last_show_time) > SHOW_TIME_PERIOD or FirstStart)
  {
    //Serial.println(millis() - last_show_time);
    last_show_time = millis();
    Serial.println();
    Serial.print("i = ");
    Serial.print(i);
    Serial.print(" ");
    Serial.print(NTP.getTimeDateString());
    Serial.print(" ");
    Serial.print(NTP.isSummerTime() ? "Summer Time. " : "Winter Time. ");
    Serial.print("WiFi is ");
    Serial.print(WiFi.isConnected() ? "connected" : "not connected");
    Serial.print(". ");
    Serial.print("Uptime: ");
    Serial.print(NTP.getUptimeString());
    Serial.print(" since ");
    Serial.println(NTP.getTimeDateString(NTP.getFirstSync()).c_str());

    Serial.printf("ESP8266 Chip id = %06X\n", ESP.getChipId());
    Serial.printf("WiFi.status () = %d", WiFi.status());
    Serial.println(", WiFi.localIP() = " + WiFi.localIP().toString());
    //        Serial.printf ("Free heap: %u\n", ESP.getFreeHeap ());
    i++;
  }

  //TimeValidator(); 

  get_and_parse_json_data(time_last_data_check, dat, time_last_emon_data);

  Serial.println();
  Serial.print("temperature in ");
  Serial.print(OTA_HOSNAME);
  Serial.print(" is ");
  Serial.println(dat, 2);
  Serial.print("temp_max = ");
  Serial.println(temp_max);


  if (hour() >= 23)
  {
    degree_to_add = -0.5; //ночью разрешаем греть чуть больше
  }
  else if (hour() >= 4 and hour() < 7)
  {
    degree_to_add = -1; //прогреваем комнату перед подъемом
  }
  else if (hour() >= 19 and hour() < 20)
  {
    degree_to_add = 0.5 ; //охлаждаем комнату перед сном 
  }
  else if (hour() >= 20 and hour() < 23)
  {
    degree_to_add = 1; //охлаждаем комнату перед сном
  }
  corrected_dat = dat + degree_to_add;

  Serial.print("corrected temperature in ");
  Serial.print(OTA_HOSNAME);
  Serial.print(" is ");
  Serial.println(corrected_dat);

  if (corrected_dat <= temp_max-0.5) //если температура на градус меньше максимальной, то греем на полную
  {
    // digitalWrite(FIRST_RELAY, HIGH);
    // digitalWrite(SECOND_RELAY, HIGH);
    n_relays_to_turn_on = 4;
  }
  else if (corrected_dat > temp_max-0.5 and corrected_dat < temp_max)
  {
    // digitalWrite(FIRST_RELAY, HIGH);
    // digitalWrite(SECOND_RELAY, LOW);
    n_relays_to_turn_on = 3;
  }
  else if (corrected_dat >= temp_max)
  {
    // digitalWrite(FIRST_RELAY, LOW);
    // digitalWrite(SECOND_RELAY, LOW);
    n_relays_to_turn_on = 0;
  }

  Serial.print("n_relays_to_turn_on = ");
  Serial.println(n_relays_to_turn_on);

  if (emon_data_check_period < EMON_DATA_CHECK_PERIOD_MAX)
  {
    emon_data_check_period += 10;
  }

  if (now() > 100000 and ip != "0.0.0.0" and ntp_sync_period < NTP_SYNC_PERIOD_MAX)
  { //постепенно увеличиваем период обновлений до суток
    ntp_sync_period += 63;
    Serial.print("ntp_sync_period = ");
    Serial.println(ntp_sync_period);
    NTP.setInterval(ntp_sync_period); // in seconds
    if (loop_delay < LOOP_DELAY_MAX)
    {                  //постепенно увеличиваем период обновлений до суток
      loop_delay += 1; //sec
    }
  }
  else if (now() < 100000 and ip != "0.0.0.0")
  {
    TimeValidator();
  }

  bool is_temp_change_big = abs(corrected_dat-corrected_dat_prev)>=0.1; //абсолютноеизменение температуры
  Serial.print("is_temp_change_big = ");
  Serial.println(is_temp_change_big);

  if (dat and n_relays_to_turn_on_prev != n_relays_to_turn_on and is_temp_change_big)
  {
    corrected_dat_prev = corrected_dat;
    n_relays_to_turn_on_prev = n_relays_to_turn_on;

    if (n_relays_to_turn_on == 4)
    {
      digitalWrite(FIRST_RELAY, HIGH);
      digitalWrite(SECOND_RELAY, HIGH);
      digitalWrite(THIRD_RELAY, HIGH);
      digitalWrite(FOURTH_RELAY, HIGH);
    }
    else if (n_relays_to_turn_on == 3)
    {
      digitalWrite(FIRST_RELAY, HIGH);
      digitalWrite(SECOND_RELAY, HIGH);
      digitalWrite(THIRD_RELAY, HIGH);
      digitalWrite(FOURTH_RELAY, LOW);
    }
    else
    {
      digitalWrite(FIRST_RELAY, LOW);
      digitalWrite(SECOND_RELAY, LOW);
      digitalWrite(THIRD_RELAY, LOW);
      digitalWrite(FOURTH_RELAY, LOW);
    }
  }
  else{
    n_relays_to_turn_on = n_relays_to_turn_on_prev; //если нет данных или температура изменилась меньше чем на 0.2 градуса, то реле не переключаем
  }

  //      if( Client.connect(EMON_DOMAIN, 80) && temp1>-50 && temp2>-50 )
  if (Client.connect(EMON_DOMAIN, 80) && (millis() - t_get) > emon_upload_period * 1000)
  {
    t_get = millis();
    Serial.println("program is HEATER-RELAY-4-WAY");
    Serial.println("connect to Server to SEND data...");
    Client.print("GET /");
    Client.print(EMON_PATH);
    Client.print("/input/post.json?apikey=");
    Client.print(EMON_APIKEY);
    Client.print("&node=");
    Client.print(EMON_SEND_NODE_ID);
    Client.print("&json={n_relay_1:");
    Client.print(n_relays_to_turn_on);
    //          Client.print(",temp2:");
    //          Client.print(temp2);
    Client.print("}");
    Client.println();
    //          http://udom.ua/emoncms/input/post.json?node=tutu&fulljson={power1:100,power2:200,power3:300}

    unsigned long old = millis();
    while ((millis() - old) < 500) // 500ms timeout for 'ok' answer from server
    {
      while (Client.available())
      {
        Serial.write(Client.read());
      }
    }
    Client.stop();
    Serial.println("\nclosed");

    if (emon_upload_period < EMON_UPLOAD_PERIOD_MAX)
    {
      emon_upload_period += 1;
    }
  }

  Serial.print("loop_delay = ");
  Serial.print(loop_delay);
  Serial.println(" sec");
  delay(loop_delay * 1000); //задержка большого цикла
  FirstStart = false;
}
