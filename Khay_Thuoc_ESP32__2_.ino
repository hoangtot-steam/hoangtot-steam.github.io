#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <RTClib.h>
#include <esp_sntp.h>
#include <time.h>
#include <atomic>

const char* WIFI_SSID = "khay thuoc";//wifi nha
const char* WIFI_PASS = "khaythuoc2026";//mk wifi nha


const char* AP_SSID = "Khay Thuoc";//wifi dự phòng
const char* AP_PASS = "khaythuoc2026";//mk wifi dự phòng


constexpr uint8_t LCD_ADDRESS = 0x27;
constexpr uint8_t LED[] = {25, 26, 27, 32};
constexpr uint8_t SLOT_COUNT = sizeof(LED) / sizeof(LED[0]);
constexpr uint8_t BUZZER = 2;
constexpr uint8_t CONFIRM_BUTTON = 15;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
constexpr uint8_t BUZZER_ON = HIGH;
constexpr uint8_t BUZZER_OFF = LOW;
constexpr uint32_t BUZZER_MS = 300;
constexpr uint32_t TEST_DURATION_MS = 5000;
constexpr uint32_t WIFI_RETRY_MS = 20000;
constexpr uint32_t NTP_FRESH_MS = 7200000;
constexpr long VN_OFFSET = 7L * 3600;

WebServer server(80);
LiquidCrystal_I2C lcd(LCD_ADDRESS, 20, 4);
RTC_DS3231 rtc;
Preferences prefs;
const char* names[] = {"Sang", "Trua", "Chieu", "Toi"};
const char* scheduleKeys[] = {"lich0", "lich1", "lich2"};
const char* reminderKeys[] = {"luot0", "luot1", "luot2", "luot3"};
uint16_t schedule[SLOT_COUNT] = {841, 1441, 2041, 2521}; // Phut trong ngay * 2 + bit bat lich
int testSlot = -1;
uint32_t testStartedAt = 0;
uint32_t reminder[SLOT_COUNT] = {}; // YYYYMMDD * 2 + bit da xac nhan
bool storageOK, rtcFound, rtcReady, apOn, wifiOn, ntpReady, everSynced;
uint32_t lastWifiTry, lastSync, lastClockUpdate, today;
std::atomic<bool> newSync{false};
DateTime now;
const char* clockSource = "Chưa đồng bộ";

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="vi"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Khay thuốc thông minh</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#edf5f0;color:#193d32;font:16px system-ui,sans-serif}
main{max-width:500px;margin:auto;padding:16px}header,article{border-radius:16px;padding:16px}
header{background:#185540;color:white}h1{font-size:24px;margin:0 0 8px}header p{margin:4px 0}
article{background:white;border:1px solid #dce8e1;margin:12px 0}article.active{background:#fff4cd;border-color:#e4bd55}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}
h2{margin:0 0 4px;font-size:19px}.controls{margin-top:12px}.status{font-size:14px}
input,button{font:inherit;accent-color:#185540}input[type=time],button{border:1px solid #c7d9ce;border-radius:9px;background:#fff;color:inherit;padding:8px;min-height:44px}
.save{width:100%;background:#185540;color:white;font-weight:700}button:disabled{opacity:.55}
</style>
<main><header><h1>Khay thuốc thông minh</h1><p id="clock">Đang kết nối…</p><p id="source"></p></header>
<form id="form"><div id="slots"></div><button class="save" disabled>Lưu lịch</button></form>
<p id="message" role="status"></p></main>
<script>
const form = document.querySelector('#form');
const clock = document.querySelector('#clock');
const source = document.querySelector('#source');
const message = document.querySelector('#message');
const save = form.querySelector('.save');
let editing = false, busy = false, loaded = false, reading = false, version = 0;
const cards = ['Sáng','Trưa','Chiều','Tối'].map((name, i) => {
  const card = document.createElement('article');
  card.innerHTML = '<div class="row"><div><h2>'+name+'</h2><div class="status"></div></div>'+
    '<label><input type="checkbox"> Bật lịch</label></div>'+
    '<div class="row controls"><label>Giờ nhắc <input type="time" required></label>'+
    '<button type="button" hidden>Xác nhận</button><button type="button" class="test">Test 5 giây</button></div>';
  document.querySelector('#slots').append(card);
  const confirm = card.querySelector('button');
  confirm.onclick = () => send('/api/confirm', {i, day:confirm.dataset.day}, 'Đã xác nhận ngăn '+name+'.');
  card.querySelector('.test').onclick = () => send('/api/test', {i}, 'Đã bật thử LED và còi ngăn '+name+' trong 5 giây.');
  return {card, confirm, enabled:card.querySelector('[type=checkbox]'),
    time:card.querySelector('[type=time]'), status:card.querySelector('.status')};
});
function render(data, updateInputs) {
  clock.textContent = data.clock;
  source.textContent = 'Nguồn giờ: '+data.source;
  data.slots.forEach((slot, i) => {
    const ui = cards[i];
    if (updateInputs) {
      ui.time.value = slot.time;
      ui.enabled.checked = slot.enabled;
    }
    ui.card.classList.toggle('active', slot.active || slot.testing);
    ui.status.textContent = slot.active ? 'Đang nhắc' : slot.testing ? 'Đang kiểm tra' : !slot.enabled ? 'Đã tắt lịch'
      : !data.day ? 'Chưa có giờ' : slot.confirmed ? 'Đã xác nhận' : 'Đang chờ';
    ui.confirm.hidden = !slot.active;
    ui.confirm.dataset.day = slot.day;
  });
  loaded = true;
  save.disabled = false;
}
async function refresh() {
  if (busy || reading) return;
  reading = true;
  const snapshot = version;
  try {
    const response = await fetch('/api/status', {cache:'no-store', signal:AbortSignal.timeout(2500)});
    if (!response.ok) throw Error();
    const data = await response.json();
    if (snapshot === version) render(data, !loaded || (!editing && !form.contains(document.activeElement)));
  } catch {
    if (snapshot === version) {
      clock.textContent = 'Đang kết nối lại với ESP32…';
      source.textContent = '';
    }
  } finally { reading = false; }
}
async function send(url, values, success) {
  if (busy || !loaded) return;
  busy = true;
  ++version;
  Array.from(form.elements).forEach(input => input.disabled = true);
  try {
    const response = await fetch(url, {method:'POST', body:new URLSearchParams(values), signal:AbortSignal.timeout(5000)});
    if (!response.ok) throw Error(await response.text());
    if (url === '/api/schedule') editing = false;
    render(await response.json(), !editing);
    message.textContent = success;
  } catch (error) { message.textContent = error.message || 'Không kết nối được với ESP32.'; }
  finally {
    Array.from(form.elements).forEach(input => input.disabled = false);
    busy = false;
  }
}
form.oninput = () => {
  editing = true;
  message.textContent = 'Bấm Lưu lịch để áp dụng thay đổi.';
};
form.onsubmit = event => {
  event.preventDefault();
  const values = {};
  cards.forEach((ui, i) => {
    values['t'+i] = ui.time.value;
    values['e'+i] = ui.enabled.checked ? '1' : '0';
  });
  send('/api/schedule', values, 'Đã lưu lịch.');
};
refresh();
setInterval(refresh, 500);
document.addEventListener('visibilitychange', () => { if (!document.hidden) refresh(); });
</script></html>
)HTML";

bool isEnabled(uint8_t i) { return schedule[i] & 1; }
uint16_t minuteOfSlot(uint8_t i) { return schedule[i] / 2; }
uint16_t currentMinute() { return now.hour() * 60 + now.minute(); }
bool isActive(uint8_t i) {
  return isEnabled(i) && reminder[i] != 0 && !(reminder[i] & 1);
}

void updateOutputs() {
  static bool anyWasActive = false;
  static bool buzzerSounding = false;
  static uint32_t lastToggle = 0;
  const uint32_t ms = millis();
  if (testSlot >= 0 && ms - testStartedAt >= TEST_DURATION_MS) testSlot = -1;
  bool anyActive = false;
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    const bool active = isActive(i) || testSlot == i;
    digitalWrite(LED[i], active ? HIGH : LOW);
    anyActive |= active;
  }
  if (anyActive && !anyWasActive) {
    buzzerSounding = true;
    lastToggle = ms;
  } else if (anyActive && ms - lastToggle >= BUZZER_MS) {
    buzzerSounding = !buzzerSounding;
    lastToggle = ms;
  } else if (!anyActive) {
    buzzerSounding = false;
  }
  anyWasActive = anyActive;
  digitalWrite(BUZZER, buzzerSounding ? BUZZER_ON : BUZZER_OFF);
}

void receivedNtp(struct timeval*) { newSync.store(true); }

void updateClock() {
  const time_t epoch = time(nullptr);
  const bool valid = epoch >= 1704067200LL && epoch < 4102420000LL;
  const uint32_t ms = millis();
  if (newSync.exchange(false) && valid) {
    lastSync = ms;
    everSynced = true;
    ntpReady = wifiOn;
    if (rtcFound) {
      rtc.adjust(DateTime(static_cast<uint32_t>(epoch + VN_OFFSET)));
      rtcReady = true;
    }
  }

  today = 0;
  clockSource = "Chưa đồng bộ";
  if (ntpReady && wifiOn && valid && ms - lastSync < NTP_FRESH_MS) {
    now = DateTime(static_cast<uint32_t>(epoch + VN_OFFSET));
    clockSource = "NTP";
  } else if (rtcReady) {
    now = rtc.now();
    if (!now.isValid() || now.year() < 2024 || now.year() >= 2100) return;
    clockSource = "DS3231";
  } else if (everSynced && valid) {
    now = DateTime(static_cast<uint32_t>(epoch + VN_OFFSET));
    clockSource = "Giờ ESP32";
  } else {
    return;
  }
  today = now.year() * 10000UL + now.month() * 100UL + now.day();
}

void checkSchedule() {
  if (!today) return;
  const uint16_t minute = currentMinute();
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    if (isEnabled(i) && minute == minuteOfSlot(i) && reminder[i] / 2 < today) {
      reminder[i] = today * 2;
      if (storageOK) prefs.putUInt(reminderKeys[i], reminder[i]);
    }
  }
}

void formatTime(uint16_t minute, char* output) {
  snprintf(output, 6, "%02u:%02u", unsigned(minute / 60), unsigned(minute % 60));
}

void lcdLine(uint8_t row, const char* value) {
  static char displayed[4][21] = {};
  char padded[21];
  snprintf(padded, sizeof(padded), "%-20.20s", value);
  if (strcmp(displayed[row], padded) == 0) return;
  strcpy(displayed[row], padded);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

void updateLcd() {
  if (!today) {
    lcdLine(0, "Khay thuoc ESP32");
    lcdLine(1, "Chua co ngay gio");
    lcdLine(2, testSlot >= 0 ? "Dang test LED + coi" : "Cho WiFi dong bo");
    lcdLine(3, "Kiem tra DS3231");
    return;
  }
  char line[32];
  snprintf(line, sizeof(line), "Ngay: %02d/%02d/%04d", now.day(), now.month(), now.year());
  lcdLine(0, line);
  snprintf(line, sizeof(line), "Gio: %02d:%02d:%02d", now.hour(), now.minute(), now.second());
  lcdLine(1, line);

  int next = -1, tomorrow = -1, activeIndex = -1;
  String activeLabels;
  const char* shortNames[] = {"S", "T", "C", "To"};
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    if (!isEnabled(i)) continue;
    if (isActive(i)) {
      if (activeLabels.length()) activeLabels += ',';
      activeLabels += shortNames[i];
      activeIndex = i;
    }
    if (tomorrow < 0 || minuteOfSlot(i) < minuteOfSlot(tomorrow)) tomorrow = i;
    if (minuteOfSlot(i) > currentMinute() && reminder[i] / 2 < today &&
        (next < 0 || minuteOfSlot(i) < minuteOfSlot(next))) next = i;
  }
  if (activeIndex >= 0) {
    snprintf(line, sizeof(line), "Dang nhac: %s",
             activeLabels.indexOf(',') < 0 ? names[activeIndex] : activeLabels.c_str());
    lcdLine(2, line);
  } else if (testSlot >= 0) {
    snprintf(line, sizeof(line), "Test: %s", names[testSlot]);
    lcdLine(2, line);
  } else {
    lcdLine(2, "Trang thai: Dang cho");
  }

  const int slot = next >= 0 ? next : tomorrow;
  if (slot < 0) {
    lcdLine(3, "Khong co lich nhac");
    return;
  }
  char hour[6];
  formatTime(minuteOfSlot(slot), hour);
  snprintf(line, sizeof(line), "%s: %s %s", next >= 0 ? "Tiep" : "Mai", names[slot], hour);
  lcdLine(3, line);
}

void sendStatus() {
  char timeText[32] = "Chưa có ngày giờ";
  if (today) {
    snprintf(timeText, sizeof(timeText), "%02d/%02d/%04d  %02d:%02d:%02d",
             now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
  }
  String json;
  json.reserve(540);
  json = "{\"clock\":\"";
  json += timeText;
  json += "\",\"source\":\"";
  json += clockSource;
  json += "\",\"day\":";
  json += today;
  json += ",\"slots\":[";
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    if (i) json += ',';
    char hour[6];
    formatTime(minuteOfSlot(i), hour);
    json += "{\"time\":\"";
    json += hour;
    json += "\",\"enabled\":";
    json += isEnabled(i) ? "true" : "false";
    json += ",\"active\":";
    json += isActive(i) ? "true" : "false";
    json += ",\"testing\":";
    json += testSlot == i ? "true" : "false";
    json += ",\"day\":";
    json += reminder[i] / 2;
    json += ",\"confirmed\":";
    json += (today && reminder[i] == today * 2 + 1) ? "true" : "false";
    json += '}';
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}

bool parseTime(const String& input, uint16_t& minute) {
  if (input.length() != 5 || input[2] != ':') return false;
  for (uint8_t i = 0; i < 5; ++i) {
    if (i != 2 && (input[i] < '0' || input[i] > '9')) return false;
  }
  const int hours = (input[0] - '0') * 10 + input[1] - '0';
  const int minutes = (input[3] - '0') * 10 + input[4] - '0';
  if (hours >= 24 || minutes >= 60) return false;
  minute = hours * 60 + minutes;
  return true;
}

void saveSchedule() {
  uint16_t values[SLOT_COUNT];
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    const String enabled = server.arg(String("e") + i);
    uint16_t minute;
    if (!parseTime(server.arg(String("t") + i), minute) ||
        (enabled != "0" && enabled != "1")) {
      server.send(400, "text/plain; charset=utf-8", "Giờ hoặc công tắc không hợp lệ.");
      return;
    }
    values[i] = minute * 2 + (enabled == "1");
  }
  if (!storageOK ||
      (memcmp(values, schedule, sizeof(schedule)) != 0 &&
       prefs.putBytes("schedule", values, sizeof(values)) != sizeof(values))) {
    server.send(500, "text/plain; charset=utf-8", "Chưa lưu được lịch.");
    return;
  }
  memcpy(schedule, values, sizeof(schedule));
  checkSchedule();
  updateOutputs();
  sendStatus();
  updateLcd();
}

int requestedSlot() {
  const String index = server.arg("i");
  if (index.length() != 1 || index[0] < '0' || index[0] >= '0' + SLOT_COUNT) {
    server.send(400, "text/plain; charset=utf-8", "Ngăn thuốc không hợp lệ.");
    return -1;
  }
  return index[0] - '0';
}

void testReminder() {
  const int slot = requestedSlot();
  if (slot < 0) return;
  testSlot = slot;
  testStartedAt = millis();
  updateOutputs();
  sendStatus();
  updateLcd();
}

bool saveConfirmation(uint8_t slot) {
  const uint32_t confirmed = reminder[slot] | 1;
  if (!storageOK || prefs.putUInt(reminderKeys[slot], confirmed) != sizeof(confirmed)) {
    return false;
  }
  reminder[slot] = confirmed;
  updateOutputs();
  return true;
}

void updateConfirmButton() {
  const bool pressed = digitalRead(CONFIRM_BUTTON) == LOW;
  const uint32_t ms = millis();
  static bool lastReading = pressed;
  static bool stableReading = pressed;
  static uint32_t changedAt = ms;

  if (pressed != lastReading) {
    lastReading = pressed;
    changedAt = ms;
  }
  if (pressed == stableReading || ms - changedAt < BUTTON_DEBOUNCE_MS) return;

  stableReading = pressed;
  if (!pressed) return;

  int slot = -1;
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    if (!isActive(i)) continue;
    if (slot < 0 || reminder[i] < reminder[slot] ||
        (reminder[i] == reminder[slot] && minuteOfSlot(i) < minuteOfSlot(slot))) {
      slot = i;
    }
  }
  if (slot < 0) {
    if (testSlot < 0) return;
    testSlot = -1;
    updateOutputs();
  } else if (!saveConfirmation(slot)) {
    Serial.println("Chua luu duoc xac nhan tu nut nhan");
    return;
  }
  updateLcd();
}

void confirmReminder() {
  const int i = requestedSlot();
  if (i < 0) return;
  if (!isActive(i) || server.arg("day") != String(reminder[i] / 2)) {
    server.send(409, "text/plain; charset=utf-8", "Lượt nhắc đã thay đổi.");
    return;
  }
  if (!saveConfirmation(i)) {
    server.send(500, "text/plain; charset=utf-8", "Chưa lưu được xác nhận.");
    return;
  }
  sendStatus();
  updateLcd();
}

void startBackupWifi() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  apOn = WiFi.softAP(AP_SSID, AP_PASS);
  if (apOn) {
    Serial.print("IP WiFi Khay Thuoc: ");
    Serial.println(WiFi.softAPIP());
  }
}

void updateWifi() {
  const uint32_t ms = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiOn) {
      wifiOn = true;
      ntpReady = false;
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      Serial.print("IP mang nha: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }
  if (wifiOn) {
    wifiOn = false;
    ntpReady = false;
    startBackupWifi();
  }
  if (ms - lastWifiTry >= WIFI_RETRY_MS) {
    lastWifiTry = ms;
    startBackupWifi();
    WiFi.reconnect();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(CONFIRM_BUTTON, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, BUZZER_OFF);
  for (uint8_t pin : LED) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  rtcFound = rtc.begin(&Wire);
  rtcReady = rtcFound && !rtc.lostPower();

  storageOK = prefs.begin("khaythuoc", false);
  if (storageOK) {
    uint16_t saved[SLOT_COUNT];
    const size_t legacySlots = sizeof(scheduleKeys) / sizeof(scheduleKeys[0]);
    const size_t savedSize = prefs.getBytesLength("schedule");
    size_t savedSlots = 0;
    if ((savedSize == sizeof(saved) || savedSize == legacySlots * sizeof(saved[0])) &&
        prefs.getBytes("schedule", saved, sizeof(saved)) == savedSize) {
      savedSlots = savedSize / sizeof(saved[0]);
    }
    for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
      uint16_t value = schedule[i];
      if (i < savedSlots) {
        value = saved[i];
      } else if (i < legacySlots) {
        value = prefs.getUShort(scheduleKeys[i], value);
      }
      if (value < 2880) schedule[i] = value;
      reminder[i] = prefs.getUInt(reminderKeys[i], 0);
    }
  }
  updateClock();
  checkSchedule();
  updateOutputs();
  updateLcd();

  sntp_set_time_sync_notification_cb(receivedNtp);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTry = millis();
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/schedule", HTTP_POST, saveSchedule);
  server.on("/api/confirm", HTTP_POST, confirmReminder);
  server.on("/api/test", HTTP_POST, testReminder);
  server.begin();
}

void loop() {
  updateWifi();
  const uint32_t ms = millis();
  const bool refreshClock = ms - lastClockUpdate >= 250;
  if (refreshClock) {
    lastClockUpdate = ms;
    updateClock();
    checkSchedule();
  }
  updateOutputs();
  updateConfirmButton();
  server.handleClient();
  if (refreshClock) updateLcd();
  delay(2);
}
