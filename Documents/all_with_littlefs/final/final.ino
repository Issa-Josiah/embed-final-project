/*************************************************
 * Cold Room Automation - Stable Version
 * ESP32 + RFID + HX711 + Servo + LEDs + Buzzer
 * LCD + RTC + Google Sheets + Email
 *************************************************/

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <RTClib.h>
#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP_Mail_Client.h>
#include <vector>
#include <WebServer.h>
#include <ArduinoJson.h>

// ------------------ WIFI ------------------
const char* WIFI_SSID = "Issa";
const char* WIFI_PASS = "Password1";

// ------------------ GOOGLE SHEETS ------------------
#define GOOGLE_SHEETS_URL "https://script.google.com/macros/s/AKfycbxEwabwWgovuhLOeoMUf5vGcJOcGYJaB9F0mHfBx_07CzPH0Q1RIkzTJCUrKZ_XNXqx/exec"
#define SPREADSHEET_LINK "https://docs.google.com/spreadsheets/d/1O53PSu8ygoGZ8fPUaG3c18rA4HmN1L1k7mqrWDRcOcs/edit?gid=0#gid=0"

// ------------------ EMAIL ------------------
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "githua.mwangi22@students.dkut.ac.ke"
#define AUTHOR_PASS  "uvph bita sxzy yepm"
#define RECIPIENT_EMAIL "g12.josiah1@gmail.com"

// ------------------ HARDWARE ------------------
#define SERVO_PIN 16
#define HX_DT 32
#define HX_SCK 33
#define LM35_PIN 34
#define GREEN_LED 25
#define RED_LED   26
#define BLUE_LED  27
#define BUZZER    14

// ------------------ RFID ------------------
#define SS_PIN 5
#define RST_PIN 4

// ------------------ STORAGE ------------------
#define LOG_FILE "/logs.txt"
#define MAX_LOG_LINES 500   // limit to prevent SPIFFS overflow

// ------------------ GLOBALS ------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(SS_PIN, RST_PIN);
Servo doorServo;
HX711 scale;
RTC_DS3231 rtc;

bool doorOpened = false;
String doorOpenedBy = "";
float currentTemp = 0.0;
float meatWeight = 0.0;
float meatMin = 0.0;
float meatMax = 18.0;
bool addUIDMode = false;
String newUserName = "";
bool weightAlertSent = false;
unsigned long lastWeeklyEmail = 0;

// ------------------ USER LIST ------------------
struct User {
  String uid;
  String name;
};
std::vector<User> users;

// ------------------ EMAIL ------------------
SMTPSession smtp;
QueueHandle_t emailQueue;

// ------------------ LOG QUEUE ------------------
struct LogItem {
  String user;
  String eventType;
  float temp;
  float weight;
  String datetime;
};
QueueHandle_t logQueue;

// ------------------ HX711 smoothing ------------------
#define HX_SMOOTH_SAMPLES 10
float weightBuffer[HX_SMOOTH_SAMPLES];
int weightIndex = 0;

// ------------------ UTILITIES ------------------
String nowTime() {
  DateTime t = rtc.now();
  char buf[30];
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
          t.day(), t.month(), t.year(), t.hour(), t.minute(), t.second());
  return String(buf);
}

String readUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

bool isAuthorizedUID(String uid, String &name) {
  for (auto &u : users) {
    if (u.uid == uid) {
      name = u.name;
      return true;
    }
  }
  return false;
}

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cold Room Dashboard</title>
<style>
  body {
    font-family: Arial, sans-serif;
    background-color: #DCEED1;
    color: #736372;
    margin: 0;
    padding: 0;
  }
  header {
    background-color: #AAC0AA;
    padding: 20px;
    text-align: center;
    font-size: 24px;
    font-weight: bold;
  }
  section {
    background-color: #AAC0AA;
    margin: 10px;
    padding: 15px;
    border-radius: 10px;
  }
  h2 {
    color: #736372;
    margin-top: 0;
  }
  button {
    background-color: #A18276;
    color: #DCEED1;
    border: none;
    padding: 10px 20px;
    margin: 5px 0;
    border-radius: 5px;
    cursor: pointer;
  }
  button:hover { background-color: #736372; }
  input[type=text], input[type=number] {
    padding: 8px;
    margin: 5px 0;
    border-radius: 5px;
    border: 1px solid #7A918D;
    width: 100%;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    margin-top: 10px;
  }
  table, th, td {
    border: 1px solid #7A918D;
  }
  th, td {
    padding: 8px;
    text-align: center;
  }
  th {
    background-color: #A18276;
    color: #DCEED1;
  }
  td {
    background-color: #DCEED1;
  }
</style>
</head>
<body>

<header>Cold Room Dashboard</header>

<section>
  <h2>Manual Door Control</h2>
  <button onclick="doorAction('open')">Open Door</button>
  <button onclick="doorAction('close')">Close Door</button>
</section>

<section>
  <h2>RFID Management</h2>
  <input type="text" id="rfidName" placeholder="Enter Name">
  <button onclick="startRFID()">Add RFID</button>
  <button onclick="deleteRFID()">Delete RFID</button>
</section>

<section>
  <h2>Event Logs</h2>
  <table id="logTable">
    <thead>
      <tr>
        <th>Date</th>
        <th>Time</th>
        <th>Temperature</th>
        <th>Weight</th>
        <th>WeightDiff</th>
        <th>User</th>
        <th>Event</th>
      </tr>
    </thead>
    <tbody>
      <!-- Logs will populate here -->
    </tbody>
  </table>
</section>

<section>
  <h2>Meat Weight Threshold</h2>
  <input type="number" id="meatMin" placeholder="Enter Low Threshold">
  <button onclick="setThreshold()">Set Threshold</button>
</section>

<script>
function doorAction(action) {
  fetch('/door?cmd=' + action)
  .then(resp => resp.text())
  .then(data => alert(data));
}

function startRFID() {
  let name = document.getElementById('rfidName').value;
  if(name === '') { alert('Enter a name'); return; }
  fetch('/rfid/add?name=' + encodeURIComponent(name))
  .then(resp => resp.text())
  .then(data => alert(data));
}

function deleteRFID() {
  let name = document.getElementById('rfidName').value;
  if(name === '') { alert('Enter a name'); return; }
  fetch('/rfid/delete?name=' + encodeURIComponent(name))
  .then(resp => resp.text())
  .then(data => alert(data));
}

function setThreshold() {
  let min = document.getElementById('meatMin').value;
  if(min === '') { alert('Enter a value'); return; }
  fetch('/threshold?min=' + encodeURIComponent(min))
  .then(resp => resp.text())
  .then(data => alert(data));
}

// Poll logs every 5 seconds
setInterval(() => {
  fetch('/logs')
    .then(resp => resp.json())
    .then(data => {
      let tbody = document.querySelector('#logTable tbody');
      tbody.innerHTML = '';
      data.forEach(log => {
        let row = `<tr>
          <td>${log.Date}</td>
          <td>${log.Time}</td>
          <td>${log.Temperature}</td>
          <td>${log.Weight}</td>
          <td>${log.WeightDiff}</td>
          <td>${log.User}</td>
          <td>${log.Event}</td>
        </tr>`;
        tbody.innerHTML += row;
      });
    });
}, 5000);
</script>

</body>
</html>
)rawliteral";

#include <WebServer.h>
WebServer server(80);

void setupWebServer() {
  server.on("/", [](){ server.send(200, "text/html", htmlPage); });

  server.on("/door", [](){
    String cmd = server.arg("cmd");
    if(cmd == "open") {
        doorServo.write(70);
        doorOpened = true;
        doorOpenedBy = "DASHBOARD";
        digitalWrite(GREEN_LED, HIGH);
        digitalWrite(RED_LED, LOW);
        server.send(200,"text/plain","Door opened");
        queueLog("DASHBOARD", "Door Opened");
    } else if(cmd == "close") {
        doorServo.write(30);
        doorOpened = false;
        doorOpenedBy = "";
        digitalWrite(GREEN_LED, LOW);
        digitalWrite(RED_LED, HIGH);
        server.send(200,"text/plain","Door closed");
        queueLog("DASHBOARD", "Door Closed");
    } else server.send(400,"text/plain","Invalid command");
});


  server.on("/rfid/add", [](){
    String name = server.arg("name");
    if(name.length() > 0) {
        addUIDMode = true;
        newUserName = name;
        server.send(200,"text/plain","Scan the new RFID card for " + name);
    }
});

  server.on("/rfid/delete", []() {
    String name = server.arg("name");
    if (name.length() == 0) {
        server.send(400, "text/plain", "Name required");
        return;
    }

    // Remove user by name
    int removed = 0;
    users.erase(std::remove_if(users.begin(), users.end(),
        [&](const User &u) {
            if (u.name == name) {
                removed++;
                queueLog(name, "RFID Deleted");
                return true;
            }
            return false;
        }), users.end());

    if (removed > 0)
        server.send(200, "text/plain", "Deleted RFID for " + name);
    else
        server.send(404, "text/plain", "User not found: " + name);
});


  server.on("/threshold", [](){
    meatMin = server.arg("min").toFloat();
    server.send(200,"text/plain","Threshold updated: " + String(meatMin));
  });

  server.on("/logs", []() {
    DynamicJsonDocument jsonDoc(8192);
    JsonArray logArray = jsonDoc.to<JsonArray>();

    // First, read logs from SPIFFS
    if (SPIFFS.exists(LOG_FILE)) {
        File f = SPIFFS.open(LOG_FILE, FILE_READ);
        while (f && f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() < 10) continue;

            int p1 = line.indexOf("|");
            int p2 = line.indexOf("|", p1 + 1);
            int p3 = line.indexOf("|", p2 + 1);
            int p4 = line.indexOf("|", p3 + 1);
            if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) continue;

            String datetime = line.substring(0, p1).trim();
            String user = line.substring(p1 + 1, p2).trim();
            String eventType = line.substring(p2 + 1, p3).trim();
            String tempStr = line.substring(p3 + 1, p4).trim();
            String weightStr = line.substring(p4 + 1).trim();

            String datePart = datetime.substring(0, 10);
            String timePart = datetime.substring(11);

            JsonObject logObj = logArray.createNestedObject();
            logObj["Date"] = datePart;
            logObj["Time"] = timePart;
            logObj["Temperature"] = tempStr.toFloat();
            logObj["Weight"] = weightStr.toFloat();
            logObj["WeightDiff"] = 0.0; // optional
            logObj["User"] = user;
            logObj["Event"] = eventType;
        }
        f.close();
    }

    // TODO: optionally include queued logs (from logQueue) here

    String output;
    serializeJson(logArray, output);
    server.send(200, "application/json", output);
});

  server.begin();
}

// ------------------ SPIFFS LOG ------------------
void saveLog(String log) {
  // Limit log file size
  File f = SPIFFS.open(LOG_FILE, FILE_READ);
  if (f) {
    int lines = 0;
    while (f.available()) {
      f.readStringUntil('\n');
      lines++;
    }
    f.close();

    if (lines >= MAX_LOG_LINES) {
      SPIFFS.remove(LOG_FILE);
    }
  }

  f = SPIFFS.open(LOG_FILE, FILE_APPEND);
  if(!f) return;
  f.println(log);
  f.close();
}

// ------------------ LOG QUEUE + BACKGROUND HTTP TASK ------------------
void httpTask(void* param) {
  LogItem item;

  for (;;) {
    if (xQueueReceive(logQueue, &item, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() != WL_CONNECTED) continue;

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient https;

      if (https.begin(client, GOOGLE_SHEETS_URL)) {
        https.setTimeout(3000);
        https.addHeader("Content-Type", "application/json");

        // parse datetime
        int y = item.datetime.substring(0,4).toInt();
        int m = item.datetime.substring(5,7).toInt();
        int d = item.datetime.substring(8,10).toInt();
        int hh = item.datetime.substring(11,13).toInt();
        int mm = item.datetime.substring(14,16).toInt();
        int ss = item.datetime.substring(17,19).toInt();

        char payload[300];
        snprintf(payload, sizeof(payload),
          "{"
          "\"Date\":\"%04d-%02d-%02d\","
          "\"Time\":\"%02d:%02d:%02d\","
          "\"Temperature\":%.1f,"
          "\"Weight\":%.2f,"
          "\"User\":\"%s\","
          "\"Event\":\"%s\""
          "}",
          y,m,d,hh,mm,ss,item.temp,item.weight,item.user.c_str(),item.eventType.c_str()
        );

        int httpCode = https.POST(payload);
        https.end();

        if (httpCode != 200) {
          // failed -> save back to SPIFFS
          String log = item.datetime + " | " + item.user + " | " + item.eventType + " | " +
                       String(item.temp, 1) + " | " + String(item.weight, 2);
          saveLog(log);
        }
      }
    }
  }
}

// ------------------ GOOGLE SHEET ------------------
void queueLog(String user, String eventType) {
  DateTime t = rtc.now();
  String datetime = String(t.year()) + "-" + String(t.month()) + "-" + String(t.day()) +
               " " + String(t.hour()) + ":" + String(t.minute()) + ":" + String(t.second());

  LogItem item;
  item.user = user;
  item.eventType = eventType;
  item.temp = currentTemp;
  item.weight = meatWeight;
  item.datetime = datetime;

  // Offline: save locally
  if (WiFi.status() != WL_CONNECTED) {
    String log = datetime + " | " + user + " | " + eventType + " | " +
                 String(currentTemp,1) + " | " + String(meatWeight,2);
    saveLog(log);
    return;
  }

  // If online, queue it
  if(xQueueSend(logQueue, &item, 0) != pdTRUE) {
    // Queue full, save locally
    saveLog(item.datetime + " | " + item.user + " | " + item.eventType + " | " + 
            String(item.temp,1) + " | " + String(item.weight,2));
}
}

// ------------------ EMAIL ------------------
struct EmailTaskData {
  String subject;
  String body;
};

void emailTask(void* param) {
  EmailTaskData data;

  for (;;) {
    if (xQueueReceive(emailQueue, &data, portMAX_DELAY) == pdTRUE) {

      if (WiFi.status() != WL_CONNECTED) continue;

      SMTP_Message msg;
      msg.sender.name = "Cold Room";
      msg.sender.email = AUTHOR_EMAIL;
      msg.subject = data.subject;
      msg.addRecipient("Admin", RECIPIENT_EMAIL);
      msg.text.content = data.body;

      Session_Config cfg;
      cfg.server.host_name = SMTP_HOST;
      cfg.server.port = SMTP_PORT;
      cfg.login.email = AUTHOR_EMAIL;
      cfg.login.password = AUTHOR_PASS;

      if (smtp.connect(&cfg)) {
        MailClient.sendMail(&smtp, &msg);
        smtp.closeSession();
      }
    }
  }
}

void sendEmailAsync(String subject, String body) {
  EmailTaskData data = {subject, body};
  xQueueSend(emailQueue, &data, 0);
}

// ------------------ SETUP ------------------
void setup() {
  Serial.begin(115200);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Starting...");

  // SPIFFS
  SPIFFS.begin(true);

  // RTC
  rtc.begin();
  if (!rtc.isrunning()) {
    rtc.adjust(DateTime(__DATE__, __TIME__));
}


  // WiFi Station only
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(1);
  }

  // Hardware init
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  doorServo.attach(SERVO_PIN);
  doorServo.write(30); // closed

  scale.begin(HX_DT, HX_SCK);
  scale.set_scale(102631.0);
  scale.tare();
  for(int i = 0; i < HX_SMOOTH_SAMPLES; i++) weightBuffer[i] = 0;


  SPI.begin();
  rfid.PCD_Init();

  // Users (hardcoded for now)
  users.push_back({"D7DAE45", "USER1"});
  users.push_back({"202E75D5", "USER2"});

  // Queues
  emailQueue = xQueueCreate(5, sizeof(EmailTaskData));
  logQueue   = xQueueCreate(20, sizeof(LogItem));

  // Tasks
  xTaskCreate(emailTask, "EmailTask", 4096, NULL, 1, NULL);
  xTaskCreate(httpTask,  "HTTPTask",  16384, NULL, 1, NULL);

  lcd.clear();
  lcd.print("Ready");
  digitalWrite(BLUE_LED, HIGH);
}

// ------------------ LOOP ------------------
unsigned long lastLCD = 0;
unsigned long lastWiFiCheck = 0;
unsigned long buzzerEnd = 0;

File logFileReader;
bool logReaderOpen = false;

void loop() {

  // ---- Non-blocking buzzer ----
  if (buzzerEnd != 0 && millis() > buzzerEnd) {
    noTone(BUZZER);
    buzzerEnd = 0;
  }

  // WiFi reconnect every 10 seconds if disconnected
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi reconnect...");
        WiFi.begin(WIFI_SSID, WIFI_PASS);  // retry only if disconnected
    }
}

  // --------- NON-BLOCKING LOG UPLOAD ----------
  // --------- NON-BLOCKING LOG UPLOAD ----------
if (WiFi.status() == WL_CONNECTED && SPIFFS.exists(LOG_FILE)) {
    if (!logReaderOpen) {
        logFileReader = SPIFFS.open(LOG_FILE, FILE_READ);
        logReaderOpen = true;
    }

    if (logFileReader && logFileReader.available()) {
        String line = logFileReader.readStringUntil('\n');
        line.trim();
        if (line.length() >= 10) { // only process valid lines
            int p1 = line.indexOf("|");
            int p2 = line.indexOf("|", p1 + 1);
            int p3 = line.indexOf("|", p2 + 1);
            int p4 = line.indexOf("|", p3 + 1);
            if (p1 >= 0 && p2 >= 0 && p3 >= 0 && p4 >= 0) {
                String datetime = line.substring(0, p1).trim();
                String user = line.substring(p1 + 1, p2).trim();
                String eventType = line.substring(p2 + 1, p3).trim();
                String tempStr = line.substring(p3 + 1, p4).trim();
                String weightStr = line.substring(p4 + 1).trim();
                LogItem item;
                item.datetime = datetime;
                item.user = user;
                item.eventType = eventType;
                item.temp = tempStr.toFloat();
                item.weight = weightStr.toFloat();
                xQueueSend(logQueue, &item, 0);
            }
        }
    } else { // finished reading file
        logFileReader.close();
        logReaderOpen = false;
        SPIFFS.remove(LOG_FILE);
    }
}
   if(addUIDMode && rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        String uid = readUID();
        users.push_back({uid, newUserName});
        queueLog(newUserName, "RFID Added");
        addUIDMode = false;
        newUserName = "";
    }
  // Read temp
  currentTemp = analogRead(LM35_PIN) * (3.3 / 4095.0) * 100;
  // RFID handling
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = readUID();
    String name;
    if (isAuthorizedUID(uid, name)) {
      if (!doorOpened) {
        doorServo.write(70);
        doorOpened = true;
        doorOpenedBy = name;
        digitalWrite(GREEN_LED, HIGH);
        digitalWrite(RED_LED, LOW);
        digitalWrite(BLUE_LED, LOW);
        queueLog(name, "Door Opened");
      }
      else if (doorOpenedBy == name) {
        doorServo.write(30);
        doorOpened = false;
        doorOpenedBy = "";
        digitalWrite(GREEN_LED, LOW);
        digitalWrite(RED_LED, HIGH);
        tone(BUZZER, 2000);
        buzzerEnd = millis() + 300;
        digitalWrite(RED_LED, LOW);
        digitalWrite(GREEN_LED, HIGH);
        queueLog(name, "Door Closed");
      }
      else {
        digitalWrite(RED_LED, HIGH);
        digitalWrite(GREEN_LED, LOW);
        tone(BUZZER, 2000);
        buzzerEnd = millis() + 400;
        digitalWrite(RED_LED, LOW);
        digitalWrite(GREEN_LED, HIGH);
      }
    }
    else {
      digitalWrite(RED_LED, HIGH);
      digitalWrite(BLUE_LED, LOW);
      tone(BUZZER, 2000);
      buzzerEnd = millis() + 600;
      digitalWrite(RED_LED, LOW);
      digitalWrite(BLUE_LED, HIGH);
    }

    rfid.PICC_HaltA();
  }
  // Weight measurement only when door is open
  if (doorOpened && scale.is_ready()) {
    // Smooth weight readings
    meatWeight = scale.get_units(5);
    weightBuffer[weightIndex++] = meatWeight;
    if (weightIndex >= HX_SMOOTH_SAMPLES) weightIndex = 0;

    float avg = 0;
    for (int i = 0; i < HX_SMOOTH_SAMPLES; i++) {
      avg += weightBuffer[i];
    }
    meatWeight = avg / HX_SMOOTH_SAMPLES;
    // Alert if out of range (only once until stable)
    if (meatWeight < meatMin || meatWeight > meatMax) {
      if (!weightAlertSent) {
        queueLog("SYSTEM", "WEIGHT ALERT");
        sendEmailAsync("COLD ROOM WEIGHT ALERT",
                  "Weight out of range.\nLink: " + String(SPREADSHEET_LINK));
        tone(BUZZER, 1500);
        buzzerEnd = millis() + 800;
        weightAlertSent = true;
      }
    } else {
      weightAlertSent = false;
    }
  }
  // LCD update every 1 second (no flicker)
// LCD update every 1 second (no flicker)
if (millis() - lastLCD > 1000) {
    lastLCD = millis();
    lcd.clear();

    // Temperature alert check
    bool tempAlert = false;
    String tempMsg = String(currentTemp, 1) + "C";
    float tempMin = 0.0;   // set your thresholds
    float tempMax = 8.0;
    if (currentTemp < tempMin || currentTemp > tempMax) {
        tempAlert = true;
        tempMsg = "ALERT:" + String(currentTemp, 1) + "C";
    }
    // Door OPEN: show weight info
    static float lastWeight = 0.0;
    if (doorOpened) {
        float weightDiff = meatWeight - lastWeight;
        lastWeight = meatWeight;

        lcd.setCursor(0, 0);
        if (meatWeight > meatMax) {
            lcd.print("OVERLOAD ");
        } else {
            lcd.print("Wt:");
        }
        lcd.print(meatWeight, 2);
        lcd.print("kg");

        lcd.setCursor(0, 1);
        lcd.print("ΔWt:");
        lcd.print(weightDiff, 2);
        lcd.print("kg");

        if (tempAlert) {
            lcd.setCursor(12, 1);
            lcd.print("T:");
            lcd.print(currentTemp, 1);
        }
    }
    // Door CLOSED: show temperature and time
    else {
        lcd.setCursor(0, 0);
        lcd.print("T:");
        lcd.print(tempMsg);

        lcd.setCursor(0, 1);
        lcd.print("Time:");
        lcd.print(nowTime());
    }
}
    if(millis() - lastWeeklyEmail > 7UL*24*3600*1000) { // 1 week
        lastWeeklyEmail = millis();
        sendEmailAsync("Weekly Cold Room Report", "Spreadsheet link: " + String(SPREADSHEET_LINK));
    }
}