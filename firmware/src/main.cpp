#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>

#include <memory>

#include "bot.h"
#include "nativebid.h"
#include "platform_esp32.h"
#include "telegram.h"

using namespace sniper;

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

constexpr uint32_t WATCHDOG_S = 60;
constexpr uint32_t WIFI_RETRY_MS = 60 * 1000;
constexpr uint32_t WIFI_GIVE_UP_MS = 10 * 60 * 1000;
constexpr uint32_t HEAP_REPORT_MS = 10 * 60 * 1000;
constexpr uint32_t WIFI_RESTART = 0x57494649;

RTC_NOINIT_ATTR static uint32_t restart_note;

struct Settings {
  String wifi_ssid, wifi_pass, nb_user, nb_pass, tg_token, tg_chat, tz;
};

static EspPlatform platform;
static Settings settings;
static std::unique_ptr<NativeBid> nb;
static std::unique_ptr<Telegram> tg;
static std::unique_ptr<Bot> bot;
static bool started = false;
static std::string boot_note;
static uint32_t offline_since = 0;
static uint32_t wifi_retried = 0;
static uint32_t heap_reported = 0;

static bool load_settings() {
  Preferences prefs;
  if (!prefs.begin("sniper", true)) return false;
  settings.wifi_ssid = prefs.getString("wifi_ssid", "");
  settings.wifi_pass = prefs.getString("wifi_pass", "");
  settings.nb_user = prefs.getString("nb_user", "");
  settings.nb_pass = prefs.getString("nb_pass", "");
  settings.tg_token = prefs.getString("tg_token", "");
  settings.tg_chat = prefs.getString("tg_chat", "");
  settings.tz = prefs.getString("tz", "UTC0");
  prefs.end();
  const char* names[] = {"wifi_ssid", "nb_user", "nb_pass", "tg_token", "tg_chat"};
  const String* values[] = {&settings.wifi_ssid, &settings.nb_user, &settings.nb_pass, &settings.tg_token,
                            &settings.tg_chat};
  bool ok = true;
  for (int i = 0; i < 5; i++) {
    if (values[i]->isEmpty()) {
      Serial.printf("Missing setting: %s\n", names[i]);
      ok = false;
    }
  }
  return ok;
}

static bool connect_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("Connecting to WiFi \"%s\"", settings.wifi_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(settings.wifi_ssid.c_str(), settings.wifi_pass.c_str());
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi didn't connect (status %d)\n", WiFi.status());
    return false;
  }
  Serial.printf("WiFi connected: %s, signal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

static bool sync_clock() {
  configTzTime(settings.tz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  for (int i = 0; i < 40; i++) {
    esp_task_wdt_reset();
    if (time(nullptr) > 1700000000) {
      platform.log("Clock set");
      return true;
    }
    delay(500);
  }
  Serial.println("Couldn't get the time from the internet");
  return false;
}

static std::string why_restarted() {
  esp_reset_reason_t reason = esp_reset_reason();
  bool wifi = reason == ESP_RST_SW && restart_note == WIFI_RESTART;
  restart_note = 0;
  switch (reason) {
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
      return "the watchdog caught a freeze";
    case ESP_RST_PANIC:
      return "a crash";
    case ESP_RST_BROWNOUT:
      return "a power dip";
    default:
      return wifi ? "WiFi was down for 10 minutes" : "";
  }
}

static void wait_with_watchdog(uint32_t ms) {
  for (uint32_t waited = 0; waited < ms; waited += 1000) {
    esp_task_wdt_reset();
    delay(ms - waited < 1000 ? ms - waited : 1000);
  }
}

static bool wifi_up() {
  if (WiFi.status() == WL_CONNECTED) {
    if (offline_since) {
      platform.log("WiFi back after " + std::to_string((millis() - offline_since) / 1000) + "s");
      offline_since = 0;
    }
    return true;
  }
  uint32_t t = millis();
  if (!offline_since) {
    offline_since = wifi_retried = t ? t : 1;
    platform.release_all();
    platform.log("WiFi dropped");
  }
  if (t - offline_since >= WIFI_GIVE_UP_MS) {
    platform.log("WiFi down too long; restarting");
    restart_note = WIFI_RESTART;
    Serial.flush();
    ESP.restart();
  }
  if (t - wifi_retried >= WIFI_RETRY_MS) {
    wifi_retried = t;
    platform.log("Rejoining WiFi");
    WiFi.disconnect();
    WiFi.begin(settings.wifi_ssid.c_str(), settings.wifi_pass.c_str());
  }
  return false;
}

static void report_heap() {
  if (heap_reported && millis() - heap_reported < HEAP_REPORT_MS) return;
  heap_reported = millis();
  uint32_t up = millis() / 60000;
  char line[48];
  snprintf(line, sizeof line, "Up %uh %02um", (unsigned)(up / 60), (unsigned)(up % 60));
  platform.log(line);
}

void setup() {
  Serial.begin(115200);
  pinMode(ACTIVITY_LED, OUTPUT);
  digitalWrite(ACTIVITY_LED, LOW);
  delay(300);
  Serial.println("\nNativeBid sniper starting");
  if (!load_settings()) {
    Serial.println("Settings are missing; flash the settings image and restart.");
    for (;;) delay(1000);
  }
  esp_task_wdt_init(WATCHDOG_S, true);
  esp_task_wdt_add(nullptr);
  boot_note = why_restarted();
  if (!boot_note.empty()) Serial.printf("Restarted after %s\n", boot_note.c_str());
  while (!connect_wifi()) wait_with_watchdog(5000);
  while (!sync_clock()) wait_with_watchdog(5000);
  nb.reset(new NativeBid(platform, settings.nb_user.c_str(), settings.nb_pass.c_str()));
  tg.reset(new Telegram(platform, settings.tg_token.c_str(), atoll(settings.tg_chat.c_str())));
  bot.reset(new Bot(platform, *nb, *tg));
}

void loop() {
  esp_task_wdt_reset();
  report_heap();
  if (!wifi_up()) {
    wait_with_watchdog(1000);
    return;
  }
  if (!started) {
    started = bot->start();
    if (!started) {
      wait_with_watchdog(30000);
      return;
    }
    if (!boot_note.empty()) bot->say("⚠️ Restarted after " + boot_note + ". Snipes were kept.");
    boot_note.clear();
    return;
  }
  Result r = bot->tick();
  if (!r.ok()) {
    platform.log("Error: " + r.msg);
    platform.sleep(5);
  }
}
