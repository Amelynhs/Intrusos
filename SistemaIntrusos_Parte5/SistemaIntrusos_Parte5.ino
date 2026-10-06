/*
  Sistema de Deteccion de Intrusos IoT - ESP32-S3 CAM + Supabase

  Hardware objetivo:
    ESP32-S3-CAM N16R8 + OV2640

  Funciones:
    - Wi-Fi AP + STA
    - Camara OV2640
    - Deteccion local de movimiento
    - API HTTP local + stream MJPEG
    - Horario local de armado
    - NTP / sincronizacion manual de hora
    - Registro de intrusiones en Supabase mediante REST HTTPS
    - Heartbeat/estado del dispositivo en Supabase
    - Descarga del horario mas reciente desde Supabase
    - Cola de eventos pendientes cuando Internet no esta disponible

  IMPORTANTE:
    Esta version usa el mapa de pines S3N16R8:
      D0=11 D1=9 D2=8 D3=10 D4=12 D5=18 D6=17 D7=16
      XCLK=15 PCLK=13 VSYNC=6 HREF=7 SDA=4 SCL=5
      PWDN=-1 RESET=-1

    Si tu ESP32-S3 CAM NO es N16R8, NO cargues esta version sin verificar
    el mapa de pines de tu placa. En placas S3-CAM el nombre comercial no
    garantiza el mismo cableado de camara.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "time.h"
#include "esp_heap_caps.h"
#include <ESP_Mail_Client.h>

// ============================================================
// Wi-Fi AP + STA
// ============================================================
const char* AP_SSID = "Intrusos-ESP32S3-CAM";
const char* AP_PASSWORD = "Intrusos123";

// CAMBIAR por tu Wi-Fi de casa para Internet/NTP/Supabase.
const char* STA_SSID = "Memelyn";
const char* STA_PASSWORD = "Puto26aaa";

IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GATEWAY(192, 168, 4, 1);
IPAddress AP_MASK(255, 255, 255, 0);

const long GMT_OFFSET_SEC = -5 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

// ============================================================
// Supabase
// ============================================================
const char* SUPABASE_URL = "https://nqqwufoxnwhourgtbafc.supabase.co";
const char* SUPABASE_PUBLISHABLE_KEY = "sb_publishable_fDYOL-t5V4t44MNyNImM4Q_AjJA4HHv";
const char* DEVICE_ID = "00000000-0000-0000-0000-000000000001";
const char* DEVICE_KEY = "ESP32CAM-PRINCIPAL-01";
const char* SMTP_HOST = "smtp.gmail.com";
const uint16_t SMTP_PORT = 465;
const char* SMTP_SENDER_EMAIL = "amelynhofmann@gmail.com";
const char* SMTP_SENDER_PASSWORD = "jkwu iksx spxd pesb";
const char* ALERT_RECIPIENT_EMAIL = "morita.aa2612@gmail.com";
const char* ALERT_RECIPIENT_NAME = "Propietario";

// REST API de Supabase / PostgREST.
String supabaseTableUrl(const char* table) {
  return String(SUPABASE_URL) + "/rest/v1/" + table;
}

// ============================================================
// ESP32-S3-CAM N16R8 / OV2640
// Mapa S3N16R8 verificado en referencias externas.
// ============================================================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      15
#define SIOD_GPIO_NUM       4
#define SIOC_GPIO_NUM       5
#define Y9_GPIO_NUM        16
#define Y8_GPIO_NUM        17
#define Y7_GPIO_NUM        18
#define Y6_GPIO_NUM        12
#define Y5_GPIO_NUM        10
#define Y4_GPIO_NUM         8
#define Y3_GPIO_NUM         9
#define Y2_GPIO_NUM        11
#define VSYNC_GPIO_NUM      6
#define HREF_GPIO_NUM       7
#define PCLK_GPIO_NUM      13

// Si tu placa tiene LED de estado, no lo usamos por ahora para evitar
// conflictos con variantes diferentes de S3-CAM.

// ============================================================
// Sistema
// ============================================================
enum SecurityState : uint8_t {
  DISARMED = 0,
  ARMED = 1,
  INTRUSION = 2
};

struct Schedule {
  bool enabled;
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t endHour;
  uint8_t endMinute;
};

volatile SecurityState securityState = DISARMED;
Schedule schedules[7];
SMTPSession smtp;
volatile bool emailBusy = false;

volatile bool motionDetected = false;
volatile bool ntpSynchronized = false;
volatile bool staConnected = false;
volatile bool streamBusy = false;
volatile bool manualOverride = false;
volatile bool scheduleInitialized = false;
volatile bool previousScheduleActive = false;
volatile uint32_t intrusionCount = 0;
volatile uint32_t lastMotionEpoch = 0;

httpd_handle_t apiServer = NULL;
httpd_handle_t streamServer = NULL;

SemaphoreHandle_t cameraMutex;
SemaphoreHandle_t latestFrameMutex;
SemaphoreHandle_t eventQueueMutex;

static uint8_t* previousGray = nullptr;
static const uint16_t MOTION_W = 80;
static const uint16_t MOTION_H = 60;
static const size_t MOTION_PIXELS = MOTION_W * MOTION_H;
static const uint16_t MOTION_THRESHOLD = 18;
static const uint16_t MOTION_PIXELS_REQUIRED = 450;
static const size_t MAX_LATEST_JPEG = 80 * 1024;
static uint8_t* latestJpeg = nullptr;
static size_t latestJpegLen = 0;

// Cola simple de eventos para reintentar cuando no hay Internet.
static const uint8_t EVENT_QUEUE_SIZE = 10;
struct PendingEvent {
  uint32_t epoch;
  uint8_t attempts;
  bool dbRecorded;
  bool emailSent;
};
PendingEvent pendingEvents[EVENT_QUEUE_SIZE];
uint8_t pendingHead = 0;
uint8_t pendingTail = 0;
uint8_t pendingCount = 0;

uint32_t lastSupabaseHeartbeat = 0;
uint32_t lastScheduleSync = 0;
uint32_t lastEmailTry = 0;

// ============================================================
// Utilidades
// ============================================================
String securityStateName() {
  switch (securityState) {
    case ARMED: return "ARMADO";
    case INTRUSION: return "INTRUSION";
    default: return "DESARMADO";
  }
}

bool getLocalTimeSafe(struct tm* timeinfo) {
  if (!getLocalTime(timeinfo, 50)) return false;
  return timeinfo->tm_year >= (2020 - 1900);
}

uint32_t currentEpoch() {
  time_t now;
  time(&now);
  if (now < 1577836800) return 0;
  return (uint32_t)now;
}

bool isScheduleActiveNow() {
  struct tm t;
  if (!getLocalTimeSafe(&t)) return false;
  int now=t.tm_hour*60+t.tm_min; int dow=t.tm_wday;
  for(int i=0;i<7;i++){
    if(!schedules[i].enabled) continue;
    int start=schedules[i].startHour*60+schedules[i].startMinute;
    int end=schedules[i].endHour*60+schedules[i].endMinute;
    if(start==end){ if(dow==i) return true; continue; }
    if(start<end && dow==i && now>=start && now<end) return true;
    if(start>end && dow==i && now>=start) return true;
    if(start>end && i==((dow+6)%7) && now<end) return true;
  }
  return false;
}

String nowISO8601() {
  struct tm t;
  if (!getLocalTimeSafe(&t)) return "TIME_NOT_SYNCED";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S-05:00", &t);
  return String(buf);
}

void updateAutomaticArming() {
  if (!ntpSynchronized && !currentEpoch()) return;

  bool active = isScheduleActiveNow();

  if (!scheduleInitialized) {
    previousScheduleActive = active;
    scheduleInitialized = true;
  } else if (active != previousScheduleActive) {
    manualOverride = false;
    previousScheduleActive = active;
  }

  if (manualOverride) return;

  if (active && securityState == DISARMED) {
    securityState = ARMED;
  } else if (!active && securityState == ARMED) {
    securityState = DISARMED;
  }
}

// ============================================================
// Cola de eventos
// ============================================================
void enqueueIntrusionEvent(uint32_t epoch) {
  if (!eventQueueMutex) return;
  if (xSemaphoreTake(eventQueueMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  if (pendingCount >= EVENT_QUEUE_SIZE) {
    // Conservamos los eventos mas recientes.
    pendingHead = (pendingHead + 1) % EVENT_QUEUE_SIZE;
    pendingCount--;
  }

  pendingEvents[pendingTail].epoch = epoch;
  pendingEvents[pendingTail].attempts = 0;
  pendingEvents[pendingTail].dbRecorded = false;
  pendingEvents[pendingTail].emailSent = false;
  pendingTail = (pendingTail + 1) % EVENT_QUEUE_SIZE;
  pendingCount++;

  xSemaphoreGive(eventQueueMutex);
}

bool peekPendingEvent(PendingEvent* event) {
  if (!event || !eventQueueMutex) return false;
  if (xSemaphoreTake(eventQueueMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool ok = pendingCount > 0;
  if (ok) *event = pendingEvents[pendingHead];
  xSemaphoreGive(eventQueueMutex);
  return ok;
}

void removePendingEvent() {
  if (!eventQueueMutex) return;
  if (xSemaphoreTake(eventQueueMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  if (pendingCount > 0) {
    pendingHead = (pendingHead + 1) % EVENT_QUEUE_SIZE;
    pendingCount--;
  }
  xSemaphoreGive(eventQueueMutex);
}

void incrementPendingAttempt() {
  if (!eventQueueMutex) return;
  if (xSemaphoreTake(eventQueueMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  if (pendingCount > 0 && pendingEvents[pendingHead].attempts < 255) {
    pendingEvents[pendingHead].attempts++;
  }
  xSemaphoreGive(eventQueueMutex);
}

// ============================================================
// Supabase REST
// ============================================================
void configureSupabaseHeaders(HTTPClient& http) {
  http.addHeader("apikey", SUPABASE_PUBLISHABLE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_PUBLISHABLE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
}

bool supabaseRequest(const String& url, const String& method, const String& body, int& code, String& response) {
  code = -1;
  response = "";

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  // HTTPS cifra la comunicacion. La verificacion del certificado queda
  // desactivada para simplificar la practica con Arduino; no usar este
  // patron para un producto comercial.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;

  configureSupabaseHeaders(http);

  if (method == "POST") {
    http.addHeader("Prefer", "return=minimal");
    code = http.POST(body);
  } else if (method == "PATCH") {
    http.addHeader("Prefer", "return=minimal");
    code = http.sendRequest("PATCH", body);
  } else if (method == "GET") {
    code = http.GET();
  } else {
    http.end();
    return false;
  }

  if (code > 0) response = http.getString();
  http.end();

  return code >= 200 && code < 300;
}

bool supabaseEventAlreadyRecorded(uint32_t epoch) {
  String url = supabaseTableUrl("intrusion_events") +
               "?device_id=eq." + String(DEVICE_ID) +
               "&metadata-%3E%3Eepoch=eq." + String(epoch) +
               "&select=id&limit=1";
  int code;
  String response;
  if (!supabaseRequest(url, "GET", "", code, response)) return false;
  return response.indexOf("\"id\"") >= 0;
}

bool supabaseInsertIntrusion(uint32_t epoch) {
  if (supabaseEventAlreadyRecorded(epoch)) {
    Serial.printf("[SUPABASE] Evento %lu ya estaba registrado; no se duplica.\n", (unsigned long)epoch);
    return true;
  }

  String url = supabaseTableUrl("intrusion_events");
  String body = "{";
  body += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  body += "\"detected_at\":\"" + nowISO8601() + "\",";
  body += "\"event_type\":\"motion\",";
  body += "\"status\":\"detected\",";
  body += "\"metadata\":{";
  body += "\"epoch\":" + String(epoch) + ",";
  body += "\"device_key\":\"" + String(DEVICE_KEY) + "\"";
  body += "}}";

  int code;
  String response;
  bool ok = supabaseRequest(url, "POST", body, code, response);

  Serial.printf("[SUPABASE] intrusion POST -> HTTP %d (%s)\n", code, ok ? "OK" : "ERROR");
  if (!ok && response.length()) Serial.println(response);
  return ok;
}

bool supabaseUpdateIntrusionStatus(uint32_t epoch, const char* status) {
  String url = supabaseTableUrl("intrusion_events") +
               "?device_id=eq." + String(DEVICE_ID) +
               "&metadata-%3E%3Eepoch=eq." + String(epoch);
  String body = "{\"status\":\"" + String(status) + "\"}";
  int code;
  String response;
  return supabaseRequest(url, "PATCH", body, code, response);
}

bool supabaseInsertLog(const char* eventName, const String& message) {
  String url = supabaseTableUrl("system_logs");
  String body = "{";
  body += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  body += "\"event\":\"" + String(eventName) + "\",";
  body += "\"message\":\"" + message + "\"";
  body += "}";

  int code;
  String response;
  return supabaseRequest(url, "POST", body, code, response);
}

bool supabaseUpdateDeviceStatus() {
  String status = "offline";
  if (staConnected) {
    if (securityState == INTRUSION) status = "intrusion";
    else if (securityState == ARMED) status = "armed";
    else status = "online";
  }

  String url = supabaseTableUrl("devices") + "?id=eq." + String(DEVICE_ID);
  String body = "{\"status\":\"" + status + "\",\"last_seen_at\":\"" + nowISO8601() + "\"}";

  int code;
  String response;
  bool ok = supabaseRequest(url, "PATCH", body, code, response);
  Serial.printf("[SUPABASE] heartbeat -> HTTP %d (%s)\n", code, ok ? "OK" : "ERROR");
  return ok;
}

bool extractJsonBool(const String& json, const char* key, bool& value) {
  String token = String("\"") + key + "\":";
  int p = json.indexOf(token);
  if (p < 0) return false;
  p += token.length();
  if (json.startsWith("true", p)) { value = true; return true; }
  if (json.startsWith("false", p)) { value = false; return true; }
  return false;
}

bool extractJsonInt(const String& json, const char* key, int& value) {
  String token = String("\"") + key + "\":";
  int p = json.indexOf(token);
  if (p < 0) return false;
  p += token.length();
  value = json.substring(p).toInt();
  return true;
}

bool supabaseSyncSchedule() {
  String url=supabaseTableUrl("schedules")+"?device_id=eq."+String(DEVICE_ID)+"&select=day_of_week,enabled,start_minute,end_minute&order=day_of_week.asc";
  int code; String response;
  if(!supabaseRequest(url,"GET","",code,response)) return false;
  for(int i=0;i<7;i++) schedules[i]={false,22,0,6,0};
  int pos=0;
  while(true){
    int obj=response.indexOf("{",pos); if(obj<0) break; int close=response.indexOf("}",obj); if(close<0) break;
    String item=response.substring(obj,close+1); pos=close+1;
    int day,startMin,endMin; bool enabled;
    if(!extractJsonInt(item,"day_of_week",day)||!extractJsonBool(item,"enabled",enabled)||!extractJsonInt(item,"start_minute",startMin)||!extractJsonInt(item,"end_minute",endMin)) continue;
    if(day<0||day>6||startMin<0||startMin>=1440||endMin<0||endMin>=1440) continue;
    schedules[day]={enabled,(uint8_t)(startMin/60),(uint8_t)(startMin%60),(uint8_t)(endMin/60),(uint8_t)(endMin%60)};
  }
  Serial.println("[SUPABASE] Horarios sincronizados"); return true;
}

bool smtpCredentialsReady() {
  return String(SMTP_SENDER_EMAIL).indexOf("CAMBIAR_") < 0 &&
         String(SMTP_SENDER_PASSWORD).indexOf("CAMBIAR_") < 0 &&
         String(ALERT_RECIPIENT_EMAIL).indexOf("CAMBIAR_") < 0;
}

bool sendIntrusionEmail(uint32_t epoch){
  if(!smtpCredentialsReady()||WiFi.status()!=WL_CONNECTED||emailBusy) return false; emailBusy=true;
  ESP_Mail_Session session; session.server.host_name=SMTP_HOST; session.server.port=SMTP_PORT; session.login.email=SMTP_SENDER_EMAIL; session.login.password=SMTP_SENDER_PASSWORD;
  SMTP_Message message; message.sender.name="Sistema de Intrusos"; message.sender.email=SMTP_SENDER_EMAIL; message.subject="ALERTA DE INTRUSION - ESP32-S3 CAM"; message.addRecipient(ALERT_RECIPIENT_NAME,ALERT_RECIPIENT_EMAIL);
  String body=String("Se detecto movimiento con el sistema armado.\n\nFecha/hora: ")+nowISO8601()+"\nEpoch: "+String(epoch)+"\nDispositivo: "+DEVICE_KEY;
  message.text.content=body.c_str(); message.text.charSet="utf-8"; message.text.transfer_encoding=Content_Transfer_Encoding::enc_7bit;
  if(!smtp.connect(&session)){ Serial.printf("[SMTP] Conexion fallida: %s\n", smtp.errorReason().c_str()); emailBusy=false; return false; }
  bool ok=MailClient.sendMail(&smtp,&message); if(!ok) Serial.printf("[SMTP] %s\n",smtp.errorReason().c_str()); else Serial.println("[SMTP] Alerta enviada"); smtp.closeSession(); emailBusy=false; return ok;
}

void processSupabaseEvents() {
  if (!staConnected || WiFi.status() != WL_CONNECTED) return;

  PendingEvent event;
  if (!peekPendingEvent(&event)) return;

  // Primero garantizamos el registro. Si ya existe por un reintento anterior,
  // supabaseEventAlreadyRecorded() evita crear una segunda fila.
  if (!event.dbRecorded) {
    if (!supabaseInsertIntrusion(event.epoch)) {
      incrementPendingAttempt();
      return;
    }

    if (eventQueueMutex && xSemaphoreTake(eventQueueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (pendingCount > 0) pendingEvents[pendingHead].dbRecorded = true;
      xSemaphoreGive(eventQueueMutex);
    }
    supabaseInsertLog("intrusion_registered", "Intrusion registrada en Supabase");
    event.dbRecorded = true;
  }

  // Sin credenciales no fingimos que el correo fue enviado. El evento queda
  // pendiente hasta que el estudiante configure SMTP.
  if (!smtpCredentialsReady()) {
    supabaseUpdateIntrusionStatus(event.epoch, "email_pending");
    supabaseInsertLog("email_pending", "SMTP sin configurar; evento conservado en cola");
    return;
  }

  if (event.emailSent) {
    removePendingEvent();
    return;
  }

  if (millis() - lastEmailTry < 8000) return;
  lastEmailTry = millis();

  if (sendIntrusionEmail(event.epoch)) {
    if (eventQueueMutex && xSemaphoreTake(eventQueueMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (pendingCount > 0) pendingEvents[pendingHead].emailSent = true;
      xSemaphoreGive(eventQueueMutex);
    }
    removePendingEvent();
    supabaseUpdateIntrusionStatus(event.epoch, "email_sent");
    supabaseInsertLog("intrusion_processed", "Intrusion registrada y alerta enviada");
  } else {
    incrementPendingAttempt();
    PendingEvent updated;
    peekPendingEvent(&updated);
    if (updated.attempts >= 5) {
      supabaseUpdateIntrusionStatus(event.epoch, "email_failed");
      supabaseInsertLog("email_failed", "No se pudo enviar el correo despues de 5 intentos; el evento queda registrado");
      removePendingEvent();
    } else {
      supabaseUpdateIntrusionStatus(event.epoch, "email_pending");
      supabaseInsertLog("email_pending", "Fallo SMTP; se reintentara");
    }
  }
}

void supabaseTask(void* parameter) {
  for (;;) {
    processSupabaseEvents();

    if (staConnected && WiFi.status() == WL_CONNECTED) {
      if (millis() - lastSupabaseHeartbeat >= 30000 || lastSupabaseHeartbeat == 0) {
        lastSupabaseHeartbeat = millis();
        supabaseUpdateDeviceStatus();
      }

      if (millis() - lastScheduleSync >= 30000 || lastScheduleSync == 0) {
        lastScheduleSync = millis();
        supabaseSyncSchedule();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ============================================================
// Movimiento
// ============================================================
bool analyzeMotionFromJpeg(const uint8_t* jpeg, size_t jpegLen) {
  if (!jpeg || jpegLen == 0 || !previousGray) return false;

  const size_t rgbBytes = MOTION_PIXELS * 2;
  uint8_t* rgb = (uint8_t*)heap_caps_malloc(rgbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb) rgb = (uint8_t*)malloc(rgbBytes);
  if (!rgb) return false;

  bool decoded = jpg2rgb565(jpeg, jpegLen, rgb, JPG_SCALE_4X);
  if (!decoded) {
    free(rgb);
    return false;
  }

  uint32_t changed = 0;
  uint32_t totalDiff = 0;

  for (size_t i = 0; i < MOTION_PIXELS; ++i) {
    uint16_t px = ((uint16_t)rgb[i * 2] << 8) | rgb[i * 2 + 1];
    uint8_t r = ((px >> 11) & 0x1F) << 3;
    uint8_t g = ((px >> 5) & 0x3F) << 2;
    uint8_t b = (px & 0x1F) << 3;
    uint8_t gray = (uint8_t)((30 * r + 59 * g + 11 * b) / 100);

    uint8_t old = previousGray[i];
    uint8_t diff = gray > old ? gray - old : old - gray;
    if (diff >= MOTION_THRESHOLD) changed++;
    totalDiff += diff;
    previousGray[i] = gray;
  }

  free(rgb);
  return changed >= MOTION_PIXELS_REQUIRED && totalDiff > 12000;
}

void registerMotion() {
  motionDetected = true;
  lastMotionEpoch = currentEpoch();

  if (securityState == ARMED) {
    securityState = INTRUSION;
    intrusionCount++;
    enqueueIntrusionEvent(lastMotionEpoch);
    Serial.printf("[ALERTA] Intrusion detectada: %s\n", nowISO8601().c_str());
  }
}

// ============================================================
// Camara
// ============================================================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] esp_camera_init fallo: 0x%x\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_vflip(sensor, 0);
    sensor->set_hmirror(sensor, 0);
  }

  Serial.printf("[CAMARA] OK. PSRAM=%s\n", psramFound() ? "SI" : "NO");
  return true;
}

// ============================================================
// Wi-Fi
// ============================================================
void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staConnected = true;
      Serial.print("[STA] IP: ");
      Serial.println(WiFi.localIP());
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staConnected = false;
      ntpSynchronized = false;
      Serial.println("[STA] Desconectado. Reintentando...");
      break;
    default:
      break;
  }
}

void initWiFi() {
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4);

  Serial.print("[AP] SSID: ");
  Serial.println(AP_SSID);
  Serial.print("[AP] IP: ");
  Serial.println(WiFi.softAPIP());

  if (String(STA_SSID) != "CAMBIAR_WIFI") {
    WiFi.begin(STA_SSID, STA_PASSWORD);
    Serial.print("[STA] Conectando a router");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
  } else {
    Serial.println("[STA] CONFIGURAR STA_SSID/STA_PASSWORD para Internet + Supabase");
  }
}

// ============================================================
// HTTP local
// ============================================================
static esp_err_t sendJson(httpd_req_t* req, const String& json, const char* status = "200 OK") {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json.c_str(), json.length());
}

String statusJson() {
  String json = "{";
  json += "\"state\":\"" + securityStateName() + "\",";
  json += "\"armed\":" + String(securityState != DISARMED ? "true" : "false") + ",";
  json += "\"motion\":" + String(motionDetected ? "true" : "false") + ",";
  json += "\"scheduleActive\":" + String(isScheduleActiveNow() ? "true" : "false") + ",";
  json += "\"staConnected\":" + String(staConnected ? "true" : "false") + ",";
  json += "\"timeSynced\":" + String(currentEpoch() > 0 ? "true" : "false") + ",";
  json += "\"apIp\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"timestamp\":\"" + nowISO8601() + "\",";
  json += "\"intrusionCount\":" + String(intrusionCount);
  json += "}";
  return json;
}

esp_err_t apiStatus(httpd_req_t* req) { return sendJson(req, statusJson()); }

esp_err_t apiArm(httpd_req_t* req) {
  manualOverride = true;
  securityState = ARMED;
  motionDetected = false;
  return sendJson(req, statusJson());
}

esp_err_t apiDisarm(httpd_req_t* req) {
  manualOverride = true;
  securityState = DISARMED;
  motionDetected = false;
  return sendJson(req, statusJson());
}

esp_err_t apiClearIntrusion(httpd_req_t* req) {
  manualOverride = true;
  securityState = DISARMED;
  motionDetected = false;
  return sendJson(req, statusJson());
}

esp_err_t apiCapture(httpd_req_t* req) {
  if (xSemaphoreTake(latestFrameMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return sendJson(req, "{\"error\":\"frame_busy\"}", "503 Service Unavailable");

  if (latestJpegLen == 0) {
    xSemaphoreGive(latestFrameMutex);
    return sendJson(req, "{\"error\":\"no_frame_yet\"}", "503 Service Unavailable");
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t result = httpd_resp_send(req, (const char*)latestJpeg, latestJpegLen);
  xSemaphoreGive(latestFrameMutex);
  return result;
}

esp_err_t apiTime(httpd_req_t* req) {
  size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1) return sendJson(req, "{\"error\":\"missing_epoch\"}", "400 Bad Request");

  char* query = (char*)malloc(len);
  if (!query) return sendJson(req, "{\"error\":\"no_memory\"}", "500 Internal Server Error");
  httpd_req_get_url_query_str(req, query, len);

  char epochText[24];
  if (httpd_query_key_value(query, "epoch", epochText, sizeof(epochText)) != ESP_OK) {
    free(query);
    return sendJson(req, "{\"error\":\"missing_epoch\"}", "400 Bad Request");
  }

  uint32_t epoch = strtoul(epochText, nullptr, 10);
  free(query);
  if (epoch < 1577836800UL) return sendJson(req, "{\"error\":\"invalid_epoch\"}", "400 Bad Request");

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  ntpSynchronized = true;
  return sendJson(req, statusJson());
}

esp_err_t apiSchedule(httpd_req_t* req) {
  size_t len = httpd_req_get_url_query_len(req) + 1;
  if (len <= 1) return sendJson(req, "{\"error\":\"missing_query\"}", "400 Bad Request");

  char* query = (char*)malloc(len);
  if (!query) return sendJson(req, "{\"error\":\"no_memory\"}", "500 Internal Server Error");
  httpd_req_get_url_query_str(req, query, len);

  char enabledText[4], startText[8], endText[8], dayText[4];
  bool ok = httpd_query_key_value(query, "enabled", enabledText, sizeof(enabledText)) == ESP_OK &&
            httpd_query_key_value(query, "start", startText, sizeof(startText)) == ESP_OK &&
            httpd_query_key_value(query, "end", endText, sizeof(endText)) == ESP_OK;

  if (!ok) {
    free(query);
    return sendJson(req, "{\"error\":\"expected_enabled_start_end\"}", "400 Bad Request");
  }

  int start = atoi(startText);
  int end = atoi(endText);
  int enabled = atoi(enabledText);
  int day = 0;
  if (httpd_query_key_value(query, "day", dayText, sizeof(dayText)) == ESP_OK) day = atoi(dayText);
  free(query);

  if (start < 0 || start >= 1440 || end < 0 || end >= 1440 || day < 0 || day > 6 || (enabled != 0 && enabled != 1))
    return sendJson(req, "{\"error\":\"invalid_schedule\"}", "400 Bad Request");

  schedules[day].enabled = enabled == 1;
  schedules[day].startHour = start / 60;
  schedules[day].startMinute = start % 60;
  schedules[day].endHour = end / 60;
  schedules[day].endMinute = end % 60;
  return sendJson(req, statusJson());
}

// ============================================================
// MJPEG
// ============================================================
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char* STREAM_BOUNDARY = "--frame\r\n";

esp_err_t streamHandler(httpd_req_t* req) {
  bool expected = false;
  if (!__atomic_compare_exchange_n((bool*)&streamBusy, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return sendJson(req, "{\"error\":\"stream_busy\"}", "503 Service Unavailable");

  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  while (res == ESP_OK) {
    size_t frameLen = 0;
    if (xSemaphoreTake(latestFrameMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      frameLen = latestJpegLen;
      if (frameLen > 0) {
        char header[96];
        int headerLen = snprintf(header, sizeof(header),
          "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)frameLen);
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, header, headerLen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)latestJpeg, frameLen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
      }
      xSemaphoreGive(latestFrameMutex);
    }
    if (res != ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  httpd_resp_send_chunk(req, NULL, 0);
  streamBusy = false;
  return res;
}

void registerUri(httpd_handle_t server, const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t item = {};
  item.uri = uri;
  item.method = method;
  item.handler = handler;
  item.user_ctx = NULL;
  httpd_register_uri_handler(server, &item);
}

bool startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 16;
  config.stack_size = 8192;
  config.lru_purge_enable = true;

  if (httpd_start(&apiServer, &config) != ESP_OK) return false;

  registerUri(apiServer, "/status", HTTP_GET, apiStatus);
  registerUri(apiServer, "/arm", HTTP_POST, apiArm);
  registerUri(apiServer, "/disarm", HTTP_POST, apiDisarm);
  registerUri(apiServer, "/clear", HTTP_POST, apiClearIntrusion);
  registerUri(apiServer, "/capture", HTTP_GET, apiCapture);
  registerUri(apiServer, "/time", HTTP_GET, apiTime);
  registerUri(apiServer, "/schedule", HTTP_GET, apiSchedule);

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = 32769;
  streamConfig.max_uri_handlers = 4;
  streamConfig.stack_size = 8192;
  streamConfig.lru_purge_enable = true;

  if (httpd_start(&streamServer, &streamConfig) != ESP_OK) return false;
  registerUri(streamServer, "/stream", HTTP_GET, streamHandler);

  Serial.println("[HTTP] API: http://192.168.4.1/status");
  Serial.println("[HTTP] Stream: http://192.168.4.1:81/stream");
  return true;
}

// ============================================================
// Tareas
// ============================================================
void cameraCaptureTask(void* parameter) {
  for (;;) {
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        bool motion = analyzeMotionFromJpeg(fb->buf, fb->len);
        if (motion) registerMotion();

        if (xSemaphoreTake(latestFrameMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          if (fb->len <= MAX_LATEST_JPEG) {
            memcpy(latestJpeg, fb->buf, fb->len);
            latestJpegLen = fb->len;
          } else latestJpegLen = 0;
          xSemaphoreGive(latestFrameMutex);
        }
        esp_camera_fb_return(fb);
      } else {
        Serial.println("[CAMARA] Captura fallo");
      }
      xSemaphoreGive(cameraMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void scheduleTask(void* parameter) {
  for (;;) {
    updateAutomaticArming();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void timeTask(void* parameter) {
  for (;;) {
    if (staConnected && !ntpSynchronized) {
      struct tm t;
      if (getLocalTimeSafe(&t)) {
        ntpSynchronized = true;
        Serial.printf("[NTP] Hora sincronizada: %s\n", nowISO8601().c_str());
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void diagnosticsTask(void* parameter) {
  for (;;) {
    Serial.printf("[DIAG] Heap=%u PSRAM=%u STA=%s Estado=%s Motion=%s Pendientes=%u\n",
      ESP.getFreeHeap(), ESP.getFreePsram(), staConnected ? "OK" : "OFF",
      securityStateName().c_str(), motionDetected ? "SI" : "NO", pendingCount);
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  for(int i=0;i<7;i++) schedules[i]={false,22,0,6,0};
  delay(1000);

  Serial.println();
  Serial.println("============================================");
  Serial.println(" Sistema de Intrusos - ESP32-S3 CAM");
  Serial.println(" S3 N16R8 + OV2640 + Supabase");
  Serial.println("============================================");

  previousGray = (uint8_t*)heap_caps_calloc(MOTION_PIXELS, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!previousGray) previousGray = (uint8_t*)calloc(MOTION_PIXELS, 1);
  if (!previousGray) while (true) delay(1000);

  latestJpeg = (uint8_t*)heap_caps_malloc(MAX_LATEST_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!latestJpeg) latestJpeg = (uint8_t*)malloc(MAX_LATEST_JPEG);
  if (!latestJpeg) while (true) delay(1000);

  cameraMutex = xSemaphoreCreateMutex();
  latestFrameMutex = xSemaphoreCreateMutex();
  eventQueueMutex = xSemaphoreCreateMutex();
  if (!cameraMutex || !latestFrameMutex || !eventQueueMutex) while (true) delay(1000);

  if (!initCamera()) while (true) delay(1000);

  initWiFi();

  if (!startServers()) while (true) delay(1000);

  xTaskCreatePinnedToCore(cameraCaptureTask, "CameraCapture", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(scheduleTask, "ScheduleTask", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(timeTask, "TimeTask", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(diagnosticsTask, "Diagnostics", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(supabaseTask, "Supabase", 8192, NULL, 1, NULL, 1);

  Serial.println("[OK] ESP32-S3 CAM inicializado");
  Serial.println("[OK] Supabase: REST HTTPS configurado");
}

void loop() {
  delay(1000);
}
