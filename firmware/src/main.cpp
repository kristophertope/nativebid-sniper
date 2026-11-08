#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include <memory>

#include "bot.h"
#include "nativebid.h"
#include "platform_esp32.h"
#include "telegram.h"

using namespace sniper;

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

struct Settings {
  String wifi_ssid, wifi_pass, nb_user, nb_pass, tg_token, tg_chat, tz;
};

static EspPlatform platform;
static Settings settings;
static std::unique_ptr<NativeBid> nb;
static std::unique_ptr<Telegram> tg;
static std::unique_ptr<Bot> bot;
static bool started = false;

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
    if (time(nullptr) > 1700000000) {
      platform.log("Clock set");
      return true;
    }
    delay(500);
  }
  Serial.println("Couldn't get the time from the internet");
  return false;
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
  while (!connect_wifi()) delay(5000);
  while (!sync_clock()) delay(5000);
  nb.reset(new NativeBid(platform, settings.nb_user.c_str(), settings.nb_pass.c_str()));
  tg.reset(new Telegram(platform, settings.tg_token.c_str(), atoll(settings.tg_chat.c_str())));
  bot.reset(new Bot(platform, *nb, *tg));
}

void loop() {
  if (!connect_wifi()) {
    platform.release_all();
    delay(5000);
    return;
  }
  if (!started) {
    started = bot->start();
    if (!started) delay(30000);
    return;
  }
  Result r = bot->tick();
  if (!r.ok()) {
    platform.log("Error: " + r.msg);
    platform.sleep(5);
  }
}
