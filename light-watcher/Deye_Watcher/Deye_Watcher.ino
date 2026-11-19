/* Deye Watcher v1.0 — мониторинг гибридного инвертора Deye через Wi-Fi логгер, уведомления в Telegram.

Что делает:
  * Каждые 10 с читает инвертор через штатный Wi-Fi логгер Solarman (LSW-3, порт 8899) по локальной сети.
    Облако Deye/Solarman продолжает работать, к инвертору ничего не подключается.
  * Шлёт в Telegram: пропала/появилась сеть (с длительностью), пропала одна фаза,
    заряд батареи упал до 50/30/20/10 %, авария инвертора, логгер не отвечает.
  * Команды бота: /status /grid /day /cap /minsoc /alerts /raw /help.
  * Локально: http://<IP>/api/status (JSON) — для Home Assistant, скриптов и т.п.

Инвертор: Deye трёхфазный HV гибрид (SUN-xxK-SG01HP3). Карта регистров взята из kellerza/sunsynk:
  https://github.com/kellerza/sunsynk/blob/main/src/sunsynk/definitions/three_phase_common.py
  https://github.com/kellerza/sunsynk/blob/main/src/sunsynk/definitions/three_phase_hv.py
Протокол логгера: https://github.com/jmccrohan/pysolarmanv5

Железо: любая плата ESP32 (ESP32-C3 SuperMini или ESP32 DevKit) + USB-блок 5 В.
Питать от резервного выхода инвертора, чтобы работало при отключениях (роутер — тоже от резерва).

Библиотеки (Arduino IDE → Library Manager): UniversalTelegramBot 1.3.0, ArduinoJson 7.x.
Плата: "ESP32C3 Dev Module" (USB CDC On Boot: Enabled) или "ESP32 Dev Module".

БЕЗОПАСНОСТЬ: порт 8899 логгера без пароля. Прошивка только ЧИТАЕТ регистры (функция 0x03),
ничего в инвертор не записывает.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include "deye_v5.h"

// ============================================================================
// НАСТРОЙКИ — заполните перед прошивкой
// ============================================================================
const char*    WIFI_SSID   = "ВАШ_WIFI";
const char*    WIFI_PASS   = "ПАРОЛЬ_WIFI";
const char*    BOT_TOKEN   = "123456:ABC...";      // токен от @BotFather
const char*    CHAT_ID     = "123456789";           // ваш ID (бот покажет его по команде /id)
const char*    GROUP_ID    = "";                    // ID группы (необязательно)

const char*    LOGGER_IP   = "192.168.1.50";        // IP логгера (резервируйте его в роутере!)
const uint32_t LOGGER_SN   = 2974513202UL;          // серийник ЛОГГЕРА (не инвертора)
const uint16_t LOGGER_PORT = 8899;
const uint8_t  MODBUS_ID   = 1;

const uint32_t POLL_MS     = 10000;                 // опрос инвертора, мс (не чаще 5000)
const bool     ENERGY_WHOLE_KWH = false;            // true, если прошивка MAIN >= 1098 (у вас 1066 → false)
const char*    TZ_INFO     = "EET-2EEST,M3.5.0/3,M10.5.0/4";  // Киев, авто летнее время

// ============================================================================
// Регистры (трёхфазный HV Deye)
// ============================================================================
struct Block { uint16_t start; uint16_t count; };
const Block BLOCKS[] = { {500, 60}, {586, 60}, {646, 60} };   // 3 запроса на весь снимок
const int NBLOCKS = sizeof(BLOCKS) / sizeof(BLOCKS[0]);
uint16_t regs[NBLOCKS][60];
bool regsValid = false;

int16_t s16(uint16_t v) { return (int16_t)v; }

// Значение регистра по адресу (0, если адрес вне прочитанных блоков)
uint16_t R(uint16_t addr) {
  for (int b = 0; b < NBLOCKS; b++)
    if (addr >= BLOCKS[b].start && addr < BLOCKS[b].start + BLOCKS[b].count) return regs[b][addr - BLOCKS[b].start];
  return 0;
}

struct Snapshot {
  int   state;          // 500: 0 standby, 1 self-check, 2 normal, 3 alarm, 4 fault
  int   soc;            // 588, %
  float battV;          // 587 ×0.1
  int   battW;          // 590 signed ×10 (+ разряд, − заряд)
  float battA;          // 591 signed ×0.01
  float battT;          // 586: raw×0.1 − 100 °C
  float gridV[3];       // 598..600 ×0.1
  float gridHz;         // 609 ×0.01
  int   gridW;          // 625 signed (+ импорт, − экспорт)
  int   loadW;          // 653 signed
  int   invW;           // 636 signed
  int   pvW;            // 672 + 673, ×10 у HV
  bool  gridRelay;      // 552 бит 2
  float dayPv, dayLoad, dayImp, dayExp, dayChg, dayDis;  // 529, 526, 520, 521, 514, 515
};
Snapshot S;

void decode() {
  float ek = ENERGY_WHOLE_KWH ? 1.0f : 0.1f;
  S.state  = R(500);
  S.soc    = R(588);
  S.battV  = R(587) * 0.1f;
  S.battW  = s16(R(590)) * 10;
  S.battA  = s16(R(591)) * 0.01f;
  S.battT  = R(586) * 0.1f - 100.0f;
  for (int i = 0; i < 3; i++) S.gridV[i] = R(598 + i) * 0.1f;
  S.gridHz = R(609) * 0.01f;
  S.gridW  = s16(R(625));
  S.loadW  = s16(R(653));
  S.invW   = s16(R(636));
  S.pvW    = (R(672) + R(673)) * 10;
  S.gridRelay = R(552) & (1 << 2);
  S.dayPv = R(529) * ek; S.dayLoad = R(526) * ek; S.dayImp = R(520) * ek;
  S.dayExp = R(521) * ek; S.dayChg = R(514) * ek; S.dayDis = R(515) * ek;
}

// ============================================================================
// Связь с логгером
// ============================================================================
WiFiClient logger;
uint8_t seqNo = 0;
String lastErr = "";

bool readExact(uint8_t* buf, size_t n, uint32_t deadline) {
  size_t got = 0;
  while (got < n) {
    if ((int32_t)(millis() - deadline) > 0) return false;
    int a = logger.available();
    if (a > 0) got += logger.read(buf + got, n - got);
    else delay(5);
  }
  return true;
}

bool readBlock(const Block& b, uint16_t* out) {
  if (!logger.connected()) {
    logger.stop();
    if (!logger.connect(LOGGER_IP, LOGGER_PORT, 3000)) { lastErr = "нет TCP-соединения с логгером"; return false; }
  }
  while (logger.available()) logger.read();     // выбрасываем старые данные

  uint8_t req[v5::REQ_LEN];
  seqNo = (uint8_t)(seqNo + 1); if (!seqNo) seqNo = 1;
  v5::buildRead(req, LOGGER_SN, seqNo, MODBUS_ID, b.start, b.count);
  logger.write(req, sizeof(req));

  static uint8_t f[v5::MAX_FRAME];
  uint32_t deadline = millis() + 6000;
  for (int attempt = 0; attempt < 4; attempt++) {
    // ищем начало кадра 0xA5
    do { if (!readExact(f, 1, deadline)) { lastErr = "таймаут ответа логгера"; return false; } } while (f[0] != 0xA5);
    if (!readExact(f + 1, 2, deadline)) { lastErr = "таймаут"; return false; }
    size_t n = v5::frameLen(f);
    if (n > sizeof(f) || n < 13) { lastErr = "неверная длина кадра"; return false; }
    if (!readExact(f + 3, n - 3, deadline)) { lastErr = "обрезанный кадр"; return false; }
    v5::Result r = v5::parseRead(f, n, LOGGER_SN, seqNo, MODBUS_ID, b.count, out);
    if (r == v5::OK) return true;
    if (r == v5::HEARTBEAT) continue;           // keep-alive логгера — читаем дальше
    lastErr = "ошибка разбора ответа (" + String((int)r) + ")";
    return false;
  }
  lastErr = "нет ответа, только keep-alive";
  return false;
}

bool pollInverter() {
  for (int b = 0; b < NBLOCKS; b++) {
    bool ok = readBlock(BLOCKS[b], regs[b]);
    if (!ok) { delay(300); ok = readBlock(BLOCKS[b], regs[b]); }   // одна повторная попытка
    if (!ok) { logger.stop(); return false; }
    delay(150);                                  // логгер не любит частые запросы подряд
  }
  regsValid = true;
  decode();
  return true;
}

// ============================================================================
// Telegram и настройки
// ============================================================================
WiFiClientSecure tls;
UniversalTelegramBot bot(BOT_TOKEN, tls);
Preferences prefs;
WebServer web(80);

float   capKwh  = 0;           // ёмкость батареи, кВт·ч (для прогноза), /cap
int     minSoc  = 10;          // минимальный разряд по настройкам инвертора, /minsoc
int     thr[4]  = {50, 30, 20, 10};
bool    thrSent[4] = {false, false, false, false};

bool timeOk() { return time(nullptr) > 1700000000; }
String hhmm(time_t t) {
  if (!t || !timeOk()) return "--:--";
  struct tm tmv; localtime_r(&t, &tmv);
  char b[8]; strftime(b, sizeof b, "%H:%M", &tmv); return String(b);
}
String dur(uint32_t s) {
  uint32_t h = s / 3600, m = (s % 3600) / 60;
  if (h) return String(h) + " ч " + String(m) + " мин";
  return String(m) + " мин";
}
String kw(int w) { return String(w / 1000.0f, 1) + " кВт"; }

void notify(const String& text) {
  if (WiFi.status() != WL_CONNECTED) return;
  bot.sendMessage(CHAT_ID, text, "HTML");
  if (strlen(GROUP_ID)) bot.sendMessage(GROUP_ID, text, "HTML");
}

// Прогноз работы от батареи
String etaLine() {
  if (capKwh <= 0) return "";
  if (S.battW <= 100) return "";
  float usable = (S.soc - minSoc) / 100.0f * capKwh * 1000.0f;
  if (usable <= 0) return "\n⏳ Батарея на минимуме";
  uint32_t sec = (uint32_t)(usable / S.battW * 3600.0f);
  return "\n⏳ Хватит примерно на " + dur(sec) + " (до " + String(minSoc) + "%)";
}

String gridLine() {
  return "L1 " + String(S.gridV[0], 0) + " · L2 " + String(S.gridV[1], 0) + " · L3 " + String(S.gridV[2], 0) + " В";
}

const char* stateName(int s) {
  switch (s) { case 0: return "ожидание"; case 1: return "самопроверка"; case 2: return "норма";
               case 3: return "⚠️ предупреждение"; case 4: return "🛑 АВАРИЯ"; default: return "?"; }
}

String statusText() {
  if (!regsValid) return "Данных ещё нет: " + lastErr;
  String s = "⚡ <b>Deye</b> — " + String(stateName(S.state)) + "\n\n";
  s += "🔋 Батарея: <b>" + String(S.soc) + "%</b>, " + String(S.battV, 1) + " В, ";
  if (S.battW > 50) s += "разряд " + kw(S.battW);
  else if (S.battW < -50) s += "заряд " + kw(-S.battW);
  else s += "покой";
  s += ", " + String(S.battT, 1) + " °C";
  s += etaLine() + "\n";
  s += "🏠 Нагрузка: " + kw(S.loadW) + "\n";
  s += "☀️ Солнце: " + kw(S.pvW) + "\n";
  s += "🔌 Сеть: " + gridLine() + ", " + String(S.gridHz, 2) + " Гц\n";
  s += "    " + String(S.gridW >= 0 ? "импорт " : "экспорт ") + kw(abs(S.gridW)) + ", реле сети " + String(S.gridRelay ? "вкл" : "выкл");
  return s;
}

String dayText() {
  if (!regsValid) return "Данных ещё нет";
  return "📅 <b>Сегодня</b>\n☀️ Солнце: " + String(S.dayPv, 1) + " кВт·ч\n🏠 Потребление: " + String(S.dayLoad, 1) +
         " кВт·ч\n🔌 Из сети: " + String(S.dayImp, 1) + " кВт·ч, в сеть: " + String(S.dayExp, 1) +
         " кВт·ч\n🔋 Заряд: " + String(S.dayChg, 1) + " кВт·ч, разряд: " + String(S.dayDis, 1) + " кВт·ч";
}

String rawText() {
  if (!regsValid) return "Данных ещё нет: " + lastErr;
  const uint16_t a[] = {500, 552, 586, 587, 588, 590, 591, 598, 599, 600, 609, 625, 636, 653, 672, 673, 514, 515, 520, 521, 526, 529};
  String s = "<code>";
  for (uint16_t x : a) s += String(x) + " = " + String(R(x)) + "\n";
  return s + "</code>";
}

void handleMessage(telegramMessage& m) {
  String text = m.text; text.trim();
  if (text == "/id" || text == "/start") { bot.sendMessage(m.chat_id, "Ваш chat ID: " + m.chat_id, ""); return; }
  if (m.chat_id != CHAT_ID && !(strlen(GROUP_ID) && m.chat_id == GROUP_ID)) return;   // чужие чаты игнорируем
  bool owner = m.chat_id == CHAT_ID;

  String cmd = text, args = "";
  int sp = text.indexOf(' ');
  if (sp > 0) { cmd = text.substring(0, sp); args = text.substring(sp + 1); args.trim(); }
  int at = cmd.indexOf('@'); if (at > 0) cmd = cmd.substring(0, at);

  String reply;
  if (cmd == "/status")    reply = statusText();
  else if (cmd == "/grid") reply = regsValid ? String("🔌 " + gridLine() + "\n🔋 " + String(S.soc) + "%" + etaLine()) : String("Данных ещё нет");
  else if (cmd == "/day")  reply = dayText();
  else if (!owner) return;
  else if (cmd == "/cap") {
    float v = args.toFloat();
    if (v > 0 && v < 1000) { capKwh = v; prefs.putFloat("cap", v); reply = "✅ Ёмкость батареи: " + String(v, 1) + " кВт·ч"; }
    else reply = "Формат: /cap 30  (ёмкость батареи в кВт·ч, для прогноза)";
  }
  else if (cmd == "/minsoc") {
    int v = args.toInt();
    if (v >= 0 && v < 90 && args.length()) { minSoc = v; prefs.putInt("minsoc", v); reply = "✅ Минимальный заряд: " + String(v) + "%"; }
    else reply = "Формат: /minsoc 10";
  }
  else if (cmd == "/alerts") {
    int v[4] = {0, 0, 0, 0}, n = 0, from = 0;
    while (n < 4 && from < (int)args.length()) {
      int e = args.indexOf(' ', from); if (e < 0) e = args.length();
      v[n++] = args.substring(from, e).toInt(); from = e + 1;
    }
    if (n > 0) {
      for (int i = 0; i < 4; i++) { thr[i] = i < n ? v[i] : 0; thrSent[i] = false; prefs.putInt(("thr" + String(i)).c_str(), thr[i]); }
    }
    reply = "🪫 Предупреждения о заряде: ";
    for (int i = 0; i < 4; i++) if (thr[i] > 0) reply += String(thr[i]) + "% ";
    reply += "\nИзменить: /alerts 50 30 20 10";
  }
  else if (cmd == "/raw")  reply = rawText();
  else reply = "Команды:\n/status — всё состояние\n/grid — сеть и батарея\n/day — энергия за сегодня\n"
               "/cap 30 — ёмкость батареи, кВт·ч (для прогноза)\n/minsoc 10 — минимальный заряд\n"
               "/alerts 50 30 20 10 — пороги предупреждений\n/raw — сырые регистры (проверка)\n/id — ваш chat ID";
  bot.sendMessage(m.chat_id, reply, "HTML");
}

void pollTelegram() {
  static uint32_t last = 0;
  if (millis() - last < 3000 || WiFi.status() != WL_CONNECTED) return;
  last = millis();
  for (int guard = 0; guard < 3; guard++) {
    int n = bot.getUpdates(bot.last_message_received + 1);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) handleMessage(bot.messages[i]);
  }
}

// ============================================================================
// Логика событий
// ============================================================================
enum GridState { GRID_UNKNOWN = -1, GRID_NONE = 0, GRID_PARTIAL = 1, GRID_OK = 2 };
GridState gridState = GRID_UNKNOWN, gridCandidate = GRID_UNKNOWN;
int candidateCount = 0;
time_t gridLostAt = 0;
int lastState = -1;
uint32_t lastOkMs = 0;
bool loggerDownSent = false;

GridState measureGrid() {
  int up = 0;
  for (int i = 0; i < 3; i++) if (S.gridV[i] > 170) up++;
  if (up == 3) return GRID_OK;
  if (up == 0) return GRID_NONE;
  return GRID_PARTIAL;
}

String missingPhases() {
  String s;
  for (int i = 0; i < 3; i++) if (S.gridV[i] <= 170) s += (s.length() ? ", L" : "L") + String(i + 1);
  return s;
}

void processEvents() {
  // --- сеть: меняем состояние только после 2 одинаковых замеров подряд (антидребезг) ---
  GridState g = measureGrid();
  if (g == gridCandidate) candidateCount++; else { gridCandidate = g; candidateCount = 1; }
  if (candidateCount >= 2 && g != gridState) {
    GridState prev = gridState;
    gridState = g;
    time_t now = time(nullptr);
    if (prev == GRID_UNKNOWN) {
      notify("▶️ Deye Watcher запущен.\n" + statusText());
      // если перезагрузились во время отключения — сохраняем исходное время начала
      if (g == GRID_NONE && !gridLostAt) { gridLostAt = now; prefs.putUInt("lostAt", (uint32_t)now); }
      if (g == GRID_OK && gridLostAt) { gridLostAt = 0; prefs.putUInt("lostAt", 0); }
    } else if (g == GRID_NONE) {
      gridLostAt = now; prefs.putUInt("lostAt", (uint32_t)now);
      notify("🔴 <b>Сеть пропала</b> " + hhmm(now) + "\n🔋 Батарея " + String(S.soc) + "%, нагрузка " + kw(S.loadW) + etaLine());
    } else if (g == GRID_PARTIAL) {
      notify("🟡 <b>Пропала фаза " + missingPhases() + "</b> " + hhmm(now) + "\n" + gridLine());
    } else if (g == GRID_OK) {
      String msg = "🟢 <b>Сеть появилась</b> " + hhmm(now);
      if (prev == GRID_NONE && gridLostAt && timeOk()) msg += "\n⏱ Не было " + dur((uint32_t)(now - gridLostAt));
      if (prev == GRID_PARTIAL) msg = "🟢 <b>Все фазы в норме</b> " + hhmm(now);
      msg += "\n🔋 Батарея " + String(S.soc) + "%";
      notify(msg);
      gridLostAt = 0; prefs.putUInt("lostAt", 0);
    }
  }

  // --- заряд батареи ---
  // если за один опрос пересечено несколько порогов — одно сообщение по самому низкому
  int crossed = -1;
  for (int i = 0; i < 4; i++) {
    if (thr[i] <= 0) continue;
    if (!thrSent[i] && S.soc <= thr[i] && S.battW > 0) {
      thrSent[i] = true;
      if (crossed < 0 || thr[i] < thr[crossed]) crossed = i;
    } else if (thrSent[i] && S.soc >= thr[i] + 5) {
      thrSent[i] = false;
    }
  }
  if (crossed >= 0)
    notify("🪫 <b>Батарея " + String(S.soc) + "%</b> (порог " + String(thr[crossed]) + "%), нагрузка " + kw(S.loadW) + etaLine());

  // --- авария / предупреждение инвертора ---
  if (lastState >= 0 && S.state != lastState && (S.state == 3 || S.state == 4))
    notify("⚠️ <b>Инвертор: " + String(stateName(S.state)) + "</b>. Проверьте приложение Solarman.");
  lastState = S.state;
}

// ============================================================================
// HTTP API в локальной сети
// ============================================================================
void handleApi() {
  JsonDocument d;
  d["ok"] = regsValid; d["error"] = lastErr; d["age_s"] = (millis() - lastOkMs) / 1000;
  d["state"] = S.state; d["soc"] = S.soc; d["battery_v"] = S.battV; d["battery_w"] = S.battW;
  d["battery_a"] = S.battA; d["battery_t"] = S.battT; d["load_w"] = S.loadW; d["pv_w"] = S.pvW;
  d["grid_w"] = S.gridW; d["grid_hz"] = S.gridHz; d["grid_relay"] = S.gridRelay; d["inverter_w"] = S.invW;
  JsonArray gv = d["grid_v"].to<JsonArray>(); for (int i = 0; i < 3; i++) gv.add(S.gridV[i]);
  d["grid"] = gridState == GRID_OK ? "ok" : gridState == GRID_PARTIAL ? "partial" : gridState == GRID_NONE ? "none" : "unknown";
  d["day_pv_kwh"] = S.dayPv; d["day_load_kwh"] = S.dayLoad;
  String s; serializeJson(d, s);
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.send(200, "application/json", s);
}

// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nDeye Watcher v1.0");

  prefs.begin("deye-watch");
  capKwh = prefs.getFloat("cap", 0);
  minSoc = prefs.getInt("minsoc", 10);
  for (int i = 0; i < 4; i++) thr[i] = prefs.getInt(("thr" + String(i)).c_str(), thr[i]);
  gridLostAt = prefs.getUInt("lostAt", 0);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? String(" Wi-Fi OK, IP " + WiFi.localIP().toString()) : String(" нет Wi-Fi"));

  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  tls.setInsecure();

  web.on("/api/status", handleApi);
  web.on("/", []() { web.send(200, "text/plain; charset=utf-8", statusText()); });
  web.begin();

  // не отвечаем на старые сообщения
  if (WiFi.status() == WL_CONNECTED && bot.getUpdates(-1) > 0) bot.last_message_received = bot.messages[0].update_id;
}

void loop() {
  web.handleClient();

  static uint32_t lastPoll = 0, lastWifi = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifi > 30000) { lastWifi = millis(); WiFi.reconnect(); }
    delay(50);
    return;
  }

  if (millis() - lastPoll >= POLL_MS || lastPoll == 0) {
    lastPoll = millis();
    if (pollInverter()) {
      lastOkMs = millis();
      if (loggerDownSent) { loggerDownSent = false; notify("✅ Связь с логгером восстановлена"); }
      processEvents();
      Serial.printf("SOC %d%%  load %d W  grid %.0f/%.0f/%.0f V\n", S.soc, S.loadW, S.gridV[0], S.gridV[1], S.gridV[2]);
    } else {
      Serial.println("Ошибка опроса: " + lastErr);
      if (!loggerDownSent && lastOkMs && millis() - lastOkMs > 180000) {
        loggerDownSent = true;
        notify("📵 Логгер не отвечает больше 3 мин: " + lastErr + "\nВозможно, пропал Wi-Fi у логгера или инвертор выключен.");
      }
    }
  }

  pollTelegram();
  delay(10);
}
