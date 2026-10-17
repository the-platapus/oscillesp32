// WiFi (AP, or join a network with AP fallback), HTTP UI and WebSocket
// streaming. The page lives at "/", waveforms and commands go over "/ws".
//
// All WebSocket sends run inside the HTTP server task (httpd_queue_work) so
// replies and broadcasts never interleave on a socket.

#include "web.h"
#include "settings.h"
#include "commands.h"
#include "config.h"
#include "webpage.h"
#include <stdlib.h>
#include <string>
#include <atomic>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"

static const char* TAG = "web";

static httpd_handle_t sServer = nullptr;
static esp_netif_t* sApNetif = nullptr;
static esp_netif_t* sStaNetif = nullptr;
static EventGroupHandle_t sWifiEvents;
static const int GOT_IP_BIT = BIT0;
static volatile bool sStaWanted = false;
static volatile bool sReconfig = false;
static volatile bool sIsAp = false;
static char sIp[20] = "0.0.0.0";
static char sSsid[33] = "";

// ---- clients ----
static const int MAX_CLIENTS = 6;
static int sClients[MAX_CLIENTS];
static int sClientCount = 0;
static portMUX_TYPE sClientMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<int> sInFlight{0};

// ---- frame hand-off from the display loop ----
static SemaphoreHandle_t sPubMutex;
static Processed sWebFrame;
static volatile bool sFrameFresh = false;

// ---- commands from browsers run in their own task (some take seconds) ----
struct CmdItem { int fd; char text[240]; };
static QueueHandle_t sCmdQueue;

const char* webIp() { return sIp; }
const char* webSsid() { return sSsid; }
bool webIsAp() { return sIsAp; }
int webClientCount() { return sClientCount; }
void webRequestReconfig() { sReconfig = true; }

void webPublish(const Processed& p) {
  if (sClientCount <= 0 || !sPubMutex) return;
  if (xSemaphoreTake(sPubMutex, 0) != pdTRUE) return;   // never stall the display loop
  memcpy(&sWebFrame, &p, sizeof(Processed));
  sFrameFresh = true;
  xSemaphoreGive(sPubMutex);
}

// ============================================================ WiFi

static void onWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (sStaWanted) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(sWifiEvents, GOT_IP_BIT);
    if (sStaWanted) esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
    snprintf(sIp, sizeof(sIp), IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(sWifiEvents, GOT_IP_BIT);
  }
}

static void wifiInit() {
  sWifiEvents = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  sApNetif = esp_netif_create_default_wifi_ap();
  sStaNetif = esp_netif_create_default_wifi_sta();
  esp_netif_set_hostname(sStaNetif, MDNS_NAME);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr);
  if (mdns_init() == ESP_OK) {
    mdns_hostname_set(MDNS_NAME);
    mdns_instance_name_set("ESP32 Scope");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  }
}

static void startAp() {
  wifi_config_t ap = {};
  strlcpy((char*)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
  strlcpy((char*)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
  ap.ap.ssid_len = strlen(AP_SSID);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_wifi_start();
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(sApNetif, &ip) == ESP_OK) snprintf(sIp, sizeof(sIp), IPSTR, IP2STR(&ip.ip));
  else strlcpy(sIp, "192.168.4.1", sizeof(sIp));
  strlcpy(sSsid, AP_SSID, sizeof(sSsid));
  sIsAp = true;
}

static void wifiApply() {
  sStaWanted = false;
  esp_wifi_disconnect();
  esp_wifi_stop();
  xEventGroupClearBits(sWifiEvents, GOT_IP_BIT);
  sIsAp = false;
  if (S.wifiMode == 0) {
    strlcpy(sIp, "off", sizeof(sIp));
    sSsid[0] = 0;
    ESP_LOGI(TAG, "WiFi off");
    return;
  }
  if (S.wifiMode == 2 && S.staSsid[0]) {
    wifi_config_t sta = {};
    strlcpy((char*)sta.sta.ssid, S.staSsid, sizeof(sta.sta.ssid));
    strlcpy((char*)sta.sta.password, S.staPass, sizeof(sta.sta.password));
    strlcpy(sIp, "joining...", sizeof(sIp));
    strlcpy(sSsid, S.staSsid, sizeof(sSsid));
    sStaWanted = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_start();
    EventBits_t b = xEventGroupWaitBits(sWifiEvents, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(12000));
    if (b & GOT_IP_BIT) {
      esp_wifi_set_ps(WIFI_PS_NONE);   // modem sleep adds 100 ms+ latency
      ESP_LOGI(TAG, "joined %s, IP %s", sSsid, sIp);
      return;
    }
    ESP_LOGW(TAG, "could not join %s, falling back to AP", S.staSsid);
    sStaWanted = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
  }
  startAp();
  ESP_LOGI(TAG, "AP \"%s\" (password \"%s\"), open http://%s/", AP_SSID, AP_PASS, sIp);
}

// ============================================================ clients

static void addClient(int fd) {
  portENTER_CRITICAL(&sClientMux);
  bool known = false;
  for (int i = 0; i < sClientCount; i++) if (sClients[i] == fd) known = true;
  if (!known && sClientCount < MAX_CLIENTS) sClients[sClientCount++] = fd;
  portEXIT_CRITICAL(&sClientMux);
}

static void removeClient(int fd) {
  portENTER_CRITICAL(&sClientMux);
  for (int i = 0; i < sClientCount; i++) {
    if (sClients[i] == fd) {
      sClients[i] = sClients[--sClientCount];
      break;
    }
  }
  portEXIT_CRITICAL(&sClientMux);
}

static void onClose(httpd_handle_t, int fd) {
  removeClient(fd);
  close(fd);
}

// ============================================================ sending

struct WsMsg {
  int fd;                    // -1 = every client
  httpd_ws_type_t type;
  size_t len;
  uint8_t data[];
};

static void sendWork(void* arg) {
  WsMsg* m = (WsMsg*)arg;
  httpd_ws_frame_t f = {};
  f.type = m->type;
  f.payload = m->data;
  f.len = m->len;
  f.final = true;
  int fds[MAX_CLIENTS];
  int n = 0;
  if (m->fd >= 0) {
    fds[n++] = m->fd;
  } else {
    portENTER_CRITICAL(&sClientMux);
    for (int i = 0; i < sClientCount; i++) fds[n++] = sClients[i];
    portEXIT_CRITICAL(&sClientMux);
  }
  for (int i = 0; i < n; i++) {
    if (httpd_ws_get_fd_info(sServer, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET ||
        httpd_ws_send_frame_async(sServer, fds[i], &f) != ESP_OK) {
      removeClient(fds[i]);
    }
  }
  free(m);
  sInFlight--;
}

static WsMsg* newMsg(int fd, httpd_ws_type_t type, size_t len) {
  WsMsg* m = (WsMsg*)malloc(sizeof(WsMsg) + len);
  if (!m) return nullptr;
  m->fd = fd;
  m->type = type;
  m->len = len;
  return m;
}

static void queueMsg(WsMsg* m) {
  if (!m) return;
  sInFlight++;
  if (httpd_queue_work(sServer, sendWork, m) != ESP_OK) {
    free(m);
    sInFlight--;
  }
}

static void queueText(int fd, const std::string& s) {
  WsMsg* m = newMsg(fd, HTTPD_WS_TYPE_TEXT, s.size());
  if (m) memcpy(m->data, s.data(), s.size());
  queueMsg(m);
}

static std::string replyJson(const std::string& s) {
  std::string o = "{\"type\":\"reply\",\"text\":\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o + "\"}";
}

// Binary frame layout (little endian), 72-byte header then int16 samples:
//  0 u8 type=1   1 u8 flags   2 u16 n   4 u16 valid   6 u8 tdiv   8 u32 seq
// 12 f32 dt  16 f32 t0  20 f32 scale (V/LSB)  24 f32 acOffset
// 28.. f32 vmax vmin vavg vrms vac freq duty dcMean dcMin dcMax
static const size_t HDR = 72;
static inline void putF(uint8_t* b, size_t off, float v) { memcpy(b + off, &v, 4); }

static WsMsg* buildFrame() {
  xSemaphoreTake(sPubMutex, portMAX_DELAY);
  const Processed& p = sWebFrame;
  WsMsg* m = newMsg(-1, HTTPD_WS_TYPE_BINARY, HDR + 2 * p.n);
  if (m) {
    uint8_t* b = m->data;
    float peak = 1e-6f;
    for (uint16_t k = 0; k < p.n; k++) peak = fmaxf(peak, fabsf(p.v[k]));
    float scale = peak / 32000.0f;
    memset(b, 0, HDR);
    b[0] = 1;
    b[1] = p.flags;
    memcpy(b + 2, &p.n, 2);
    memcpy(b + 4, &p.valid, 2);
    b[6] = p.tdiv;
    memcpy(b + 8, &p.seq, 4);
    putF(b, 12, p.dt);
    putF(b, 16, p.t0);
    putF(b, 20, scale);
    putF(b, 24, p.acOffset);
    const Measurements& q = p.m;
    const float vals[] = {q.vmax, q.vmin, q.vavg, q.vrms, q.vac, q.freq, q.duty, q.dcMean, q.dcMin, q.dcMax};
    for (int i = 0; i < 10; i++) putF(b, 28 + 4 * i, vals[i]);
    const float inv = 1.0f / scale;
    for (uint16_t k = 0; k < p.n; k++) {
      int16_t s = (int16_t)lrintf(p.v[k] * inv);
      memcpy(b + HDR + 2 * k, &s, 2);
    }
  }
  sFrameFresh = false;
  xSemaphoreGive(sPubMutex);
  return m;
}

// ============================================================ HTTP handlers

static esp_err_t indexHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t statusHandler(httpd_req_t* req) {
  std::string s = statusJson();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, s.c_str(), s.size());
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// GET /cmd?c=tdiv%201ms  (handy for curl/scripts)
static esp_err_t cmdHandler(httpd_req_t* req) {
  char q[300], raw[260];
  std::string line;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "c", raw, sizeof(raw)) == ESP_OK) {
    for (size_t i = 0; raw[i]; i++) {
      if (raw[i] == '+') line += ' ';
      else if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) { line += (char)(hexv(raw[i + 1]) * 16 + hexv(raw[i + 2])); i += 2; }
      else line += raw[i];
    }
  }
  std::string r = runCommand(line);
  httpd_resp_set_type(req, !r.empty() && r[0] == '{' ? "application/json" : "text/plain");
  return httpd_resp_send(req, r.c_str(), r.size());
}

static esp_err_t wsHandler(httpd_req_t* req) {
  int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET) {   // handshake done
    addClient(fd);
    queueText(fd, statusJson());
    return ESP_OK;
  }
  httpd_ws_frame_t f = {};
  esp_err_t r = httpd_ws_recv_frame(req, &f, 0);
  if (r != ESP_OK) return r;
  if (f.len == 0) return ESP_OK;
  uint8_t* buf = (uint8_t*)malloc(f.len + 1);
  if (!buf) return ESP_ERR_NO_MEM;
  f.payload = buf;
  r = httpd_ws_recv_frame(req, &f, f.len);
  if (r == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) {
    CmdItem it;
    it.fd = fd;
    size_t n = f.len < sizeof(it.text) - 1 ? f.len : sizeof(it.text) - 1;
    memcpy(it.text, buf, n);
    it.text[n] = 0;
    xQueueSend(sCmdQueue, &it, 0);
  }
  free(buf);
  return r;
}

static esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  return httpd_resp_send(req, nullptr, 0);
}

static void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.core_id = 0;
  cfg.stack_size = 6144;
  cfg.max_open_sockets = 7;
  cfg.lru_purge_enable = true;
  cfg.send_wait_timeout = 2;
  cfg.close_fn = onClose;
  if (httpd_start(&sServer, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server failed to start");
    return;
  }
  httpd_uri_t root = {};
  root.uri = "/"; root.method = HTTP_GET; root.handler = indexHandler;
  httpd_register_uri_handler(sServer, &root);
  httpd_uri_t st = {};
  st.uri = "/status"; st.method = HTTP_GET; st.handler = statusHandler;
  httpd_register_uri_handler(sServer, &st);
  httpd_uri_t cmd = {};
  cmd.uri = "/cmd"; cmd.method = HTTP_GET; cmd.handler = cmdHandler;
  httpd_register_uri_handler(sServer, &cmd);
  httpd_uri_t ws = {};
  ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = wsHandler; ws.is_websocket = true;
  httpd_register_uri_handler(sServer, &ws);
  httpd_register_err_handler(sServer, HTTPD_404_NOT_FOUND, notFound);
}

// ============================================================ tasks

static void cmdTask(void*) {
  CmdItem it;
  for (;;) {
    if (xQueueReceive(sCmdQueue, &it, portMAX_DELAY) != pdTRUE) continue;
    std::string r = runCommand(it.text);
    if (!r.empty()) queueText(it.fd, r[0] == '{' ? r : replyJson(r));
    queueText(-1, statusJson());
  }
}

static void netTask(void*) {
  wifiInit();
  wifiApply();
  startServer();
  uint32_t lastFrame = 0, lastStatus = 0;
  for (;;) {
    if (sReconfig) {
      sReconfig = false;
      wifiApply();
    }
    uint32_t now = millis();
    if (sClientCount > 0 && sServer) {
      // <= 25 fps, and back off while the network is still busy with the last frames
      if (sFrameFresh && now - lastFrame >= 40 && sInFlight < 3) {
        queueMsg(buildFrame());
        lastFrame = now;
      }
      if (now - lastStatus >= 500 && sInFlight < 4) {
        queueText(-1, statusJson());
        lastStatus = now;
      }
    }
    delay(5);
  }
}

void webBegin() {
  sPubMutex = xSemaphoreCreateMutex();
  sCmdQueue = xQueueCreate(6, sizeof(CmdItem));
  xTaskCreatePinnedToCore(netTask, "net", 6144, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(cmdTask, "cmd", 8192, nullptr, 2, nullptr, 0);
}
