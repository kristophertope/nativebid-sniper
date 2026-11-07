#include "telegram.h"

#include "text.h"

namespace sniper {

Result Telegram::call(const char* method, const JsonDocument& params, JsonDocument& reply, int wait) {
  std::string body, out;
  serializeJson(params, body);
  Reply r = web_.text("POST", url_ + method, body, {{"Content-Type", "application/json"}}, out, 32768, wait);
  if (!r.ok) return Result::network(r.error);
  reply.clear();
  bool parsed = !deserializeJson(reply, out);
  if (r.status == 200 && parsed && reply["ok"] == true) return {};
  std::string reason = parsed ? (reply["description"] | "no details") : "no details";
  if (r.status == 409) p_.log("Telegram conflict: something else is reading this bot\xE2\x80\x99s messages");
  return Result::site(std::string("Telegram ") + method + " failed (HTTP " + std::to_string(r.status) + "): " + reason);
}

Result Telegram::send(const std::string& text, const Keyboard& kb, long long edit) {
  JsonDocument params, reply;
  params["chat_id"] = chat_id_;
  params["text"] = prefix(text, 4000);
  params["parse_mode"] = "HTML";
  params["link_preview_options"]["is_disabled"] = true;
  if (kb.set) {
    JsonArray rows = params["reply_markup"]["inline_keyboard"].to<JsonArray>();
    for (const auto& row : kb.rows) {
      JsonArray r = rows.add<JsonArray>();
      for (const auto& b : row) {
        JsonObject o = r.add<JsonObject>();
        o["text"] = b.first;
        o["callback_data"] = b.second;
      }
    }
  }
  if (!edit) return call("sendMessage", params, reply);
  params["message_id"] = edit;
  Result r = call("editMessageText", params, reply);
  if (!r.ok() && contains(r.msg, "not modified")) return {};
  return r;
}

Result Telegram::send_plain(const std::string& text) {
  JsonDocument params, reply;
  params["chat_id"] = chat_id_;
  params["text"] = prefix(text, 4000);
  return call("sendMessage", params, reply);
}

Result Telegram::answer(const std::string& query_id, const std::string& text, bool alert) {
  JsonDocument params, reply;
  params["callback_query_id"] = query_id;
  if (!text.empty()) {
    params["text"] = prefix(text, 200);
    params["show_alert"] = alert;
  }
  return call("answerCallbackQuery", params, reply);
}

Result Telegram::updates(long long offset, int wait, JsonDocument& reply) {
  JsonDocument params;
  params["timeout"] = wait;
  params["limit"] = 5;
  JsonArray allowed = params["allowed_updates"].to<JsonArray>();
  allowed.add("message");
  allowed.add("callback_query");
  if (offset >= 0) params["offset"] = offset;
  return call("getUpdates", params, reply, wait + 15);
}

}
