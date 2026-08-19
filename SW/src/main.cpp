#include <WiFi.h>
#include <WebServer.h>
#include "secrets.h" // WIFI_SSID a WIFI_PASSWORD

// Pins for UART comm with Roomba
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
bool oiInitialized = false;
String lastCommand = "unknown";

// ---------- WAKE-UP SEQUENCE ----------
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

  oiInitialized = true;
  lastCommand = "idle_awake";
}

// ---------- KEEPALIVE ----------
void keepAlive() {
  if (!oiInitialized) return; // nothing to keep alive yet
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

// ---------- DOCK / CHARGING STATE (packet 21) ----------
String getDockedStatus() {
  Serial2.write(OI_SENSORS);
  Serial2.write((byte)21);
  delay(50);

  if (Serial2.available() < 1) {
    return "unknown";
  }

  int state = Serial2.read();

  switch (state) {
    case 0: return "not_docked";
    case 1: return "docked_reconditioning";
    case 2: return "docked_full_charging";
    case 3: return "docked_trickle_charging";
    case 4: return "docked_waiting";
    case 5: return "docked_charging_fault";
    default: return "unknown";
  }
}

// ---------- OI MODE (packet 35) ----------
// Tells us Off / Passive / Safe / Full - useful to confirm the ESP32
// actually still has control, independent of the cleaning heuristic above.
String getOiMode() {
  Serial2.write(OI_SENSORS);
  Serial2.write((byte)35);
  delay(50);

  if (Serial2.available() < 1) {
    return "unknown";
  }

  int mode = Serial2.read();

  switch (mode) {
    case 0: return "off";
    case 1: return "passive";
    case 2: return "safe";
    case 3: return "full";
    default: return "unknown";
  }
}

// ---------- WEB ENDPOINTS ----------
void handleRoot() {
  String html = "<html><body style='font-family:sans-serif'>";
  html += "<h1>Roomba ovladac</h1>";
  html += "<p><a href='/start'><button>Start/Wake</button></a></p>";
  html += "<p><a href='/clean'><button>Spustit uklizeni</button></a></p>";
  html += "<p><a href='/dock'><button>Na nabijecku</button></a></p>";
  html += "<p><a href='/power'><button>Vypnout</button></a></p>";
  html += "<p><a href='/status'><button>Stav (JSON)</button></a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStart() {
  wakeRoomba();
  server.send(200, "text/plain", "Roomba is awake, Safe mode active");
}

void handleClean() {
  if (!oiInitialized) wakeRoomba(); // auto-init if nobody called /start yet
  Serial2.write(OI_CLEAN);
  lastCommand = "cleaning";
  server.send(200, "text/plain", "Started cleaning");
}

void handleDock() {
  if (!oiInitialized) wakeRoomba();
  Serial2.write(OI_DOCK);
  lastCommand = "returning_to_dock";
  server.send(200, "text/plain", "Roomba is docking");
}

void handlePower() {
  Serial2.write(OI_POWER);
  lastCommand = "off";
  oiInitialized = false; // OI needs Start again after Power
  server.send(200, "text/plain", "Roomba is off (in passive state)");
}

void handleStatus() {
  // Auto-init on first ever poll so /status works standalone too,
  // but we do NOT call wakeRoomba()/Start on every subsequent poll -
  // that would interrupt an active cleaning cycle.
  if (!oiInitialized) {
    wakeRoomba();
    delay(200);
  }

  int battery = readBatteryPercent();
  String docked = getDockedStatus();
  String oiMode = getOiMode();

  String response = "{";
  response += "\"battery_percent\":" + (battery >= 0 ? String(battery) : String("null")) + ",";
  response += "\"dock_state\":\"" + docked + "\",";
  response += "\"oi_mode\":\"" + oiMode + "\",";
  response += "\"last_command\":\"" + lastCommand + "\"";
  response += "}";

  server.send(200, "application/json", response);
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
  Serial.print("Connected! IP address: ");
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