/* Light-Watcher firmware v2.1 (модернизация на базе v1.6)
Оригинал: https://github.com/Stanislav-developer/Light_Watcher
Авторы оригинала: Stanislav Turii, Noctyon

ЧТО НОВОГО В v2.0 (кратко, подробности в README.md):
  * Надёжное детектирование: усреднение АЦП, гистерезис, антидребезг (нет ложных "вимкнули/увімкнули").
  * Очередь уведомлений: события не теряются, если роутер/интернет пропали вместе со светом.
  * Устройство больше не "застревает" в режиме настройки при перезагрузке без интернета.
  * Внешние приложения: HTTP JSON API, Prometheus /metrics, Webhook (POST JSON), MQTT + Home Assistant autodiscovery.
  * Локальная веб-панель (http://lightwatcher.local) и страница настроек с паролем.
  * Новые команды бота: история, статистика, калибровка порога, тихие часы, шаблоны сообщений и др.
  * Автоматический переход на летнее/зимнее время (POSIX TZ).
  v2.1:
  * Мониторинг аккумулятора: напряжение, %, прогноз "на сколько хватит", предупреждение о разряде.
  * Подписка жильцов/соседей: /subscribe (открытая или с одобрения владельца).
  * Пульс (heartbeat) на webhook — для сервера-агрегатора (папка server/).
  * OTA-обновление: веб-страница /update, Arduino IDE по сети, команда бота /update URL.

БИБЛИОТЕКИ (Arduino IDE -> Library Manager):
  * UniversalTelegramBot (Brian Lough) 1.3.0+
  * ArduinoJson (Benoit Blanchon) 7.x   <- именно 7-я версия
  * PubSubClient (Nick O'Leary) 2.8+
  Остальное (WiFi, WebServer, DNSServer, ESPmDNS, HTTPClient, Preferences) входит в ядро ESP32.

НАСТРОЙКИ ПРОШИВКИ (ESP32-C3), как в v1.6:
  Board: "ESP32C3 Dev Module", USB CDC On Boot: "Enabled",
  Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"  <- нужно для OTA.
  (со схемой "Huge APP (3MB No OTA)" из v1.6 прошивка тоже работает, но без OTA)

ВАЖНО: На ESP32-C3 АЦП1 — это GPIO 0..4 (GPIO5 — АЦП2, он не работает вместе с Wi-Fi).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <HTTPUpdate.h>
#include <HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>

// ============================================================================
// Константы
// ============================================================================
#define FW_VERSION      "2.1"
#define READ_PIN        3        // аналоговый вход детектора сети (ADC1)
#define BOOT_PIN        9        // кнопка BOOT на ESP32-C3 (на ESP32 обычно 0)
#define HIST_SIZE       30       // сколько последних отключений хранить
#define OUTBOX_SIZE     12       // размер очереди неотправленных уведомлений
#define TG_POLL_MS      2000     // как часто опрашивать Telegram
#define MAX_SUBS        30       // максимум подписчиков
#define AP_NAME         "Light-Watcher-Setup"

static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.google.com";
static const char* NTP3 = "time.cloudflare.com";

// Киев/Европа-Восток: авто-переход на летнее время (последнее вс марта / октября)
static const char* DEFAULT_TZ      = "EET-2EEST,M3.5.0/3,M10.5.0/4";
static const char* DEFAULT_MSG_OFF = "🔴 {time} <b>СВІТЛО ВИМКНУЛИ</b>";
static const char* DEFAULT_MSG_ON  = "🟢 {time} <b>СВІТЛО З'ЯВИЛОСЯ</b>\n\n⏱ Тривалість відключення: {duration}";

// ============================================================================
// Конфигурация (хранится в Preferences, редактируется в веб-интерфейсе и боте)
// ============================================================================
struct Config {
  String   ssid, pass;            // Wi-Fi
  String   botToken, chatId, groupId;
  String   name     = "Light Watcher";
  String   host     = "lightwatcher";   // mDNS: http://lightwatcher.local
  String   tz       = DEFAULT_TZ;
  uint16_t thrOn    = 3000;       // АЦП >= thrOn  -> свет ЕСТЬ
  uint16_t thrOff   = 2700;       // АЦП <  thrOff -> света НЕТ (гистерезис)
  uint32_t debounceMs = 3000;     // состояние должно держаться N мс
  int8_t   quietFrom = -1;        // тихие часы (уведомления без звука), -1 = выкл
  int8_t   quietTo   = -1;
  String   msgOff = DEFAULT_MSG_OFF;
  String   msgOn  = DEFAULT_MSG_ON;
  String   webhookUrl;            // POST JSON при каждом событии
  String   mqttHost; uint16_t mqttPort = 1883;
  String   mqttUser, mqttPass, mqttBase;
  String   apiKey;                // пароль веб-настроек и ключ API (логин: admin)
  int8_t   batPin   = -1;         // вход АЦП с делителя аккумулятора (-1 = не используется)
  float    batDiv   = 2.0;        // коэффициент делителя: Vбат = Vпина * batDiv
  uint8_t  batCells = 1;          // Li-ion банок последовательно (1S..4S)
  uint8_t  batLow   = 20;         // % — предупреждение о низком заряде
  bool     subsOpen = false;      // true — любой может /subscribe, false — с одобрения владельца
  uint16_t hbMin    = 0;          // пульс на webhook каждые N минут (0 = выкл)
} cfg;

// ============================================================================
// Объекты
// ============================================================================
Preferences          prefs;
WiFiClientSecure     tgClient;
UniversalTelegramBot bot("", tgClient);
WebServer            server(80);
DNSServer            dnsServer;
WiFiClient           mqttNet;
PubSubClient         mqtt(mqttNet);
HTTPUpdateServer     httpUpdater;

String deviceId;                  // короткий уникальный ID устройства (из MAC)

// ---- Состояние сети 220В ----
bool     powerOn      = true;     // устойчивое состояние (после антидребезга)
bool     rawPower     = true;     // мгновенное состояние (с гистерезисом)
uint32_t rawChangedAt = 0;
uint16_t lastAdc      = 0;

// ---- Учёт отключений ----
bool     inOutage      = false;   // идёт отключение (сохраняется между перезагрузками)
uint32_t offTs         = 0;       // unix-время начала отключения (0 = неизвестно)
uint32_t offMs         = 0;       // millis() начала (если NTP ещё не было)
bool     offMsValid    = false;
uint32_t outageCount   = 0;
uint32_t totalDownSec  = 0;
uint32_t lastOutageDur = 0;
uint32_t powerOnSince  = 0;

struct OutageRec { uint32_t off; uint32_t on; };
OutageRec hist[HIST_SIZE];
uint8_t   histHead = 0, histCount = 0;

// ---- Очередь уведомлений ----
enum EvType : uint8_t { EV_OFF, EV_ON, EV_ON_REBOOT, EV_BOOT, EV_TEST, EV_BAT_LOW };
#define T_OWNER 0x01
#define T_GROUP 0x02
#define T_HOOK  0x04
#define T_SUBS  0x08
struct Event {
  EvType   type;
  uint32_t ts;       // unix-время события (0 = ещё неизвестно)
  uint32_t ms;       // millis() события
  uint32_t dur;      // длительность отключения (для EV_ON) / состояние сети (для EV_BOOT)
  uint32_t refTs;    // начало отключения (для EV_ON)
  uint8_t  pending;  // каким адресатам ещё не доставлено
  uint8_t  fails;
  uint8_t  subIdx;   // до какого подписчика дошли
};
Event    outbox[OUTBOX_SIZE];
uint8_t  obHead = 0, obCount = 0;
uint32_t nextFlushAt = 0;

bool     timeSynced  = false;
uint32_t restartAt   = 0;
uint32_t mqttLastTry = 0;
bool     cfgOk       = false;     // конфигурация хоть раз успешно заработала

// ---- Аккумулятор ----
float    batV       = 0;          // сглаженное напряжение аккумулятора
uint32_t batRefMs   = 0;          // от какого момента считаем скорость разряда
float    batRefPct  = -1;
bool     batLowSent = false;

// ---- Подписчики ----
String   subs[MAX_SUBS];
uint8_t  subCount = 0;

bool batEnabled();
int batPct();
String batLine();

// ============================================================================
// Утилиты
// ============================================================================
bool timeValid() { return time(nullptr) > 1700000000; }
uint32_t nowTs() { return timeValid() ? (uint32_t)time(nullptr) : 0; }
bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

String fmtTs(uint32_t ts, const char* fmt) {
  if (!ts) return "??:??";
  time_t t = ts; struct tm tmv;
  localtime_r(&t, &tmv);
  char b[40];
  strftime(b, sizeof(b), fmt, &tmv);
  return String(b);
}
String isoTs(uint32_t ts) { return ts ? fmtTs(ts, "%Y-%m-%dT%H:%M:%S%z") : String(""); }

String formatDuration(uint32_t s) {
  uint32_t d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
  String r;
  if (d) r += String(d) + " д. ";
  if (h || d) r += String(h) + " год. ";
  if (m || h || d) r += String(m) + " хв. ";
  if (!d && !h && m < 10) r += String(sec) + " сек.";
  r.trim();
  return r;
}

String htmlEscape(const String& in) {
  String o; o.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String randomKey() {
  const char* abc = "abcdefghjkmnpqrstuvwxyz23456789";
  String k;
  for (int i = 0; i < 12; i++) k += abc[esp_random() % 31];
  return k;
}

bool isQuietNow() {
  if (cfg.quietFrom < 0 || cfg.quietTo < 0 || !timeValid()) return false;
  time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
  int h = tmv.tm_hour;
  if (cfg.quietFrom < cfg.quietTo) return h >= cfg.quietFrom && h < cfg.quietTo;
  return h >= cfg.quietFrom || h < cfg.quietTo;   // через полночь, напр. 23..7
}

// ============================================================================
// Хранилище (Preferences). Ключи v1.6 сохранены для бесшовного обновления.
// ============================================================================
// ---- Подписчики: хранятся строкой "id1,id2,..." ----
void loadSubs() {
  String s = prefs.getString("subs", "");
  subCount = 0;
  int from = 0;
  while (from < (int)s.length() && subCount < MAX_SUBS) {
    int e = s.indexOf(',', from);
    if (e < 0) e = s.length();
    String id = s.substring(from, e);
    id.trim();
    if (id.length()) subs[subCount++] = id;
    from = e + 1;
  }
}
void saveSubs() {
  String s;
  for (uint8_t i = 0; i < subCount; i++) { if (i) s += ","; s += subs[i]; }
  prefs.putString("subs", s);
}
int findSub(const String& id) {
  for (uint8_t i = 0; i < subCount; i++) if (subs[i] == id) return i;
  return -1;
}
bool addSub(const String& id) {
  if (id.isEmpty() || id.length() > 20 || findSub(id) >= 0 || subCount >= MAX_SUBS) return false;
  subs[subCount++] = id;
  saveSubs();
  return true;
}
bool delSub(const String& id) {
  int i = findSub(id);
  if (i < 0) return false;
  for (uint8_t j = i; j + 1 < subCount; j++) subs[j] = subs[j + 1];
  subCount--;
  saveSubs();
  return true;
}

void loadConfig() {
  cfg.ssid     = prefs.getString("ssid", "");
  cfg.pass     = prefs.getString("pass", "");
  cfg.botToken = prefs.getString("token", "");
  cfg.chatId   = prefs.getString("chatId", "");
  cfg.groupId  = prefs.getString("groupId", "");
  cfg.name     = prefs.getString("name", "Light Watcher");
  cfg.host     = prefs.getString("host", "lightwatcher");
  cfg.tz       = prefs.getString("tz-rule", DEFAULT_TZ);
  if (cfg.tz == "EEST-2" || cfg.tz == "EEST-3") cfg.tz = DEFAULT_TZ;   // миграция с v1.6
  cfg.thrOn    = prefs.getUShort("thrOn", 3000);
  cfg.thrOff   = prefs.getUShort("thrOff", 2700);
  cfg.debounceMs = prefs.getUInt("debounce", 3000);
  cfg.quietFrom = prefs.getChar("qFrom", -1);
  cfg.quietTo   = prefs.getChar("qTo", -1);
  cfg.msgOff   = prefs.getString("msgOff", DEFAULT_MSG_OFF);
  cfg.msgOn    = prefs.getString("msgOn", DEFAULT_MSG_ON);
  cfg.webhookUrl = prefs.getString("hook", "");
  cfg.mqttHost = prefs.getString("mqHost", "");
  cfg.mqttPort = prefs.getUShort("mqPort", 1883);
  cfg.mqttUser = prefs.getString("mqUser", "");
  cfg.mqttPass = prefs.getString("mqPass", "");
  cfg.mqttBase = prefs.getString("mqBase", "lightwatcher/" + deviceId);
  cfg.apiKey   = prefs.getString("apiKey", "");
  if (cfg.apiKey.isEmpty()) { cfg.apiKey = randomKey(); prefs.putString("apiKey", cfg.apiKey); }
  cfg.batPin   = prefs.getChar("batPin", -1);
  cfg.batDiv   = prefs.getFloat("batDiv", 2.0);
  cfg.batCells = prefs.getUChar("batCells", 1);
  cfg.batLow   = prefs.getUChar("batLow", 20);
  cfg.subsOpen = prefs.getBool("subsOpen", false);
  cfg.hbMin    = prefs.getUShort("hbMin", 0);
  loadSubs();
  cfgOk = prefs.getBool("cfgOk", false);
}

void loadState() {
  outageCount   = prefs.getUInt("powerOutageCnt", 0);
  offTs         = prefs.getUInt("pwrOffTmstmp", 0);
  inOutage      = prefs.getBool("lastUotDetect", false);
  totalDownSec  = prefs.getUInt("totDown", 0);
  lastOutageDur = prefs.getUInt("lastDur", 0);
  histHead      = prefs.getUChar("histHead", 0);
  histCount     = prefs.getUChar("histCnt", 0);
  if (prefs.getBytes("hist", hist, sizeof(hist)) != sizeof(hist) || histCount > HIST_SIZE || histHead >= HIST_SIZE) {
    histHead = histCount = 0;
  }
}

void saveHistory() {
  prefs.putBytes("hist", hist, sizeof(hist));
  prefs.putUChar("histHead", histHead);
  prefs.putUChar("histCnt", histCount);
}

void addHistory(uint32_t off, uint32_t on) {
  hist[(histHead + histCount) % HIST_SIZE] = { off, on };
  if (histCount < HIST_SIZE) histCount++; else histHead = (histHead + 1) % HIST_SIZE;
  saveHistory();
}

// i = 0 -> самое свежее отключение
const OutageRec& histAt(uint8_t i) { return hist[(histHead + histCount - 1 - i) % HIST_SIZE]; }

void clearStats() {
  outageCount = totalDownSec = lastOutageDur = 0;
  histHead = histCount = 0;
  prefs.putUInt("powerOutageCnt", 0);
  prefs.putUInt("totDown", 0);
  prefs.putUInt("lastDur", 0);
  saveHistory();
}

uint32_t currentOutageSec() {
  if (powerOn) return 0;
  if (offTs && timeValid()) return nowTs() - offTs;
  if (offMsValid) return (millis() - offMs) / 1000;
  return 0;
}

// Суммарное время без света за последние `window` секунд
uint32_t downtimeWindow(uint32_t window) {
  uint32_t now = nowTs();
  if (!now) return 0;
  uint32_t from = now - window, sum = 0;
  for (uint8_t i = 0; i < histCount; i++) {
    const OutageRec& r = histAt(i);
    uint32_t a = max(r.off, from), b = min(r.on, now);
    if (b > a) sum += b - a;
  }
  if (!powerOn && offTs) sum += now - max(offTs, from);
  return sum;
}

// ============================================================================
// Детектор сети: усреднение + гистерезис + антидребезг
// ============================================================================
uint16_t readAdcAvg() {
  uint32_t s = 0;
  for (int i = 0; i < 16; i++) s += analogRead(READ_PIN);
  return s / 16;
}

void samplePower() {
  static uint32_t last = 0;
  if (millis() - last < 50) return;
  last = millis();
  lastAdc = readAdcAvg();
  bool r = rawPower;
  if (lastAdc >= cfg.thrOn) r = true;
  else if (lastAdc < cfg.thrOff) r = false;          // между порогами — оставляем как было
  if (r != rawPower) { rawPower = r; rawChangedAt = millis(); }
}

// ============================================================================
// Очередь уведомлений (Telegram владелец / группа / webhook)
// ============================================================================
uint8_t defaultTargets() {
  uint8_t m = T_OWNER;
  if (cfg.groupId.length()) m |= T_GROUP;
  if (cfg.webhookUrl.length()) m |= T_HOOK;
  if (subCount) m |= T_SUBS;
  return m;
}

void pushEvent(EvType t, uint32_t dur, uint32_t refTs, uint8_t targets) {
  if (obCount == OUTBOX_SIZE) { obHead = (obHead + 1) % OUTBOX_SIZE; obCount--; }  // вытесняем самое старое
  Event& e = outbox[(obHead + obCount) % OUTBOX_SIZE];
  e.type = t; e.ts = nowTs(); e.ms = millis(); e.dur = dur; e.refTs = refTs;
  e.pending = targets; e.fails = 0; e.subIdx = 0;
  obCount++;
  nextFlushAt = millis();
}

String applyTpl(String t, const Event& e) {
  t.replace("{name}", cfg.name);
  t.replace("{time}", fmtTs(e.ts, "%H:%M"));
  t.replace("{date}", fmtTs(e.ts, "%d.%m.%Y"));
  t.replace("{datetime}", fmtTs(e.ts, "%d.%m.%Y %H:%M:%S"));
  t.replace("{duration}", formatDuration(e.dur));
  t.replace("{off_time}", fmtTs(e.refTs, "%d.%m %H:%M"));
  t.replace("{count}", String(outageCount));
  t.replace("{battery}", batPct() >= 0 ? String(String(batPct()) + "%") : String(""));
  return t;
}

String renderEvent(const Event& e, bool delayed) {
  String s;
  switch (e.type) {
    case EV_OFF:       s = applyTpl(cfg.msgOff, e); break;
    case EV_ON:        s = applyTpl(cfg.msgOn, e); break;
    case EV_ON_REBOOT: s = applyTpl(cfg.msgOn, e) + "\n⚠️ Пристрій вимикався (розрядився акумулятор)"; break;
    case EV_BOOT:
      s = "⚡ <b>" + htmlEscape(cfg.name) + "</b> v" FW_VERSION " запущено\n"
          "🕐 " + fmtTs(e.ts, "%d.%m.%Y %H:%M:%S") + "\n"
          "💡 Електромережа: " + String(e.dur ? "<b>ПРИСУТНЯ</b> ✅" : "<b>ВІДСУТНЯ</b> ⚠️ (резервне живлення)") + "\n"
          "🌐 http://" + WiFi.localIP().toString() + "  (http://" + cfg.host + ".local)";
      if (batPct() >= 0) s += "\n" + batLine();
      break;
    case EV_BAT_LOW:
      s = "🪫 <b>" + htmlEscape(cfg.name) + "</b>: заряд резерву " + String(e.dur) + "%";
      if (e.refTs) s += ", залишилось ~" + formatDuration(e.refTs);
      break;
    case EV_TEST: s = "🧪 Тестове повідомлення від <b>" + htmlEscape(cfg.name) + "</b>"; break;
  }
  if (delayed && e.type != EV_TEST) s += "\n📵 Надіслано із затримкою (не було мережі)";
  return s;
}

const char* eventName(EvType t) {
  switch (t) {
    case EV_OFF: return "power_off";
    case EV_ON: case EV_ON_REBOOT: return "power_on";
    case EV_BOOT: return "boot";
    case EV_BAT_LOW: return "battery_low";
    default: return "test";
  }
}

String eventJson(const Event& e) {
  JsonDocument d;
  d["event"]     = eventName(e.type);
  d["power"]     = (e.type == EV_OFF || e.type == EV_BAT_LOW) ? false : e.type == EV_BOOT ? (bool)e.dur : e.type == EV_TEST ? powerOn : true;
  d["device"]    = cfg.name;
  d["device_id"] = deviceId;
  d["ts"]        = e.ts;
  d["time"]      = isoTs(e.ts);
  if (e.type == EV_ON || e.type == EV_ON_REBOOT) {
    d["outage_duration_s"] = e.dur;
    d["outage_started"]    = isoTs(e.refTs);
    d["device_rebooted"]   = e.type == EV_ON_REBOOT;
  }
  d["outages_total"] = outageCount;
  d["delayed_s"]     = (millis() - e.ms) / 1000;
  if (batEnabled() && batV > 0) { d["battery_pct"] = batPct(); d["battery_v"] = roundf(batV * 100) / 100; }
  String s; serializeJson(d, s);
  return s;
}

bool sendTg(const String& chat, const String& text, bool silent = false) {
  if (chat.isEmpty() || cfg.botToken.isEmpty()) return true;
  JsonDocument doc;
  doc["chat_id"] = chat;
  doc["text"] = text;
  doc["parse_mode"] = "HTML";
  doc["disable_web_page_preview"] = true;
  if (silent) doc["disable_notification"] = true;
  return bot.sendPostMessage(doc.as<JsonObject>());
}

bool httpPostJson(const String& url, const String& body, const char* evName) {
  HTTPClient http;
  http.setTimeout(5000);
  int code = -1;
  if (url.startsWith("https://")) {
    WiFiClientSecure c; c.setInsecure();
    if (http.begin(c, url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-LightWatcher-Event", evName);
      code = http.POST(body);
      http.end();
    }
  } else {
    WiFiClient c;
    if (http.begin(c, url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-LightWatcher-Event", evName);
      code = http.POST(body);
      http.end();
    }
  }
  Serial.printf("Webhook -> %d\n", code);
  return code >= 200 && code < 300;
}

void flushOutbox() {
  if (!obCount || WiFi.status() != WL_CONNECTED || !due(nextFlushAt)) return;
  Event& e = outbox[obHead];
  if (!e.ts && timeValid()) e.ts = nowTs() - (millis() - e.ms) / 1000;  // восстанавливаем время задним числом
  bool delayed = (millis() - e.ms) > 60000;
  bool silent = isQuietNow() && e.type != EV_TEST;
  String text;
  if (e.pending & (T_OWNER | T_GROUP | T_SUBS)) text = renderEvent(e, delayed);

  // 1. Владелец. Не дошло — значит нет интернета: ждём и повторяем, остальным пока не шлём.
  if (e.pending & T_OWNER) {
    if (!sendTg(cfg.chatId, text, silent)) { nextFlushAt = millis() + 15000; return; }
    e.pending &= ~T_OWNER;
  }
  // 2. Группа и webhook
  if ((e.pending & T_GROUP) && sendTg(cfg.groupId, text, silent)) e.pending &= ~T_GROUP;
  if ((e.pending & T_HOOK) && (cfg.webhookUrl.isEmpty() || httpPostJson(cfg.webhookUrl, eventJson(e), eventName(e.type))))
    e.pending &= ~T_HOOK;
  // 3. Подписчики — порциями по 5, чтобы не блокировать устройство надолго.
  //    Ошибки отдельных подписчиков (например, заблокировали бота) пропускаем.
  if (e.pending & T_SUBS) {
    for (uint8_t n = 0; n < 5 && e.subIdx < subCount; n++, e.subIdx++) sendTg(subs[e.subIdx], text, silent);
    if (e.subIdx >= subCount) e.pending &= ~T_SUBS;
  }
  // Интернет есть (владелец получил), а группа/webhook стабильно не принимают — ошибка их настройки.
  if ((e.pending & (T_GROUP | T_HOOK)) && ++e.fails >= 10) {
    Serial.println("Outbox: група/webhook недоступні, пропускаю");
    e.pending &= ~(T_GROUP | T_HOOK);
  }

  if (!e.pending) { obHead = (obHead + 1) % OUTBOX_SIZE; obCount--; nextFlushAt = millis() + 300; }
  else if (e.pending == T_SUBS) nextFlushAt = millis() + 50;
  else nextFlushAt = millis() + 15000;
}

// ============================================================================
// Аккумулятор: напряжение, %, прогноз времени работы
// ============================================================================
// Кривая разряда Li-ion: напряжение на банку под небольшой нагрузкой -> %
static const float LI_V[] = { 3.30, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20 };
static const float LI_P[] = { 0,    5,    12,   27,   45,   62,   78,   90,   100  };

float liionPct(float cellV) {
  if (cellV <= LI_V[0]) return 0;
  for (int i = 1; i < 9; i++)
    if (cellV <= LI_V[i]) return LI_P[i - 1] + (LI_P[i] - LI_P[i - 1]) * (cellV - LI_V[i - 1]) / (LI_V[i] - LI_V[i - 1]);
  return 100;
}

bool batEnabled() { return cfg.batPin >= 0; }

int batPct() {
  if (!batEnabled() || batV <= 0) return -1;
  return (int)(liionPct(batV / (cfg.batCells ? cfg.batCells : 1)) + 0.5f);
}

// Прогноз: сколько секунд ещё проработает резерв (по скорости разряда за это отключение). -1 = мало данных
long batEtaSec() {
  int pct = batPct();
  if (powerOn || pct < 0 || batRefPct < 0) return -1;
  float elapsed = (millis() - batRefMs) / 1000.0f, drop = batRefPct - pct;
  if (elapsed < 300 || drop < 2) return -1;
  return (long)(pct / (drop / elapsed));
}

String batLine() {
  int pct = batPct();
  if (pct < 0) return "";
  String s = "🔋 Резерв: " + String(pct) + "% (" + String(batV, 2) + " В)";
  long eta = batEtaSec();
  if (eta > 0) s += ", вистачить ~" + formatDuration(eta - eta % 60);
  return s;
}

void sampleBattery() {
  if (!batEnabled()) return;
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(cfg.batPin);
  float v = mv / 8 / 1000.0f * cfg.batDiv;
  batV = batV <= 0 ? v : batV * 0.9f + v * 0.1f;   // сглаживание

  if (!powerOn) {
    // Первую минуту после пропадания сети не учитываем: напряжение "сползает" с уровня зарядки
    if (batRefPct < 0 && millis() - batRefMs > 60000) { batRefPct = batPct(); batRefMs = millis(); }
    int pct = batPct();
    if (!batLowSent && pct >= 0 && pct <= cfg.batLow) {
      batLowSent = true;
      long eta = batEtaSec();
      pushEvent(EV_BAT_LOW, pct, eta > 0 ? (uint32_t)eta : 0, T_OWNER | (cfg.webhookUrl.length() ? T_HOOK : 0));
    }
  }
}

// ============================================================================
// MQTT + Home Assistant autodiscovery
// ============================================================================
void fillStatus(JsonDocument& d) {
  d["device"]    = cfg.name;
  d["device_id"] = deviceId;
  d["version"]   = FW_VERSION;
  d["power"]     = powerOn;
  d["state"]     = powerOn ? "on" : "off";
  if (powerOn) {
    d["power_since"]            = powerOnSince;
    d["last_outage_duration_s"] = lastOutageDur;
  } else {
    d["outage_started"]    = offTs;
    d["outage_duration_s"] = currentOutageSec();
  }
  d["outages_total"]    = outageCount;
  d["downtime_total_s"] = totalDownSec + currentOutageSec();
  d["downtime_24h_s"]   = downtimeWindow(86400);
  d["downtime_7d_s"]    = downtimeWindow(7 * 86400);
  d["adc"]       = lastAdc;
  d["thr_on"]    = cfg.thrOn;
  d["thr_off"]   = cfg.thrOff;
  d["rssi"]      = WiFi.RSSI();
  d["ip"]        = WiFi.localIP().toString();
  d["uptime_s"]  = millis() / 1000;
  d["now"]       = nowTs();
  d["time_synced"] = timeValid();
  d["queue"]     = obCount;
  d["mqtt"]      = mqtt.connected();
  if (batEnabled() && batV > 0) {
    d["battery_pct"] = batPct();
    d["battery_v"]   = roundf(batV * 100) / 100;
    long eta = batEtaSec();
    if (eta > 0) d["battery_eta_s"] = eta;
  }
  d["subscribers"] = subCount;
  d["ota"] = esp_ota_get_next_update_partition(NULL) != NULL;
}

void mqttPublishState() {
  if (!mqtt.connected()) return;
  mqtt.publish((cfg.mqttBase + "/state").c_str(), powerOn ? "ON" : "OFF", true);
  JsonDocument d; fillStatus(d);
  String s; serializeJson(d, s);
  mqtt.publish((cfg.mqttBase + "/attributes").c_str(), s.c_str(), true);
}

void mqttDiscoveryOne(const char* component, const char* key, const char* nameUk,
                      const char* devClass, const char* valueTpl, const char* unit, const char* stateClass) {
  JsonDocument d;
  String uid = "lw_" + deviceId + "_" + key;
  d["name"]    = nameUk;
  d["uniq_id"] = uid;
  d["obj_id"]  = uid;
  d["avty_t"]  = cfg.mqttBase + "/availability";
  if (devClass) d["dev_cla"] = devClass;
  if (valueTpl) {
    d["stat_t"]  = cfg.mqttBase + "/attributes";
    d["val_tpl"] = valueTpl;
  } else {
    d["stat_t"]      = cfg.mqttBase + "/state";
    d["pl_on"]       = "ON";
    d["pl_off"]      = "OFF";
    d["json_attr_t"] = cfg.mqttBase + "/attributes";
  }
  if (unit) d["unit_of_meas"] = unit;
  if (stateClass) d["stat_cla"] = stateClass;
  JsonObject dev = d["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add("lw_" + deviceId);
  dev["name"] = cfg.name;
  dev["mf"]   = "Light Watcher";
  dev["mdl"]  = "ESP32-C3";
  dev["sw"]   = FW_VERSION;
  dev["cu"]   = "http://" + WiFi.localIP().toString();
  String topic = String("homeassistant/") + component + "/lw_" + deviceId + "/" + key + "/config";
  String s; serializeJson(d, s);
  mqtt.publish(topic.c_str(), s.c_str(), true);
}

void mqttDiscovery() {
  mqttDiscoveryOne("binary_sensor", "power", "Електромережа", "power", nullptr, nullptr, nullptr);
  mqttDiscoveryOne("sensor", "outages", "Кількість відключень", nullptr, "{{ value_json.outages_total }}", nullptr, "total_increasing");
  mqttDiscoveryOne("sensor", "outage_now", "Поточне відключення", "duration", "{{ value_json.outage_duration_s | default(0) }}", "s", nullptr);
  mqttDiscoveryOne("sensor", "down24", "Без світла за 24 год", "duration", "{{ value_json.downtime_24h_s }}", "s", nullptr);
  mqttDiscoveryOne("sensor", "adc", "Рівень АЦП", nullptr, "{{ value_json.adc }}", nullptr, "measurement");
  if (batEnabled()) {
    mqttDiscoveryOne("sensor", "battery", "Заряд резерву", "battery", "{{ value_json.battery_pct | default(0) }}", "%", "measurement");
    mqttDiscoveryOne("sensor", "battery_v", "Напруга акумулятора", "voltage", "{{ value_json.battery_v | default(0) }}", "V", "measurement");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String cmd; for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim(); cmd.toLowerCase();
  if (cmd == "status")       mqttPublishState();
  else if (cmd == "test")    pushEvent(EV_TEST, 0, 0, defaultTargets());
  else if (cmd == "restart") restartAt = millis() + 1000;
}

void mqttLoop() {
  if (cfg.mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) {
    if (mqttLastTry && millis() - mqttLastTry < 15000) return;
    mqttLastTry = millis();
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    String avail = cfg.mqttBase + "/availability";
    String cid = "lightwatcher-" + deviceId;
    bool ok = mqtt.connect(cid.c_str(),
                           cfg.mqttUser.length() ? cfg.mqttUser.c_str() : nullptr,
                           cfg.mqttUser.length() ? cfg.mqttPass.c_str() : nullptr,
                           avail.c_str(), 1, true, "offline");
    Serial.printf("MQTT connect %s: %s\n", cfg.mqttHost.c_str(), ok ? "OK" : "FAIL");
    if (ok) {
      mqtt.publish(avail.c_str(), "online", true);
      mqttDiscovery();
      mqttPublishState();
      mqtt.subscribe((cfg.mqttBase + "/cmd").c_str());
    }
    return;
  }
  mqtt.loop();
  static uint32_t lastAttr = 0;
  if (millis() - lastAttr > 60000) { lastAttr = millis(); mqttPublishState(); }
}

// Пульс: периодически шлём полный статус на webhook (сервер-агрегатор понимает, что устройство живо)
void heartbeatLoop() {
  if (!cfg.hbMin || cfg.webhookUrl.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  static uint32_t last = 0;
  static bool first = true;
  if (!first && millis() - last < cfg.hbMin * 60000UL) return;
  first = false;
  last = millis();
  JsonDocument d;
  d["event"] = "heartbeat";
  fillStatus(d);
  d["interval_s"] = cfg.hbMin * 60;
  String body; serializeJson(d, body);
  httpPostJson(cfg.webhookUrl, body, "heartbeat");
}

// ============================================================================
// Переходы состояния
// ============================================================================
void onPowerLost(bool notify) {
  powerOn = false; inOutage = true;
  offTs = nowTs(); offMs = millis(); offMsValid = true;
  batRefMs = millis(); batRefPct = -1;
  outageCount++;
  prefs.putUInt("pwrOffTmstmp", offTs);
  prefs.putBool("lastUotDetect", true);
  prefs.putUInt("powerOutageCnt", outageCount);
  if (notify) pushEvent(EV_OFF, 0, 0, defaultTargets());
  mqttPublishState();
  Serial.printf("СВЕТ ПРОПАЛ (ADC=%u)\n", lastAdc);
}

void onPowerRestored(bool afterReboot) {
  uint32_t now = nowTs(), dur = 0;
  if (offTs && now && now > offTs) dur = now - offTs;
  else if (offMsValid) dur = (millis() - offMs) / 1000;
  if (now && dur) addHistory(offTs ? offTs : now - dur, now);

  powerOn = true; inOutage = false;
  batRefPct = -1; batLowSent = false;
  lastOutageDur = dur; totalDownSec += dur; powerOnSince = now;
  prefs.putBool("lastUotDetect", false);
  prefs.putUInt("totDown", totalDownSec);
  prefs.putUInt("lastDur", lastOutageDur);
  pushEvent(afterReboot ? EV_ON_REBOOT : EV_ON, dur, offTs, defaultTargets());
  offTs = 0; offMsValid = false;
  mqttPublishState();
  Serial.printf("СВЕТ ПОЯВИЛСЯ (ADC=%u), отключение %lu сек\n", lastAdc, (unsigned long)dur);
}

// Как только появилось NTP-время — восстанавливаем неизвестное время начала отключения
void checkTimeSync() {
  if (timeSynced || !timeValid()) return;
  timeSynced = true;
  Serial.println("Время синхронизировано: " + fmtTs(nowTs(), "%d.%m.%Y %H:%M:%S"));
  if (inOutage && !offTs && offMsValid) {
    offTs = nowTs() - (millis() - offMs) / 1000;
    prefs.putUInt("pwrOffTmstmp", offTs);
  }
  if (powerOn && !powerOnSince) powerOnSince = nowTs() - millis() / 1000;
}

// ============================================================================
// Wi-Fi: переподключение, полный перезапуск стека при долгом отсутствии
// ============================================================================
void wifiWatchdog() {
  static uint32_t lostAt = 0, lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) { lostAt = 0; return; }
  if (!lostAt) { lostAt = millis(); lastTry = millis(); Serial.println("WiFi потерян"); }
  if (millis() - lastTry < 30000) return;
  lastTry = millis();
  if (millis() - lostAt > 10UL * 60 * 1000) {          // >10 мин — полный перезапуск Wi-Fi
    Serial.println("WiFi: перезапуск стека");
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    lostAt = millis();
  } else {
    WiFi.reconnect();
  }
}

// ============================================================================
// OTA: обновление прошивки по ссылке на .bin
// ============================================================================
bool otaAvailable() { return esp_ota_get_next_update_partition(NULL) != NULL; }

String otaFromUrl(const String& url) {
  if (!otaAvailable()) return "❌ Немає OTA-розділу. Один раз прошийте кабелем зі схемою \"Minimal SPIFFS (1.9MB APP with OTA)\"";
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return r;
  if (url.startsWith("https://")) { WiFiClientSecure c; c.setInsecure(); r = httpUpdate.update(c, url); }
  else { WiFiClient c; r = httpUpdate.update(c, url); }
  if (r == HTTP_UPDATE_OK) { restartAt = millis() + 2000; return "✅ Прошивку оновлено, перезапуск…"; }
  if (r == HTTP_UPDATE_NO_UPDATES) return "ℹ️ Оновлень немає";
  return "❌ Помилка оновлення: " + httpUpdate.getLastErrorString();
}

// ============================================================================
// Telegram
// ============================================================================
String powerLine() {
  if (powerOn) {
    String s = "💡 Електромережа: <b>ПРИСУТНЯ</b> ✅";
    if (powerOnSince && timeValid()) s += "\n🕐 З " + fmtTs(powerOnSince, "%d.%m %H:%M") + " (" + formatDuration(nowTs() - powerOnSince) + ")";
    return s;
  }
  String s = "💡 Електромережа: <b>ВІДСУТНЯ</b> ⚠️\n⏱ Без світла: " + formatDuration(currentOutageSec());
  if (offTs) s += "\n🕐 Вимкнули: " + fmtTs(offTs, "%d.%m %H:%M");
  return s;
}

String cmdLight() {
  if (powerOn) {
    String s = "🟢 <b>СВІТЛО Є!</b>";
    if (powerOnSince && timeValid()) s += "\n\n🕐 Увімкнули: " + fmtTs(powerOnSince, "%d.%m %H:%M") + "\n⏱ Вже " + formatDuration(nowTs() - powerOnSince);
    return s;
  }
  String s = "🔴 <b>СВІТЛА НЕМАЄ :(</b>\n\n⏱ Тривалість відключення: " + formatDuration(currentOutageSec());
  if (offTs) s += "\n🕐 Час відключення: " + fmtTs(offTs, "%d.%m %H:%M");
  if (batPct() >= 0) s += "\n" + batLine();
  return s;
}

String cmdStatus() {
  String s = "📟 <b>" + htmlEscape(cfg.name) + "</b> — стан системи\n\n" + powerLine() + "\n\n";
  s += "📊 Відключень: " + String(outageCount) + " (за 24 год: " + formatDuration(downtimeWindow(86400)) + " без світла)\n";
  s += "🔌 АЦП: " + String(lastAdc) + " (поріг " + String(cfg.thrOn) + "/" + String(cfg.thrOff) + ")\n";
  if (batEnabled()) s += batLine() + "\n";
  if (subCount) s += "👥 Підписників: " + String(subCount) + "\n";
  if (cfg.hbMin) s += "💓 Пульс: кожні " + String(cfg.hbMin) + " хв\n";
  s += "⬆️ OTA: " + String(otaAvailable() ? "доступно" : "немає розділу") + "\n";
  s += "📶 WiFi: " + String(WiFi.RSSI()) + " dBm, IP " + WiFi.localIP().toString() + "\n";
  s += "🔗 MQTT: " + String(cfg.mqttHost.isEmpty() ? "вимкнено" : mqtt.connected() ? "підключено" : "немає з'єднання");
  s += " | Webhook: " + String(cfg.webhookUrl.isEmpty() ? "вимкнено" : "увімкнено") + "\n";
  if (obCount) s += "📨 У черзі повідомлень: " + String(obCount) + "\n";
  if (cfg.quietFrom >= 0) s += "🌙 Тихі години: " + String(cfg.quietFrom) + ":00–" + String(cfg.quietTo) + ":00\n";
  s += "🕐 Час: " + (timeValid() ? fmtTs(nowTs(), "%d.%m.%Y %H:%M:%S") : String("не синхронізовано")) + "\n";
  s += "⏱ Аптайм: " + formatDuration(millis() / 1000) + "\n";
  s += "🧩 Прошивка v" FW_VERSION;
  return s;
}

String cmdHistory(int n) {
  if (n <= 0 || n > HIST_SIZE) n = 10;
  String s = "📜 <b>Останні відключення</b>\n\n";
  if (!powerOn) s += "🔴 " + fmtTs(offTs, "%d.%m %H:%M") + " → триває (" + formatDuration(currentOutageSec()) + ")\n";
  if (!histCount && powerOn) return s + "Поки що відключень не зафіксовано.";
  for (int i = 0; i < n && i < histCount; i++) {
    const OutageRec& r = histAt(i);
    s += String(i + 1) + ". " + fmtTs(r.off, "%d.%m %H:%M") + " → " + fmtTs(r.on, "%H:%M") +
         " (" + formatDuration(r.on - r.off) + ")\n";
  }
  return s;
}

String cmdStats() {
  uint32_t longest = 0, sum = 0;
  for (uint8_t i = 0; i < histCount; i++) {
    uint32_t d = histAt(i).on - histAt(i).off;
    sum += d; if (d > longest) longest = d;
  }
  String s = "📈 <b>Статистика</b>\n\n";
  s += "Без світла за 24 год: " + formatDuration(downtimeWindow(86400)) + "\n";
  s += "Без світла за 7 днів: " + formatDuration(downtimeWindow(7 * 86400)) + "\n";
  s += "Всього відключень: " + String(outageCount) + "\n";
  s += "Всього без світла: " + formatDuration(totalDownSec + currentOutageSec()) + "\n";
  if (histCount) {
    s += "Середнє (останні " + String(histCount) + "): " + formatDuration(sum / histCount) + "\n";
    s += "Найдовше: " + formatDuration(longest) + "\n";
  }
  return s;
}

String cmdHelp(const String& who) {
  return "👋 Привіт, " + htmlEscape(who) + "!\n\n"
    "<b>Для всіх (група):</b>\n"
    "/light — є світло чи ні\n"
    "/history — останні відключення\n"
    "/stats — статистика\n"
    "/subscribe — підписатися на сповіщення\n"
    "/unsubscribe — відписатися\n\n"
    "<b>Власник:</b>\n"
    "/status — стан системи\n"
    "/adc — поточне значення АЦП (калібрування)\n"
    "/threshold 3000 2700 — пороги \"є/нема\"\n"
    "/debounce 3000 — антидребезг, мс\n"
    "/quiet 23 7 | /quiet off — тихі години\n"
    "/name Дача — назва пристрою\n"
    "/tz auto | summer | winter | &lt;POSIX TZ&gt;\n"
    "/msg_off текст — шаблон \"вимкнули\"\n"
    "/msg_on текст — шаблон \"увімкнули\"\n"
    "   змінні: {time} {date} {datetime} {duration} {off_time} {count} {name}\n"
    "/webhook URL | off — HTTP-сповіщення\n"
    "/mqtt host [port] [user] [pass] | off\n"
    "/heartbeat 10 | 0 — пульс на webhook кожні N хв\n"
    "/battery — стан акумулятора\n"
    "/bat_pin 4 | off, /bat_div 2.0, /bat_cal 4.05, /bat_cells 1, /bat_low 20\n"
    "/subs — підписники; /subs open | closed\n"
    "/sub_add ID, /sub_del ID\n"
    "/update URL — оновити прошивку (.bin) по Wi-Fi\n"
    "/web — адреса веб-панелі та ключ API\n"
    "/newkey — згенерувати новий ключ API\n"
    "/test — тестове сповіщення у всі канали\n"
    "/clear_stats — очистити статистику\n"
    "/clear_data — скинути все та перейти в налаштування\n"
    "/restart — перезапуск\n"
    "/id — показати ID чату";
}

void setBotCommands() {
  if (prefs.getString("cmdVer", "") == FW_VERSION) return;
  String cmds = "[{\"command\":\"light\",\"description\":\"Є світло чи ні\"},"
                "{\"command\":\"status\",\"description\":\"Стан системи\"},"
                "{\"command\":\"history\",\"description\":\"Останні відключення\"},"
                "{\"command\":\"stats\",\"description\":\"Статистика\"},"
                "{\"command\":\"subscribe\",\"description\":\"Підписатися на сповіщення\"},"
                "{\"command\":\"battery\",\"description\":\"Стан акумулятора\"},"
                "{\"command\":\"web\",\"description\":\"Веб-панель і API\"},"
                "{\"command\":\"help\",\"description\":\"Усі команди\"}]";
  if (bot.setMyCommands(cmds)) prefs.putString("cmdVer", FW_VERSION);
}

void handleMessage(telegramMessage& m) {
  String chat = m.chat_id, text = m.text;
  text.trim();
  bool isAdmin = (m.from_id == cfg.chatId) || (chat == cfg.chatId);
  bool isGroup = cfg.groupId.length() && chat == cfg.groupId;
  Serial.println("TG: " + text + " від " + m.from_name + " (" + m.from_id + ") в " + chat);

  // Разбор: "/cmd@BotName аргументы"
  String cmd = text, args = "";
  int sp = text.indexOf(' ');
  if (sp > 0) { cmd = text.substring(0, sp); args = text.substring(sp + 1); args.trim(); }
  int at = cmd.indexOf('@');
  if (at > 0) cmd = cmd.substring(0, at);
  cmd.toLowerCase();

  String reply;
  if (cmd == "/id" || cmd == "/start") {
    reply = "🆔 Chat ID: <code>" + chat + "</code>\n👤 Ваш ID: <code>" + m.from_id + "</code>";
    if (isAdmin || isGroup) reply += "\n\nСписок команд: /help";
    sendTg(chat, reply);
    return;
  }
  bool isSub = findSub(chat) >= 0;

  // ---- Подписка (доступна любому) ----
  if (cmd == "/subscribe") {
    if (isGroup || chat == cfg.chatId) reply = "Цей чат і так отримує сповіщення ✅";
    else if (isSub) reply = "Ви вже підписані ✅ Відписатися: /unsubscribe";
    else if (cfg.subsOpen) {
      if (addSub(chat)) reply = "✅ Ви підписалися на сповіщення про світло (" + htmlEscape(cfg.name) + ").\nКоманди: /light /history /stats\nВідписатися: /unsubscribe";
      else reply = "❌ Досягнуто ліміт підписників";
    } else {
      static String lastReq;
      reply = "📨 Запит надіслано власнику. Ваш ID: <code>" + chat + "</code>";
      if (lastReq != chat) {                          // защита от спама одинаковыми запросами
        lastReq = chat;
        sendTg(cfg.chatId, "👤 " + htmlEscape(m.from_name) + " хоче підписатися на сповіщення.\nДодати: /sub_add " + chat);
      }
    }
    sendTg(chat, reply);
    return;
  }
  if (cmd == "/unsubscribe") {
    sendTg(chat, delSub(chat) ? "🔕 Ви відписалися" : "Ви не були підписані");
    return;
  }

  if (!isAdmin && !isGroup && !isSub) return;       // чужие чаты игнорируем

  // ---- Публичные команды ----
  if (text == "Світло є чи нема?" || cmd == "/light" || cmd == "/svitlo") reply = cmdLight();
  else if (cmd == "/history") reply = cmdHistory(args.toInt());
  else if (cmd == "/stats")   reply = cmdStats();
  else if (cmd == "/help")    reply = isAdmin ? cmdHelp(m.from_name) : String("Доступно: /light /history /stats /subscribe /unsubscribe");
  else if (!isAdmin) return;                        // остальное — только владельцу

  // ---- Команды владельца ----
  else if (cmd == "/status") reply = cmdStatus();
  else if (cmd == "/info") {
    reply = "⚡ <b>Light Watcher</b> v" FW_VERSION "\n\nМоніторинг електромережі з Telegram, HTTP API, Webhook та MQTT.\n"
            "📦 <a href='https://github.com/Stanislav-developer/Light_Watcher'>GitHub Repository</a>";
  }
  else if (cmd == "/adc") {
    samplePower();
    reply = "🔌 АЦП зараз: <b>" + String(readAdcAvg()) + "</b>\nПороги: є ≥ " + String(cfg.thrOn) + ", нема < " + String(cfg.thrOff) +
            "\n\nПідказка: заміряйте /adc при світлі і без нього, і поставте пороги між цими значеннями.";
  }
  else if (cmd == "/threshold") {
    int sp2 = args.indexOf(' ');
    int a = args.toInt(), b = sp2 > 0 ? args.substring(sp2 + 1).toInt() : a - 300;
    if (a > 0 && a <= 4095 && b > 0 && b < a) {
      cfg.thrOn = a; cfg.thrOff = b;
      prefs.putUShort("thrOn", a); prefs.putUShort("thrOff", b);
      reply = "✅ Пороги: є ≥ " + String(a) + ", нема < " + String(b);
    } else reply = "Формат: /threshold 3000 2700 (поріг \"є\" більший за поріг \"нема\")";
  }
  else if (cmd == "/debounce") {
    long v = args.toInt();
    if (v >= 0 && v <= 60000 && args.length()) { cfg.debounceMs = v; prefs.putUInt("debounce", v); reply = "✅ Антидребезг: " + String(v) + " мс"; }
    else reply = "Формат: /debounce 3000 (0…60000 мс)";
  }
  else if (cmd == "/quiet") {
    if (args == "off") { cfg.quietFrom = cfg.quietTo = -1; reply = "🔔 Тихі години вимкнено"; }
    else {
      int sp2 = args.indexOf(' ');
      int a = args.toInt(), b = sp2 > 0 ? args.substring(sp2 + 1).toInt() : -1;
      if (sp2 > 0 && a >= 0 && a < 24 && b >= 0 && b < 24 && a != b) {
        cfg.quietFrom = a; cfg.quietTo = b;
        reply = "🌙 Тихі години: " + String(a) + ":00–" + String(b) + ":00 (сповіщення без звуку)";
      } else { reply = "Формат: /quiet 23 7  або  /quiet off"; }
    }
    prefs.putChar("qFrom", cfg.quietFrom); prefs.putChar("qTo", cfg.quietTo);
  }
  else if (cmd == "/name") {
    if (args.length()) { cfg.name = args; prefs.putString("name", args); reply = "✅ Назва: " + htmlEscape(args); mqttLastTry = 0; mqtt.disconnect(); }
    else reply = "Формат: /name Дача";
  }
  else if (cmd == "/tz") {
    String tz = args;
    if (tz == "auto" || tz.isEmpty()) tz = DEFAULT_TZ;
    else if (tz == "summer") tz = "EEST-3";
    else if (tz == "winter") tz = "EET-2";
    cfg.tz = tz; prefs.putString("tz-rule", tz);
    setenv("TZ", tz.c_str(), 1); tzset();
    reply = "🕐 Часовий пояс: <code>" + htmlEscape(tz) + "</code>\nЗараз: " + fmtTs(nowTs(), "%d.%m.%Y %H:%M:%S");
  }
  else if (cmd == "/set_summer_time" || cmd == "/set_winter_time") {   // совместимость с v1.6
    reply = "ℹ️ Тепер перехід на літній/зимовий час автоматичний. Для ручного режиму: /tz summer або /tz winter";
  }
  else if (cmd == "/msg_off" || cmd == "/msg_on") {
    bool off = cmd == "/msg_off";
    String v = args;
    if (v == "default") v = off ? DEFAULT_MSG_OFF : DEFAULT_MSG_ON;
    if (v.length()) {
      v.replace("\\n", "\n");
      (off ? cfg.msgOff : cfg.msgOn) = v;
      prefs.putString(off ? "msgOff" : "msgOn", v);
      Event e{ off ? EV_OFF : EV_ON, nowTs(), (uint32_t)millis(), 3725, nowTs() - 3725, 0, 0 };
      reply = "✅ Шаблон збережено. Приклад:\n\n" + applyTpl(v, e);
    } else {
      reply = "Поточний шаблон:\n<code>" + htmlEscape(off ? cfg.msgOff : cfg.msgOn) + "</code>\n\n"
              "Змінити: " + cmd + " текст (\\n — новий рядок)\nСкинути: " + cmd + " default";
    }
  }
  else if (cmd == "/webhook") {
    if (args == "off") { cfg.webhookUrl = ""; reply = "Webhook вимкнено"; }
    else if (args.startsWith("http://") || args.startsWith("https://")) { cfg.webhookUrl = args; reply = "✅ Webhook: " + htmlEscape(args) + "\nПеревірка: /test"; }
    else reply = "Формат: /webhook https://example.com/hook  або  /webhook off\nЗараз: " + (cfg.webhookUrl.isEmpty() ? String("вимкнено") : htmlEscape(cfg.webhookUrl));
    prefs.putString("hook", cfg.webhookUrl);
  }
  else if (cmd == "/mqtt") {
    if (args == "off") {
      if (mqtt.connected()) mqtt.publish((cfg.mqttBase + "/availability").c_str(), "offline", true);
      mqtt.disconnect(); cfg.mqttHost = ""; prefs.putString("mqHost", ""); reply = "MQTT вимкнено";
    } else if (args.length()) {
      String p[4]; int n = 0, from = 0;
      while (n < 4 && from < (int)args.length()) {
        int e = args.indexOf(' ', from); if (e < 0) e = args.length();
        p[n++] = args.substring(from, e); from = e + 1;
      }
      cfg.mqttHost = p[0];
      cfg.mqttPort = p[1].length() ? p[1].toInt() : 1883;
      cfg.mqttUser = p[2]; cfg.mqttPass = p[3];
      prefs.putString("mqHost", cfg.mqttHost); prefs.putUShort("mqPort", cfg.mqttPort);
      prefs.putString("mqUser", cfg.mqttUser); prefs.putString("mqPass", cfg.mqttPass);
      mqtt.disconnect(); mqttLastTry = 0;
      reply = "✅ MQTT: " + htmlEscape(cfg.mqttHost) + ":" + String(cfg.mqttPort) + "\nТопік: <code>" + htmlEscape(cfg.mqttBase) + "</code>\nHome Assistant знайде пристрій автоматично.";
    } else reply = "Формат: /mqtt 192.168.1.10 1883 user pass  або  /mqtt off";
  }
  else if (cmd == "/web") {
    String ip = WiFi.localIP().toString();
    reply = "🌐 Панель: http://" + ip + "/  (http://" + cfg.host + ".local)\n"
            "⚙️ Налаштування: http://" + ip + "/settings\n   логін <code>admin</code>, пароль <code>" + cfg.apiKey + "</code>\n\n"
            "API (без пароля, лише локальна мережа):\n"
            "• GET /api/status — JSON стан\n• GET /api/history — історія\n• GET /metrics — Prometheus\n\n"
            "API з ключем (заголовок X-Api-Key):\n• POST /api/test\n• POST /api/restart";
  }
  else if (cmd == "/battery") {
    if (!batEnabled()) reply = "🔋 Моніторинг акумулятора вимкнено.\nУвімкнути: /bat_pin 4 (вхід АЦП через дільник напруги, див. README)";
    else {
      reply = batLine() + "\n\nПін: GPIO" + String(cfg.batPin) + ", дільник ×" + String(cfg.batDiv, 3) +
              ", Li-ion " + String(cfg.batCells) + "S, попередження при " + String(cfg.batLow) + "%\n"
              "Калібрування: виміряйте напругу мультиметром і надішліть /bat_cal 4.05";
    }
  }
  else if (cmd == "/bat_pin") {
    if (args == "off") { cfg.batPin = -1; reply = "🔋 Моніторинг акумулятора вимкнено"; }
    else {
      int p = args.toInt();
      if (args.length() && p >= 0 && p <= 4 && p != READ_PIN) { cfg.batPin = p; batV = 0; reply = "✅ Акумулятор на GPIO" + String(p) + "\nПеревірте: /battery"; }
      else reply = "Формат: /bat_pin 4 (GPIO 0…4, крім " + String(READ_PIN) + ")";
    }
    prefs.putChar("batPin", cfg.batPin);
    mqtt.disconnect(); mqttLastTry = 0;               // перепубликуем discovery
  }
  else if (cmd == "/bat_div") {
    float v = args.toFloat();
    if (v >= 1 && v <= 20) { cfg.batDiv = v; batV = 0; prefs.putFloat("batDiv", v); reply = "✅ Дільник ×" + String(v, 3); }
    else reply = "Формат: /bat_div 2.0  — (R1+R2)/R2";
  }
  else if (cmd == "/bat_cal") {
    float real = args.toFloat();
    if (batEnabled() && batV > 0.5 && real > 0.5 && real < 60) {
      cfg.batDiv *= real / batV; batV = real;
      prefs.putFloat("batDiv", cfg.batDiv);
      reply = "✅ Відкалібровано: дільник ×" + String(cfg.batDiv, 3);
    } else reply = "Формат: /bat_cal 4.05 — реальна напруга з мультиметра (моніторинг має бути увімкнено)";
  }
  else if (cmd == "/bat_cells") {
    int n = args.toInt();
    if (n >= 1 && n <= 4) { cfg.batCells = n; prefs.putUChar("batCells", n); reply = "✅ Li-ion " + String(n) + "S"; }
    else reply = "Формат: /bat_cells 1 (1…4)";
  }
  else if (cmd == "/bat_low") {
    int n = args.toInt();
    if (n >= 5 && n <= 80) { cfg.batLow = n; prefs.putUChar("batLow", n); reply = "✅ Попередження при " + String(n) + "%"; }
    else reply = "Формат: /bat_low 20 (5…80)";
  }
  else if (cmd == "/subs") {
    if (args == "open" || args == "closed") { cfg.subsOpen = args == "open"; prefs.putBool("subsOpen", cfg.subsOpen); }
    reply = "👥 Підписників: " + String(subCount) + "/" + String(MAX_SUBS) + "\nРежим: " +
            String(cfg.subsOpen ? "відкритий (будь-хто через /subscribe)" : "за схваленням власника") + "\n";
    for (uint8_t i = 0; i < subCount; i++) reply += "• <code>" + subs[i] + "</code>\n";
    reply += "\n/subs open | /subs closed\n/sub_add ID, /sub_del ID";
  }
  else if (cmd == "/sub_add") {
    if (addSub(args)) {
      reply = "✅ Додано " + htmlEscape(args);
      sendTg(args, "✅ Вас підписано на сповіщення про світло (" + htmlEscape(cfg.name) + ").\nВідписатися: /unsubscribe");
    } else reply = "Не вдалося: порожній ID, вже є або ліміт " + String(MAX_SUBS);
  }
  else if (cmd == "/sub_del") reply = delSub(args) ? "🗑 Видалено" : "Такого підписника немає";
  else if (cmd == "/heartbeat") {
    if (args.length()) {
      long v = args.toInt();
      if (v >= 0 && v <= 1440) { cfg.hbMin = v; prefs.putUShort("hbMin", v); }
    }
    if (cfg.hbMin) reply = "💓 Пульс на webhook кожні " + String(cfg.hbMin) + " хв";
    else reply = "💓 Пульс вимкнено. Увімкнути: /heartbeat 10 (потрібен /webhook)";
  }
  else if (cmd == "/update") {
    if (!args.startsWith("http")) {
      reply = "Формат: /update https://example.com/firmware.bin\nАбо відкрийте http://" + WiFi.localIP().toString() +
              "/update (логін admin, пароль — ключ API з /web)";
    } else {
      sendTg(chat, "⏳ Завантажую прошивку…");
      reply = otaFromUrl(args);
    }
  }
  else if (cmd == "/newkey") { cfg.apiKey = randomKey(); prefs.putString("apiKey", cfg.apiKey); reply = "🔑 Новий ключ API: <code>" + cfg.apiKey + "</code>"; }
  else if (cmd == "/test")   { pushEvent(EV_TEST, 0, 0, defaultTargets()); reply = "🧪 Тест поставлено в чергу (Telegram/група/webhook)"; }
  else if (cmd == "/clear_stats") { clearStats(); reply = "🧹 Статистику очищено"; }
  else if (cmd == "/clear_data")  { prefs.clear(); reply = "Конфігурацію та статистику очищено. Перезапуск у режим налаштування (Wi-Fi: " AP_NAME ")"; restartAt = millis() + 2000; }
  else if (cmd == "/restart")     { reply = "🔄 Перезапуск..."; restartAt = millis() + 2000; }
  else {
    if (chat != cfg.chatId) return;        // в группе не отвечаем на обычную переписку
    reply = "❓ Невідома команда. Список команд: /help";
  }

  sendTg(chat, reply);   // сначала отвечаем, потом (если надо) перезагружаемся — баг v1.6 исправлен
}

void handleTelegram() {
  if (WiFi.status() != WL_CONNECTED || cfg.botToken.isEmpty()) return;
  static uint32_t last = 0;
  if (millis() - last < TG_POLL_MS) return;
  last = millis();
  for (int guard = 0; guard < 3; guard++) {          // библиотека отдаёт по 1 сообщению за вызов
    int n = bot.getUpdates(bot.last_message_received + 1);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) handleMessage(bot.messages[i]);
  }
}

// ============================================================================
// Веб: страница настроек (общая для портала и локальной сети)
// ============================================================================
static const char PAGE_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html><html lang="uk"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Light Watcher</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1d2330;--mut:#6b7280;--acc:#0057B8;--bd:#e3e6eb;--on:#16a34a;--off:#dc2626}
@media (prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#181b21;--fg:#e6e8ec;--mut:#9aa1ad;--bd:#2a2f38}}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg)}
header{background:var(--acc);color:#fff;padding:18px 16px;border-bottom:4px solid #FFD700}
header h1{margin:0;font-size:20px}header p{margin:4px 0 0;opacity:.85;font-size:13px}
main{max-width:560px;margin:0 auto;padding:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:16px;margin-bottom:14px}
.card h2{margin:0 0 12px;font-size:15px}
label{display:block;font-size:12px;font-weight:600;color:var(--mut);margin:10px 0 4px}
input,textarea{width:100%;padding:10px 12px;border:1px solid var(--bd);border-radius:8px;font-size:15px;background:var(--bg);color:var(--fg)}
textarea{min-height:64px;font-family:inherit}
.row{display:flex;gap:10px}.row>div{flex:1}
.btn{display:block;width:100%;background:var(--acc);color:#fff;border:0;border-radius:8px;padding:14px;font-size:16px;font-weight:700;cursor:pointer}
.hint{font-size:12px;color:var(--mut);margin-top:6px}
.big{font-size:42px;font-weight:800;text-align:center;padding:28px 10px;border-radius:12px;color:#fff}
.k{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px dashed var(--bd);font-size:14px}
a{color:var(--acc)}code{font-size:13px}
</style></head><body>)rawliteral";

String inputField(const char* label, const char* name, const String& val, const char* type = "text", const char* ph = "") {
  return String("<label>") + label + "</label><input type='" + type + "' name='" + name + "' value='" +
         htmlEscape(val) + "' placeholder='" + ph + "'>";
}

String settingsPage(bool portal) {
  String keep = portal ? "" : "залиште порожнім — без змін";
  String s = FPSTR(PAGE_HEAD);
  s += "<header><h1>Light Watcher v" FW_VERSION "</h1><p>" + String(portal ? "Початкове налаштування" : "Налаштування") + " · ID " + deviceId + "</p></header><main>";
  s += "<form method='POST' action='" + String(portal ? "/save" : "/settings") + "'>";

  s += "<div class='card'><h2>📶 Wi-Fi</h2>";
  s += inputField("Назва мережі (SSID)", "ssid", cfg.ssid);
  s += inputField("Пароль", "pass", "", "password", portal ? "порожньо — відкрита мережа" : keep.c_str());
  s += "</div>";

  s += "<div class='card'><h2>🤖 Telegram</h2>";
  s += inputField("Bot Token", "token", portal ? cfg.botToken : String(""), "text", cfg.botToken.length() ? "збережено — порожньо = без змін" : "123456:ABC...");
  s += inputField("Власник (Chat ID)", "owner", cfg.chatId, "text", "напишіть боту /id");
  s += inputField("Група (Group ID, необов'язково)", "group", cfg.groupId, "text", "-100...");
  s += "</div>";

  s += "<div class='card'><h2>⚙️ Пристрій</h2>";
  s += inputField("Назва (у повідомленнях)", "name", cfg.name);
  s += inputField("mDNS ім'я (http://ім'я.local)", "host", cfg.host);
  s += inputField("Часовий пояс (POSIX TZ)", "tz", cfg.tz);
  s += "<div class='row'><div>" + inputField("Поріг \"є світло\" (АЦП ≥)", "thrOn", String(cfg.thrOn), "number") + "</div><div>" +
       inputField("Поріг \"нема\" (АЦП <)", "thrOff", String(cfg.thrOff), "number") + "</div></div>";
  s += "<div class='hint'>Поточне АЦП: " + String(lastAdc) + "</div>";
  s += inputField("Антидребезг, мс", "debounce", String(cfg.debounceMs), "number");
  s += "<div class='row'><div>" + inputField("Тихі години з (0-23, порожньо = вимк.)", "qFrom", cfg.quietFrom >= 0 ? String(cfg.quietFrom) : String(""), "number") +
       "</div><div>" + inputField("до", "qTo", cfg.quietTo >= 0 ? String(cfg.quietTo) : String(""), "number") + "</div></div>";
  s += "</div>";

  s += "<div class='card'><h2>🔋 Акумулятор</h2>";
  s += "<div class='row'><div>" + inputField("GPIO дільника (порожньо = вимк.)", "batPin", cfg.batPin >= 0 ? String(cfg.batPin) : String(""), "number") +
       "</div><div>" + inputField("Коефіцієнт дільника", "batDiv", String(cfg.batDiv, 3)) + "</div></div>";
  s += "<div class='row'><div>" + inputField("Li-ion банок (1-4)", "batCells", String(cfg.batCells), "number") +
       "</div><div>" + inputField("Попередження, %", "batLow", String(cfg.batLow), "number") + "</div></div>";
  if (batEnabled()) s += "<div class='hint'>Зараз: " + String(batV, 2) + " В, " + String(batPct()) + "%</div>";
  s += "</div>";

  s += "<div class='card'><h2>💬 Шаблони повідомлень</h2>";
  s += "<label>Світло вимкнули</label><textarea name='msgOff'>" + htmlEscape(cfg.msgOff) + "</textarea>";
  s += "<label>Світло з'явилося</label><textarea name='msgOn'>" + htmlEscape(cfg.msgOn) + "</textarea>";
  s += "<div class='hint'>Змінні: {time} {date} {datetime} {duration} {off_time} {count} {name} {battery}. HTML Telegram: &lt;b&gt;, &lt;i&gt;.</div></div>";

  s += "<div class='card'><h2>🔗 Інтеграції</h2>";
  s += inputField("Webhook URL (POST JSON)", "hook", cfg.webhookUrl, "url", "https://... (n8n, Make, Zapier, власний сервер)");
  s += "<div class='row'><div>" + inputField("MQTT сервер", "mqHost", cfg.mqttHost, "text", "192.168.1.10") + "</div><div>" +
       inputField("Порт", "mqPort", String(cfg.mqttPort), "number") + "</div></div>";
  s += "<div class='row'><div>" + inputField("MQTT логін", "mqUser", cfg.mqttUser) + "</div><div>" +
       inputField("MQTT пароль", "mqPass", "", "password", "порожньо — без змін") + "</div></div>";
  s += inputField("MQTT базовий топік", "mqBase", cfg.mqttBase);
  s += inputField("Пульс на webhook, хв (0 = вимк.)", "hbMin", String(cfg.hbMin), "number");
  s += "<label><input type='checkbox' name='subsOpen' value='1' style='width:auto'" + String(cfg.subsOpen ? " checked" : "") +
       "> Відкрита підписка (/subscribe без схвалення власника)</label>";
  s += "</div>";

  if (!portal) {
    s += "<div class='card'><h2>🔑 Доступ</h2>" + inputField("Пароль веб-налаштувань / ключ API (логін admin)", "apiKey", "", "text", "порожньо — без змін") + "</div>";
  }
  s += "<button class='btn' type='submit'>💾 ЗБЕРЕГТИ</button></form>";
  if (!portal) s += "<p class='hint' style='text-align:center'><a href='/'>← До панелі</a></p>";
  s += "<p class='hint' style='text-align:center'>Based on <a href='https://github.com/Stanislav-developer/Light_Watcher'>Light Watcher</a> by Stanislav Turii &amp; Noctyon</p></main></body></html>";
  return s;
}

bool requireAuth() {
  if (server.header("X-Api-Key") == cfg.apiKey) return true;
  if (server.authenticate("admin", cfg.apiKey.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Light Watcher", "Unauthorized");
  return false;
}

void handleSettingsSave(bool portal) {
  if (!portal && !requireAuth()) return;
  bool needRestart = portal, mqttChanged = false;
  String err;

  // setS: обновить строковую настройку. keepEmpty — пустое значение означает "не менять".
  auto setS = [&](const char* arg, const char* key, String& field, bool keepEmpty, bool trimV, bool restart, bool isMqtt) {
    if (!server.hasArg(arg)) return;
    String v = server.arg(arg);
    if (trimV) v.trim();
    v.replace("\r\n", "\n");
    if (keepEmpty && v.isEmpty()) return;
    if (v == field) return;
    field = v; prefs.putString(key, v);
    if (restart) needRestart = true;
    if (isMqtt) mqttChanged = true;
  };

  setS("ssid",   "ssid",    cfg.ssid,     false, true,  true,  false);
  setS("pass",   "pass",    cfg.pass,     !portal, false, true, false);
  setS("token",  "token",   cfg.botToken, true,  true,  true,  false);
  setS("owner",  "chatId",  cfg.chatId,   false, true,  false, false);
  setS("group",  "groupId", cfg.groupId,  false, true,  false, false);
  setS("name",   "name",    cfg.name,     true,  true,  false, true);
  setS("host",   "host",    cfg.host,     true,  true,  true,  false);
  setS("tz",     "tz-rule", cfg.tz,       true,  true,  false, false);
  setS("msgOff", "msgOff",  cfg.msgOff,   true,  true,  false, false);
  setS("msgOn",  "msgOn",   cfg.msgOn,    true,  true,  false, false);
  setS("hook",   "hook",    cfg.webhookUrl, false, true, false, false);
  setS("mqHost", "mqHost",  cfg.mqttHost, false, true,  false, true);
  setS("mqUser", "mqUser",  cfg.mqttUser, false, true,  false, true);
  setS("mqPass", "mqPass",  cfg.mqttPass, true,  false, false, true);
  setS("mqBase", "mqBase",  cfg.mqttBase, true,  true,  false, true);
  if (!portal) setS("apiKey", "apiKey", cfg.apiKey, true, true, false, false);

  if (server.hasArg("thrOn") && server.hasArg("thrOff")) {
    int a = server.arg("thrOn").toInt(), b = server.arg("thrOff").toInt();
    if (a > 0 && a <= 4095 && b > 0 && b < a) {
      cfg.thrOn = a; cfg.thrOff = b;
      prefs.putUShort("thrOn", a); prefs.putUShort("thrOff", b);
    } else err += "Пороги АЦП невірні (потрібно 0 &lt; нема &lt; є ≤ 4095). ";
  }
  if (server.hasArg("debounce")) {
    long v = server.arg("debounce").toInt();
    if (v >= 0 && v <= 60000) { cfg.debounceMs = v; prefs.putUInt("debounce", v); }
  }
  if (server.hasArg("qFrom") && server.hasArg("qTo")) {
    String f = server.arg("qFrom"), t = server.arg("qTo");
    int a = f.length() ? f.toInt() : -1, b = t.length() ? t.toInt() : -1;
    if (a < 0 || b < 0 || a > 23 || b > 23 || a == b) a = b = -1;
    cfg.quietFrom = a; cfg.quietTo = b;
    prefs.putChar("qFrom", a); prefs.putChar("qTo", b);
  }
  if (server.hasArg("mqPort")) {
    uint16_t p = server.arg("mqPort").toInt();
    if (p && p != cfg.mqttPort) { cfg.mqttPort = p; prefs.putUShort("mqPort", p); mqttChanged = true; }
  }

  if (server.hasArg("batPin")) {
    String p = server.arg("batPin"); p.trim();
    int v = p.length() ? p.toInt() : -1;
    if (v < 0 || v > 4 || v == READ_PIN) v = -1;
    if (v != cfg.batPin) { cfg.batPin = v; batV = 0; prefs.putChar("batPin", v); mqttChanged = true; }
  }
  if (server.hasArg("batDiv")) {
    float v = server.arg("batDiv").toFloat();
    if (v >= 1 && v <= 20 && v != cfg.batDiv) { cfg.batDiv = v; batV = 0; prefs.putFloat("batDiv", v); }
  }
  if (server.hasArg("batCells")) { int v = server.arg("batCells").toInt(); if (v >= 1 && v <= 4) { cfg.batCells = v; prefs.putUChar("batCells", v); } }
  if (server.hasArg("batLow"))   { int v = server.arg("batLow").toInt();   if (v >= 5 && v <= 80) { cfg.batLow = v; prefs.putUChar("batLow", v); } }
  if (server.hasArg("hbMin")) {                     // поле есть в форме -> форма отправлена целиком
    long v = server.arg("hbMin").toInt();
    if (v >= 0 && v <= 1440) { cfg.hbMin = v; prefs.putUShort("hbMin", v); }
    cfg.subsOpen = server.hasArg("subsOpen");
    prefs.putBool("subsOpen", cfg.subsOpen);
  }

  if (cfg.ssid.isEmpty() || cfg.botToken.isEmpty() || cfg.chatId.isEmpty()) {
    server.send(400, "text/html; charset=utf-8", String(FPSTR(PAGE_HEAD)) +
      "<main><div class='card'><h2>⚠️ Заповніть Wi-Fi, Bot Token і Chat ID</h2><a href='/'>← Назад</a></div></main></body></html>");
    return;
  }

  setenv("TZ", cfg.tz.c_str(), 1); tzset();
  if (mqttChanged) { mqtt.disconnect(); mqttLastTry = 0; }
  if (needRestart) { prefs.putBool("cfgOk", false); restartAt = millis() + 2000; }

  String msg = String(FPSTR(PAGE_HEAD)) + "<main><div class='card'><h2>✅ Збережено</h2>";
  if (err.length()) msg += "<p style='color:var(--off)'>" + err + "</p>";
  msg += needRestart ? "<p>Пристрій перезавантажується… Цю сторінку можна закрити.</p>" : "<p><a href='/settings'>← Назад до налаштувань</a></p>";
  msg += "</div></main></body></html>";
  server.send(200, "text/html; charset=utf-8", msg);
}

// ============================================================================
// Веб: панель и API (режим клиента Wi-Fi)
// ============================================================================
static const char DASH_BODY[] PROGMEM = R"rawliteral(
<header><h1 id="nm">Light Watcher</h1><p id="sub">завантаження…</p></header><main>
<div id="big" class="big" style="background:#888">…</div>
<div class="card" id="kv"></div>
<div class="card"><h2>Останні відключення</h2><div id="hist"></div></div>
<p class="hint" style="text-align:center"><a href="/settings">⚙️ Налаштування</a> · <a href="/update">⬆️ Оновлення</a> · <a href="/api/status">/api/status</a> · <a href="/api/history">/api/history</a> · <a href="/metrics">/metrics</a></p>
</main><script>
function d(s){s=s|0;var h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h?h+' год '+m+' хв':m+' хв '+(s%60)+' с'}
function t(ts){if(!ts)return'—';var x=new Date(ts*1000);return x.toLocaleString('uk-UA',{day:'2-digit',month:'2-digit',hour:'2-digit',minute:'2-digit'})}
function kv(k,v){return '<div class="k"><span>'+k+'</span><b>'+v+'</b></div>'}
async function upd(){try{
 var s=await (await fetch('/api/status')).json();
 document.getElementById('nm').textContent=s.device;document.title=(s.power?'🟢 ':'🔴 ')+s.device;
 var b=document.getElementById('big');b.style.background=s.power?'var(--on)':'var(--off)';
 b.textContent=s.power?'СВІТЛО Є':'СВІТЛА НЕМАЄ';
 document.getElementById('sub').textContent=s.power?('з '+t(s.power_since)):('вже '+d(s.outage_duration_s)+' (з '+t(s.outage_started)+')');
 document.getElementById('kv').innerHTML=kv('Відключень всього',s.outages_total)+kv('Без світла за 24 год',d(s.downtime_24h_s))+
  kv('Без світла за 7 днів',d(s.downtime_7d_s))+kv('АЦП',s.adc+' ('+s.thr_on+'/'+s.thr_off+')')+kv('Wi-Fi',s.rssi+' dBm')+
  kv('MQTT',s.mqtt?'так':'ні')+(s.battery_pct!=null?kv('Резерв',s.battery_pct+'% · '+s.battery_v+' В'+(s.battery_eta_s?' · ~'+d(s.battery_eta_s):'')):'')+kv('Аптайм',d(s.uptime_s))+kv('Прошивка','v'+s.version);
 var h=await (await fetch('/api/history')).json(),o='';
 h.forEach(function(r){o+=kv(t(r.off)+' → '+t(r.on),d(r.duration_s))});
 document.getElementById('hist').innerHTML=o||'<span class="hint">Поки що немає</span>';
}catch(e){document.getElementById('sub').textContent='немає зв\'язку з пристроєм'}}
upd();setInterval(upd,5000);
</script></body></html>)rawliteral";

void sendJson(JsonDocument& d) {
  String s; serializeJson(d, s);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleApiStatus() { JsonDocument d; fillStatus(d); sendJson(d); }

void handleApiHistory() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (uint8_t i = 0; i < histCount; i++) {
    const OutageRec& r = histAt(i);
    JsonObject o = a.add<JsonObject>();
    o["off"] = r.off; o["on"] = r.on; o["duration_s"] = r.on - r.off;
    o["off_iso"] = isoTs(r.off); o["on_iso"] = isoTs(r.on);
  }
  sendJson(d);
}

void handleMetrics() {
  String n = cfg.name; n.replace("\"", "'");
  String l = "{device=\"" + n + "\",id=\"" + deviceId + "\"}";
  String s;
  s += "# HELP lightwatcher_power 1 if mains power present\n# TYPE lightwatcher_power gauge\n";
  s += "lightwatcher_power" + l + " " + String(powerOn ? 1 : 0) + "\n";
  s += "# TYPE lightwatcher_outages_total counter\nlightwatcher_outages_total" + l + " " + String(outageCount) + "\n";
  s += "# TYPE lightwatcher_downtime_seconds_total counter\nlightwatcher_downtime_seconds_total" + l + " " + String(totalDownSec + currentOutageSec()) + "\n";
  s += "# TYPE lightwatcher_current_outage_seconds gauge\nlightwatcher_current_outage_seconds" + l + " " + String(currentOutageSec()) + "\n";
  s += "# TYPE lightwatcher_adc gauge\nlightwatcher_adc" + l + " " + String(lastAdc) + "\n";
  s += "# TYPE lightwatcher_wifi_rssi_dbm gauge\nlightwatcher_wifi_rssi_dbm" + l + " " + String(WiFi.RSSI()) + "\n";
  s += "# TYPE lightwatcher_uptime_seconds gauge\nlightwatcher_uptime_seconds" + l + " " + String(millis() / 1000) + "\n";
  s += "# TYPE lightwatcher_outbox_queue gauge\nlightwatcher_outbox_queue" + l + " " + String(obCount) + "\n";
  s += "# TYPE lightwatcher_subscribers gauge\nlightwatcher_subscribers" + l + " " + String(subCount) + "\n";
  if (batEnabled() && batV > 0) {
    s += "# TYPE lightwatcher_battery_percent gauge\nlightwatcher_battery_percent" + l + " " + String(batPct()) + "\n";
    s += "# TYPE lightwatcher_battery_volts gauge\nlightwatcher_battery_volts" + l + " " + String(batV, 3) + "\n";
  }
  server.send(200, "text/plain; version=0.0.4", s);
}

void setupWeb() {
  const char* hdrs[] = { "X-Api-Key" };
  server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", String(FPSTR(PAGE_HEAD)) + FPSTR(DASH_BODY)); });
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/history", HTTP_GET, handleApiHistory);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.on("/health", HTTP_GET, []() { server.send(200, "text/plain", powerOn ? "ON" : "OFF"); });
  server.on("/settings", HTTP_GET, []() { if (requireAuth()) server.send(200, "text/html; charset=utf-8", settingsPage(false)); });
  server.on("/settings", HTTP_POST, []() { handleSettingsSave(false); });
  server.on("/api/test", HTTP_POST, []() {
    if (!requireAuth()) return;
    pushEvent(EV_TEST, 0, 0, defaultTargets());
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/restart", HTTP_POST, []() {
    if (!requireAuth()) return;
    server.send(200, "application/json", "{\"ok\":true}");
    restartAt = millis() + 1000;
  });
  httpUpdater.setup(&server, "/update", "admin", cfg.apiKey);   // веб-OTA: загрузка .bin через браузер
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ============================================================================
// Портал первичной настройки (точка доступа + captive portal)
// timeoutMs = 0 — ждать бесконечно (конфигурации нет совсем)
// ============================================================================
void launchPortal(uint32_t timeoutMs) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME);
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", settingsPage(true)); });
  server.on("/save", HTTP_POST, []() { handleSettingsSave(true); });
  server.onNotFound([]() { server.send(200, "text/html; charset=utf-8", settingsPage(true)); });
  server.begin();
  Serial.println("Портал настройки: Wi-Fi \"" AP_NAME "\", http://" + WiFi.softAPIP().toString());
  uint32_t start = millis();
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (restartAt && due(restartAt)) ESP.restart();
    if (timeoutMs && millis() - start > timeoutMs && WiFi.softAPgetStationNum() == 0) {
      Serial.println("Портал: таймаут, перезапуск");
      ESP.restart();
    }
    delay(5);
  }
}

// ============================================================================
// setup / loop
// ============================================================================
void setup() {
  pinMode(READ_PIN, INPUT);
  analogSetAttenuation(ADC_11db);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  Serial.println("\nLight Watcher v" FW_VERSION);

  uint64_t mac = ESP.getEfuseMac();
  char idb[8]; snprintf(idb, sizeof(idb), "%06lx", (unsigned long)((mac >> 24) & 0xFFFFFF));
  deviceId = idb;

  prefs.begin("light-watcher");
  loadConfig();
  loadState();

  // 3 секунды на то, чтобы зажать BOOT для входа в настройку
  bool bootHeld = false;
  for (int i = 0; i < 30 && !bootHeld; i++) { bootHeld = !digitalRead(BOOT_PIN); delay(100); }

  bool hasConfig = cfg.ssid.length() && cfg.botToken.length() && cfg.chatId.length();
  if (!hasConfig) { Serial.println("Нет конфигурации -> портал"); launchPortal(0); }
  if (bootHeld)   { Serial.println("BOOT зажата -> портал");    launchPortal(10UL * 60 * 1000); }

  // Wi-Fi
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.host.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  Serial.print("WiFi");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? String(" OK " + WiFi.localIP().toString()) : String(" нет связи"));

  // Время: NTP + автоматический DST
  configTzTime(cfg.tz.c_str(), NTP1, NTP2, NTP3);
  for (int i = 0; i < 10 && !timeValid(); i++) delay(500);
  checkTimeSync();

  // Telegram
  tgClient.setInsecure();
  tgClient.setTimeout(2);
  bot.updateToken(cfg.botToken);
  bot.maxMessageLength = 3000;

  if (WiFi.status() == WL_CONNECTED) {
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) ok = bot.getMe();
    if (ok) {
      Serial.println("Telegram OK: @" + bot.userName);
      if (!cfgOk) { cfgOk = true; prefs.putBool("cfgOk", true); }
      setBotCommands();
    } else if (!cfgOk) {
      // Конфигурация НОВАЯ и ни разу не заработала — вероятно, неверный токен.
      Serial.println("Telegram недоступен, конфиг не проверен -> портал на 5 мин");
      launchPortal(5UL * 60 * 1000);
    } else {
      // Конфиг уже работал раньше: скорее всего, просто нет интернета. Продолжаем работу.
      Serial.println("Telegram недоступен, работаем дальше (очередь сообщений)");
    }
  } else if (!cfgOk) {
    Serial.println("WiFi не подключился, конфиг не проверен -> портал на 5 мин");
    launchPortal(5UL * 60 * 1000);
  }

  // Начальное состояние сети и аккумулятора
  sampleBattery();
  lastAdc = readAdcAvg();
  rawPower = lastAdc >= cfg.thrOff;        // между порогами считаем, что свет есть
  rawChangedAt = millis();
  uint8_t bootTargets = T_OWNER | (cfg.webhookUrl.length() ? T_HOOK : 0);

  if (rawPower) {
    powerOn = true;
    if (inOutage) {
      powerOn = false;
      onPowerRestored(true);               // свет дали, пока устройство было выключено
    } else {
      powerOnSince = nowTs();
      pushEvent(EV_BOOT, 1, 0, bootTargets);
    }
  } else {
    powerOn = false;
    pushEvent(EV_BOOT, 0, 0, bootTargets);
    if (!inOutage) onPowerLost(false);     // старт на резервном питании
  }

  // Сервисы локальной сети
  if (MDNS.begin(cfg.host.c_str())) MDNS.addService("http", "tcp", 80);
  setupWeb();
  // OTA из Arduino IDE по сети (порт 3232, пароль = ключ API)
  ArduinoOTA.setHostname(cfg.host.c_str());
  ArduinoOTA.setPassword(cfg.apiKey.c_str());
  ArduinoOTA.onStart([]() {
    if (mqtt.connected()) mqtt.publish((cfg.mqttBase + "/availability").c_str(), "offline", true);
  });
  ArduinoOTA.begin();
  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);

  // Не отвечаем на сообщения, пришедшие пока устройство было выключено
  if (WiFi.status() == WL_CONNECTED && bot.getUpdates(-1) > 0) bot.last_message_received = bot.messages[0].update_id;

  Serial.println("Готово. Панель: http://" + WiFi.localIP().toString() + "  (http://" + cfg.host + ".local)");
}

void loop() {
  samplePower();
  if (rawPower != powerOn && millis() - rawChangedAt >= cfg.debounceMs) {
    if (rawPower) onPowerRestored(false);
    else onPowerLost(true);
  }

  sampleBattery();
  checkTimeSync();
  wifiWatchdog();
  server.handleClient();
  ArduinoOTA.handle();
  flushOutbox();
  mqttLoop();
  heartbeatLoop();
  handleTelegram();

  if (restartAt && due(restartAt)) {
    if (mqtt.connected()) mqtt.publish((cfg.mqttBase + "/availability").c_str(), "offline", true);
    delay(200);
    ESP.restart();
  }
  delay(2);
}

/*
ИСТОРИЯ ВЕРСИЙ:
v1.0 (25.01.2026) - Базовый функционал.
v1.5 (13.02.2026) - WEB интерфейс, отложенные сообщения.
v1.6 (20.10.2026) - Новый дизайн WEB, оптимизация, debug в Serial.
v2.0 (14.11.2026) - Интеграции (HTTP API, Prometheus, Webhook, MQTT/Home Assistant),
                    очередь уведомлений, гистерезис+антидребезг, история и статистика,
                    кастомизация (шаблоны, пороги, тихие часы, имя), авто-DST,
                    исправлены ошибки v1.6 (см. README.md).
v2.1 (14.11.2026) - Мониторинг аккумулятора с прогнозом, подписка жильцов,
                    heartbeat для сервера-агрегатора, OTA (веб, IDE, Telegram).
*/
