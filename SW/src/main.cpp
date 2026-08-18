#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h" // WIFI_SSID a WIFI_PASSWORD

// Pins for UART coom with Roomba
// ESP32 TX -> LV1 -> HV1 -> Roomba pin 3 (RXD)
// ESP32 RX <- LV2 <- HV2 <- Roomba pin 4 (TXD)
#define ROOMBA_RX_PIN 16
#define ROOMBA_TX_PIN 17

// Pin for BRC (wake) signal
#define BRC_PIN 4

// OI opcode const
#define OI_START   128
#define OI_SAFE    131
#define OI_FULL    132
#define OI_POWER   133
#define OI_CLEAN   135
#define OI_DOCK    143
#define OI_SENSORS 142

WebServer server(80);

unsigned long lastKeepAlive = 0;
const unsigned long KEEPALIVE_INTERVAL = 1000;

// ---------- WAKE-UP SEKVENCE ----------
void wakeRoomba() {
  pinMode(BRC_PIN, OUTPUT);
  digitalWrite(BRC_PIN, LOW);
  delay(500);
  digitalWrite(BRC_PIN, HIGH);
  delay(100);

  Serial2.write(OI_START);
  delay(50);
  Serial2.write(OI_SAFE); // Safe mode
  delay(50);
}

// ---------- KEEPALIVE ----------
void keepAlive() {
  // We send anythng to keep alive
  Serial2.write(OI_SENSORS);
  Serial2.write((byte)21);
  // Throw away response
  delay(20);
  while (Serial2.available()) {
    Serial2.read();
  }
}

// ---------- BATT READ ----------
// Returns percentage or -1 in case of error
int readBatteryPercent() {
  // Packet 25 = battery charge (2 bytes, mAh)
  Serial2.write(OI_SENSORS);
  Serial2.write((byte)25);
  delay(50);

  if (Serial2.available() < 2) {
    return -1;
  }
  int chargeHigh = Serial2.read();
  int chargeLow = Serial2.read();
  int charge = (chargeHigh << 8) | chargeLow;

  // Packet 26 = battery capacity (2 bytes, mAh)
  Serial2.write(OI_SENSORS);
  Serial2.write((byte)26);
  delay(50);

  if (Serial2.available() < 2) {
    return -1;
  }
  int capHigh = Serial2.read();
  int capLow = Serial2.read();
  int capacity = (capHigh << 8) | capLow;

  if (capacity == 0) {
    return -1;
  }

  return (int)((charge * 100.0) / capacity);
}

// ---------- WEB ENDPOINTS ----------
void handleRoot() {
  String html = "<html><body style='font-family:sans-serif'>";
  html += "<h1>Roomba ovladac</h1>";
  html += "<p><a href='/start'><button>Start/Wake</button></a></p>";
  html += "<p><a href='/clean'><button>Spustit uklizeni</button></a></p>";
  html += "<p><a href='/dock'><button>Na nabijecku</button></a></p>";
  html += "<p><a href='/power'><button>Vypnout</button></a></p>";
  html += "<p><a href='/status'><button>Stav baterky</button></a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStart() {
  wakeRoomba();
  server.send(200, "text/plain", "Roomba is awake, Safe mode active");
}

void handleClean() {
  Serial2.write(OI_CLEAN);
  server.send(200, "text/plain", "Started cleaning");
}

void handleDock() {
  Serial2.write(OI_DOCK);
  server.send(200, "text/plain", "Roomba is docking");
}

void handlePower() {
  Serial2.write(OI_POWER);
  server.send(200, "text/plain", "Roomba is off (in passive state)");
}

void handleStatus() {
  int battery = readBatteryPercent();
  String response;
  if (battery >= 0) {
    response = "Batt: " + String(battery) + "%";
  } else {
    response = "Unable to read battery state (is roomba active? try /start)";
  }
  server.send(200, "text/plain", response);
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200); // PC debug via usb
  Serial2.begin(115200, SERIAL_8N1, ROOMBA_RX_PIN, ROOMBA_TX_PIN); // Roomba communication

  IPAddress local_IP(192, 168, 0, 200);
  IPAddress gateway(192, 168, 0, 1);
  IPAddress subnet(255, 255, 255, 0);

  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("Static IP config failed");
  }

  Serial.println("Connecting to wifi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected! IP adresa: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/start", handleStart);
  server.on("/clean", handleClean);
  server.on("/dock", handleDock);
  server.on("/power", handlePower);
  server.on("/status", handleStatus);
  server.begin();

  Serial.println("Web server is on.");
}

// ---------- LOOP ----------
void loop() {
  server.handleClient();

  // Periodic keepalive
  if (millis() - lastKeepAlive > KEEPALIVE_INTERVAL) {
    keepAlive();
    lastKeepAlive = millis();
  }
}