/****************************************************
 * ARCADE-QUIZ
 * Proyecto: Recurso Educativo Interactivo Aplicando Sistemas de Inteligencia Embebida
 * by Ing. Gambino, Fernando - NOV 2025
 *
 ****************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// =====================================================
// CONFIG
// =====================================================
static const char* MDNS_NAME = "arcadequiz";      // http://arcadequiz.local/
static const char* STATE_FILE = "/state.json";
static const char* STATE_TMP  = "/state.tmp";   // escritura atómica: tmp -> rename

// Tamaño máximo del estado (config + preguntas + resultados) aceptado por POST.
// El cuerpo HTTP queda en RAM dos veces (buffer del WebServer + copia local),
// por eso se limita a 64 KB (~128 KB de heap en el pico).
static const size_t MAX_STATE_BYTES = 64 * 1024;
// Margen libre que se exige en LittleFS además del propio archivo.
static const size_t FS_RESERVE_BYTES = 8 * 1024;

// ---- Pines (AJUSTAR si tu cableado usa otros) ----
// INPUT_PULLUP => pulsado = LOW
// Mapeo pedido: Rojo=A, Verde=B, Amarillo=C, Blanco=D, Azul=E
static const uint8_t PIN_BTN_A_RED     = 25; // Rojo -> A
static const uint8_t PIN_BTN_B_GREEN   = 26; // Verde -> B
static const uint8_t PIN_BTN_C_YELLOW  = 27; // Amarillo -> C
static const uint8_t PIN_BTN_D_WHITE   = 33; // Blanco -> D
static const uint8_t PIN_BTN_E_BLUE    = 32; // Azul -> E

static const uint8_t PIN_BTN_START     = 14; // btn_start físico -> "START"
static const uint8_t PIN_BTN_CANCEL    = 16; // rojo cancelar   -> "CANCEL"
static const uint8_t PIN_BTN_ACCEPT    = 17; // verde aceptar   -> "ACCEPT" (confirma)

static const uint8_t PIN_BUZZER        = 4;  // buzzer (pasivo recomendado)
static const uint8_t BUZZER_CH         = 0;  // canal LEDC

static const uint32_t DEBOUNCE_MS      = 40;
static const uint32_t ACCEPT_WINDOW_MS = 8000; // aceptar válido luego de elegir A-E

// =====================================================
// GLOBALS
// =====================================================
WebServer server(80);
// El estado vive en LittleFS (STATE_FILE) y se sirve directamente desde ahí,
// sin mantener una copia en RAM.

// /api/last_button
static volatile uint32_t g_eventId = 0;
static String g_lastButton = "";

// Para aceptar sin tocar tu JS: re-emitir última letra A-E
static String g_lastSelectedLetter = "";
static uint32_t g_lastSelectedMs = 0;

// =====================================================
// HELPERS
// =====================================================
static void pushEvent(const String& code) {
  g_eventId++;
  g_lastButton = code;
}

static void sendNoCache() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

static String contentType(const String& path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css"))  return "text/css; charset=utf-8";
  if (path.endsWith(".js"))   return "application/javascript; charset=utf-8";
  if (path.endsWith(".json")) return "application/json; charset=utf-8";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".ico"))  return "image/x-icon";
  return "text/plain; charset=utf-8";
}

// Sirve un archivo de LittleFS. Si no existe "<ruta>" pero sí "<ruta>.gz",
// envía la versión comprimida: streamFile() agrega "Content-Encoding: gzip"
// porque el nombre termina en .gz, y el navegador la descomprime.
// Así las librerías grandes (SheetJS) ocupan ~1/3 de la flash.
static bool serveFile(const String& uri) {
  String p = uri;
  if (p == "/") p = "/index.html";

  String fsPath = p;
  bool gz = false;
  if (!LittleFS.exists(fsPath)) {
    fsPath = p + ".gz";
    if (!LittleFS.exists(fsPath)) return false;
    gz = true;
  }

  File f = LittleFS.open(fsPath, "r");
  if (!f) return false;

  if (gz) {
    // Librerías de terceros con versión fija: se pueden cachear (1 semana)
    // y así no se retransmiten al ESP32 en cada recarga de la página.
    server.sendHeader("Cache-Control", "public, max-age=604800");
  } else {
    sendNoCache();
  }
  server.streamFile(f, contentType(p));
  f.close();
  return true;
}

// =====================================================
// AP SSID: ARCADE-QUIZ + last4(MAC)
// =====================================================
static String macSuffix4() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char suf[5];
  snprintf(suf, sizeof(suf), "%02X%02X", mac[4], mac[5]);
  return String(suf);
}
static String buildApSsid() {
  return String("ARCADE-QUIZ") + macSuffix4();
}

// =====================================================
// STATE DEFAULT + LOAD/SAVE
// =====================================================
static String defaultStateJson() {
  StaticJsonDocument<768> doc;

  JsonObject teacherConfig = doc.createNestedObject("teacherConfig");
  teacherConfig["nombreDocente"] = "";
  teacherConfig["apellidoDocente"] = "";
  teacherConfig["curso"] = "";
  teacherConfig["division"] = "";
  teacherConfig["numQuestions"] = 0;
  teacherConfig["timerMode"] = "up";
  teacherConfig["timerMinutes"] = 40;

  doc["questionsTxt"] = "";
  doc.createNestedArray("results");

  JsonObject adminCreds = doc.createNestedObject("adminCreds");
  adminCreds["user"] = "admin";
  adminCreds["pass"] = "admin0381";

  String out;
  serializeJson(doc, out);
  return out;
}

enum class SaveResult { Ok, NoSpace, WriteFailed };

static SaveResult saveState(const String& json) {
  // Espacio: durante la escritura conviven state.tmp (nuevo) y state.json (viejo).
  const size_t total = LittleFS.totalBytes();
  const size_t used  = LittleFS.usedBytes();
  if (used + json.length() + FS_RESERVE_BYTES > total) {
    return SaveResult::NoSpace;
  }

  File f = LittleFS.open(STATE_TMP, "w");
  if (!f) return SaveResult::WriteFailed;
  const size_t written = f.print(json);
  f.close();
  if (written != json.length()) {
    LittleFS.remove(STATE_TMP);
    return SaveResult::WriteFailed;
  }

  // Reemplazo: si se corta la energía entre remove y rename,
  // loadState() recupera state.tmp en el próximo arranque.
  LittleFS.remove(STATE_FILE);
  if (!LittleFS.rename(STATE_TMP, STATE_FILE)) return SaveResult::WriteFailed;
  return SaveResult::Ok;
}

static void loadState() {
  // Recuperación de una escritura interrumpida
  if (!LittleFS.exists(STATE_FILE) && LittleFS.exists(STATE_TMP)) {
    LittleFS.rename(STATE_TMP, STATE_FILE);
  }
  // Primer arranque (o archivo vacío): crear estado por defecto
  bool needDefault = !LittleFS.exists(STATE_FILE);
  if (!needDefault) {
    File f = LittleFS.open(STATE_FILE, "r");
    needDefault = !f || f.size() == 0;
    if (f) f.close();
  }
  if (needDefault) {
    saveState(defaultStateJson());
  }
}

// =====================================================
// BUZZER (LEDC)
// =====================================================
static void buzzerInit() {
  ledcAttachPin(PIN_BUZZER, BUZZER_CH);
  ledcWriteTone(BUZZER_CH, 0);
}

static void toneMs(uint16_t freq, uint16_t ms) {
  ledcWriteTone(BUZZER_CH, freq);
  delay(ms);
  ledcWriteTone(BUZZER_CH, 0);
  delay(15);
}

static void playBuzzer(const String& tipo) {
  if (tipo == "correcto") {
    toneMs(2000, 120);
    toneMs(2500, 120);
  } else if (tipo == "incorrecto") {
    toneMs(400, 250);
  } else if (tipo == "aprobado") {
    toneMs(1500, 180);
    toneMs(1800, 180);
    toneMs(2100, 260);
  } else {
    toneMs(900, 120);
  }
}

// =====================================================
// BUTTONS (INPUT_PULLUP + debounce)
// =====================================================
struct Btn {
  uint8_t pin;
  const char* code;     // "A".."E","START","CANCEL","ACCEPT"
  bool stable;
  bool lastRead;
  uint32_t lastChangeMs;
};

static Btn btns[] = {
  { PIN_BTN_A_RED,    "A", true, true, 0 },
  { PIN_BTN_B_GREEN,  "B", true, true, 0 },
  { PIN_BTN_C_YELLOW, "C", true, true, 0 },
  { PIN_BTN_D_WHITE,  "D", true, true, 0 },
  { PIN_BTN_E_BLUE,   "E", true, true, 0 },

  { PIN_BTN_START,    "START",  true, true, 0 },
  { PIN_BTN_CANCEL,   "CANCEL", true, true, 0 },
  { PIN_BTN_ACCEPT,   "ACCEPT", true, true, 0 },
};

static void buttonsInit() {
  for (auto &b : btns) {
    pinMode(b.pin, INPUT_PULLUP);
    b.stable = digitalRead(b.pin);
    b.lastRead = b.stable;
    b.lastChangeMs = millis();
  }
}

static void onButtonPressed(const String& code) {
  if (code == "START" || code == "CANCEL") {
    pushEvent(code);
    return;
  }

  // Letras A–E
  if (code.length() == 1 && code[0] >= 'A' && code[0] <= 'E') {
    g_lastSelectedLetter = code;
    g_lastSelectedMs = millis();
    pushEvent(code);
    return;
  }

  // ACCEPT: confirma re-emitiendo última letra dentro de ventana
  if (code == "ACCEPT") {
    if (g_lastSelectedLetter.length() == 1 &&
        (millis() - g_lastSelectedMs) <= ACCEPT_WINDOW_MS) {
      pushEvent(g_lastSelectedLetter);
    }
    return;
  }
}

static void pollButtons() {
  const uint32_t now = millis();
  for (auto &b : btns) {
    bool r = digitalRead(b.pin);

    if (r != b.lastRead) {
      b.lastRead = r;
      b.lastChangeMs = now;
    }

    if ((now - b.lastChangeMs) > DEBOUNCE_MS && r != b.stable) {
      b.stable = r;

      // flanco de presión: HIGH -> LOW
      if (b.stable == LOW) {
        onButtonPressed(String(b.code));
      }
    }
  }
}

// =====================================================
// API
// =====================================================
static void sendJsonError(int code, const char* error) {
  String out = String("{\"ok\":false,\"error\":\"") + error + "\"}";
  server.send(code, "application/json; charset=utf-8", out);
}

static void apiStateGet() {
  File f = LittleFS.open(STATE_FILE, "r");
  if (!f) {
    sendNoCache();
    server.send(200, "application/json; charset=utf-8", defaultStateJson());
    return;
  }
  sendNoCache();
  server.streamFile(f, "application/json; charset=utf-8");
  f.close();
}

// Valida el JSON recibido con memoria constante: el filtro conserva solo los
// objetos chicos (teacherConfig, adminCreds) y descarta "questionsTxt" y
// "results", que igualmente se recorren y se validan sintácticamente
// (estructura, comillas, llaves y corchetes; las secuencias de escape dentro
// de esos textos descartados no se verifican, cosa que JSON.stringify
// nunca genera de forma inválida).
// Antes se usaba un documento fijo de 4 KB, que fallaba (NoMemory) con
// bancos de preguntas o listas de resultados grandes.
static DeserializationError validateState(const String& body, DynamicJsonDocument& doc) {
  StaticJsonDocument<128> filter;
  filter["teacherConfig"] = true;
  filter["adminCreds"] = true;

  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) return err;
  if (!doc.is<JsonObject>()) return DeserializationError::InvalidInput;
  return DeserializationError::Ok;
}

static void apiStatePost() {
  if (!server.hasArg("plain")) {
    sendJsonError(400, "missing body");
    return;
  }
  String body = server.arg("plain");
  if (body.length() == 0) {
    sendJsonError(400, "missing body");
    return;
  }
  if (body.length() > MAX_STATE_BYTES) {
    sendJsonError(413, "state too large");
    return;
  }

  {
    DynamicJsonDocument doc(2048);
    DeserializationError err = validateState(body, doc);
    if (err) {
      Serial.printf("[STATE] JSON rechazado (%u bytes): %s\n",
                    (unsigned)body.length(), err.c_str());
      sendJsonError(400, err == DeserializationError::NoMemory ? "config too large" : "invalid json");
      return;
    }
  }

  switch (saveState(body)) {
    case SaveResult::Ok:
      server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
      break;
    case SaveResult::NoSpace:
      Serial.printf("[STATE] Sin espacio en LittleFS para %u bytes\n", (unsigned)body.length());
      sendJsonError(507, "no space");
      break;
    default:
      Serial.println("[STATE] Error al escribir state.json");
      sendJsonError(500, "write failed");
      break;
  }
}

static void apiLastButton() {
  StaticJsonDocument<128> doc;
  doc["eventId"] = g_eventId;
  doc["button"]  = g_lastButton;

  String out;
  serializeJson(doc, out);

  sendNoCache();
  server.send(200, "application/json; charset=utf-8", out);
}

static void apiBuzzer() {
  String tipo = "";

  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, server.arg("plain"))) {
      tipo = (const char*)(doc["tipo"] | "");
    }
  }
  if (tipo.length() == 0 && server.hasArg("tipo")) {
    tipo = server.arg("tipo");
  }

  playBuzzer(tipo);
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

// =====================================================
// WiFiManager + mDNS
// =====================================================
static void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);

  WiFiManager wm;
  wm.setDebugOutput(true);

  const String apSsid = buildApSsid();   // ARCADE-QUIZxxxx
  // AP sin password
  bool ok = wm.autoConnect(apSsid.c_str());
  if (!ok) {
    Serial.println("[WiFi] autoConnect falló o quedó en portal/AP.");
  } else {
    Serial.print("[WiFi] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  }
}

// =====================================================
// SETUP / LOOP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ARCADE QUIZ ESP32 (WiFiManager + mDNS + LittleFS) ===");

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount FAILED");
  } else {
    Serial.printf("[FS] LittleFS OK (%u / %u bytes usados)\n",
                  (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  }

  loadState();
  buttonsInit();
  buzzerInit();

  setupWiFi();

  if (MDNS.begin(MDNS_NAME)) {
    Serial.printf("[mDNS] OK: http://%s.local/\n", MDNS_NAME);
  } else {
    Serial.println("[mDNS] FAILED");
  }

  // API
  server.on("/api/state", HTTP_GET,  apiStateGet);
  server.on("/api/state", HTTP_POST, apiStatePost);
  server.on("/api/last_button", HTTP_GET, apiLastButton);
  server.on("/api/buzzer", HTTP_POST, apiBuzzer);

  // Web
  server.on("/", HTTP_GET, []() {
    if (!serveFile("/index.html")) {
      server.send(500, "text/plain; charset=utf-8",
                  "Falta /index.html en LittleFS. Copia tus archivos a /data y ejecuta: pio run -t uploadfs");
    }
  });

  server.onNotFound([]() {
    if (serveFile(server.uri())) return;
    server.send(404, "text/plain; charset=utf-8", "404 Not Found");
  });

  server.begin();
  Serial.println("[HTTP] Server started :80");
}

void loop() {
  server.handleClient();
  pollButtons();
}
