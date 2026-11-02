#include "TriviaSyncClient.h"

#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstring>

namespace {

constexpr const char* kManifestUrl =
    "https://github.com/ma-r-s/crossplay/releases/download/trivia-pack/pack.json";
constexpr const char* kReportUrl = "https://crossplay.ma-r-s.com/api/trivia";
constexpr size_t kManifestMaxBytes = 512;
constexpr uint32_t kRequestTimeoutMs = 30000;

struct ResponseBuffer {
  char* data;
  size_t capacity;
  size_t length = 0;
  bool overflow = false;
};

esp_err_t collectResponse(esp_http_client_event_t* event) {
  if (event == nullptr || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
  auto* response = static_cast<ResponseBuffer*>(event->user_data);
  if (response == nullptr) return ESP_OK;
  const size_t bytes = static_cast<size_t>(event->data_len);
  if (bytes >= response->capacity - response->length) {
    response->overflow = true;
    return ESP_FAIL;
  }
  std::memcpy(response->data + response->length, event->data, bytes);
  response->length += bytes;
  response->data[response->length] = '\0';
  return ESP_OK;
}

esp_http_client_handle_t createClient(const char* url, ResponseBuffer* response) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.timeout_ms = kRequestTimeoutMs;
  config.buffer_size = 1024;
  config.buffer_size_tx = 1024;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = response == nullptr ? nullptr : collectResponse;
  config.user_data = response;
  return esp_http_client_init(&config);
}

bool addCommonHeaders(esp_http_client_handle_t client) {
  return esp_http_client_set_header(client, "User-Agent", "CrossPoint-Trivia") == ESP_OK &&
         esp_http_client_set_header(client, "Accept", "application/json") == ESP_OK;
}

}  // namespace

bool trivia::fetchPublishedManifest(PackManifest& manifest) {
  manifest = {};
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("TRIVIA", "Cannot check pack updates while Wi-Fi is disconnected");
    return false;
  }

  auto responseData = makeUniqueNoThrow<char[]>(kManifestMaxBytes + 1);
  if (!responseData) {
    LOG_ERR("TRIVIA", "OOM: manifest response buffer");
    return false;
  }
  ResponseBuffer response{responseData.get(), kManifestMaxBytes + 1};
  esp_http_client_handle_t client = createClient(kManifestUrl, &response);
  if (client == nullptr) {
    LOG_ERR("TRIVIA", "Cannot create manifest HTTP client");
    return false;
  }

  int status = 0;
  const bool configured = addCommonHeaders(client);
  const esp_err_t request = configured ? esp_http_client_perform(client) : ESP_FAIL;
  if (request == ESP_OK) status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (request != ESP_OK || status != 200 || response.overflow || response.length == 0) {
    LOG_ERR("TRIVIA", "Manifest request failed (err=%s, status=%d, overflow=%d)",
            esp_err_to_name(request), status, response.overflow ? 1 : 0);
    return false;
  }
  manifest = parseManifest(response.data, response.length);
  if (!manifest.valid) {
    LOG_ERR("TRIVIA", "Published manifest is invalid");
    return false;
  }
  return true;
}

int trivia::submitReportBatch(const char* body, const size_t length) {
  if (body == nullptr || length == 0 || length > INT32_MAX) {
    LOG_ERR("TRIVIA", "Invalid report batch size");
    return -1;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("TRIVIA", "Cannot submit reports while Wi-Fi is disconnected");
    return -1;
  }

  esp_http_client_handle_t client = createClient(kReportUrl, nullptr);
  if (client == nullptr) {
    LOG_ERR("TRIVIA", "Cannot create report HTTP client");
    return -1;
  }

  int status = -1;
  const bool configured =
      addCommonHeaders(client) &&
      esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK &&
      esp_http_client_set_method(client, HTTP_METHOD_POST) == ESP_OK &&
      esp_http_client_set_post_field(client, body, static_cast<int>(length)) == ESP_OK;
  const esp_err_t request = configured ? esp_http_client_perform(client) : ESP_FAIL;
  if (request == ESP_OK) {
    status = esp_http_client_get_status_code(client);
  } else {
    LOG_ERR("TRIVIA", "Report request failed: %s", esp_err_to_name(request));
  }
  esp_http_client_cleanup(client);
  return status;
}
