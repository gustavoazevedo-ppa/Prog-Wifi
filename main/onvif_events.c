#include "onvif_events.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "net/if.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "packet_manager.h"
#include "uart_handler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "ONVIF";

// ─── Software SHA-1 (avoids hardware SHA engine contention with MQTT TLS) ────
// The ESP32 hardware SHA engine is shared via a semaphore between all tasks.
// When MQTT TLS is active on one core and ONVIF WSSE hashes on the other,
// the semaphore can get corrupted under heap pressure. Using a pure software
// SHA-1 here eliminates that contention for the tiny 50-byte WSSE digest.

static inline uint32_t sha1_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sw_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
           h3 = 0x10325476, h4 = 0xC3D2E1F0;

  // Pre-processing: pad message
  size_t new_len = len + 1;
  while (new_len % 64 != 56) new_len++;
  uint8_t msg[128]; // max input is ~53 bytes → padded ≤ 128
  assert(new_len + 8 <= sizeof(msg));
  memcpy(msg, data, len);
  msg[len] = 0x80;
  memset(msg + len + 1, 0, new_len - len - 1);
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) msg[new_len + i] = (uint8_t)(bits >> (56 - 8 * i));
  new_len += 8;

  // Process each 64-byte block
  for (size_t offset = 0; offset < new_len; offset += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)msg[offset + 4*i] << 24) | ((uint32_t)msg[offset + 4*i+1] << 16) |
             ((uint32_t)msg[offset + 4*i+2] << 8) | (uint32_t)msg[offset + 4*i+3];
    for (int i = 16; i < 80; i++)
      w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
      uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
      e = d; d = c; c = sha1_rotl(b, 30); b = a; a = temp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }

  // Produce output
  uint32_t h[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; i++) {
    out[4*i]   = (uint8_t)(h[i] >> 24);
    out[4*i+1] = (uint8_t)(h[i] >> 16);
    out[4*i+2] = (uint8_t)(h[i] >> 8);
    out[4*i+3] = (uint8_t)(h[i]);
  }
}

// Event service paths to try
static const char *EVENT_PATHS[] = {ONVIF_EVENT_PATH_0, ONVIF_EVENT_PATH_1};
#define EVENT_PATHS_COUNT (sizeof(EVENT_PATHS) / sizeof(EVENT_PATHS[0]))

// ─── Poll outcome ─────────────────────────────────────────────────────────
// Distinguishes *why* a PullMessages poll did not succeed, so the scheduler can
// treat a genuinely unreachable camera (park OFFLINE fast) differently from a
// local heap shortage (keep the camera WATCHING and retry — it's not the
// camera's fault).
typedef enum {
  POLL_OK = 0,       // 200 response received and parsed
  POLL_FAIL_NETWORK, // perform error / non-200 — camera unreachable; counts toward OFFLINE
  POLL_DEFER_NOMEM,  // could not allocate HTTP client — module out of heap; do NOT penalize camera
} poll_result_t;

// ─── Camera context (one per slot) ───────────────────────────────────────────
typedef struct {
  // Connection info
  char pullpoint_url[ONVIF_URL_MAX_LEN];
  char event_svc_path[32];
  char ip[17];
  char user[17];
  char pass[17];
  uint32_t id_camera;
  uint8_t slot;

  // State machine
  cam_state_t state;
  TickType_t next_poll_due;
  uint8_t consecutive_failures; // consecutive *network* failures (drives OFFLINE)
  uint16_t nomem_defer_count;   // consecutive no-mem deferrals (separate bound)
  uint16_t backoff_ms;

  // Motion detection
  bool motion_active;
  bool motion_detected_edge;
  uint8_t no_motion_count;

  // HTTP response tracking (reset before each request)
  size_t response_bytes;

  // Interface this camera was last found reachable on (set by the OFFLINE probe).
  // All HTTP traffic for this camera is pinned to it. NULL = use default routing.
  esp_netif_t *netif;

  // Persistent HTTP client — reused across polls to avoid heap fragmentation.
  // Created on first successful subscribe, destroyed on stop or fatal error.
  esp_http_client_handle_t http_client;
} cam_ctx_t;

// ─── Module state ────────────────────────────────────────────────────────────
static cam_ctx_t s_cameras[MAX_CAMERAS] = {0};
static TaskHandle_t s_poll_task_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL; // protects s_cameras during add/remove

static bool s_task_stop_requested = false;
static uint8_t s_last_online_bitmask = 0; // tracks last reported camera bitmask

// Shared SOAP buffer — only used from the single poll task, protected by
// sequential access (no concurrent calls). Avoids 1.5 KB stack allocation
// on every HTTP request which stresses the 6 KB task stack.
static char s_soap_buf[ONVIF_SOAP_BUF_SIZE];

// Forward declarations for HTTP client lifecycle (defined after event handlers)
static void cam_destroy_http_client(cam_ctx_t *cam);
static bool cam_ensure_http_client(cam_ctx_t *cam);

// ─── CMD47 violation notification ────────────────────────────────────────────
static void send_cmd47_violation(uint32_t id_camera, uint8_t motion_type) {
  uint8_t pkt[14];
  pkt[0] = 0xAA;
  pkt[1] = 0xAA;
  pkt[2] = 0x00;
  pkt[3] = 0x05;
  pkt[4] = PKT_TYPE_CONFIG;
  pkt[5] = PKT_CMD_47;
  memcpy(&pkt[6], &id_camera, 4);
  pkt[10] = motion_type; // 0x01 = motion detected, 0x00 = motion ended
  pkt[11] = crc8(&pkt[5], 6);
  pkt[12] = 0xBB;
  pkt[13] = 0xBB;
  uart_handler_write_bytes(pkt, sizeof(pkt));
  ESP_LOGI(TAG, "CMD47: camera 0x%08lX motion=%s", (unsigned long)id_camera,
           motion_type ? "START" : "END");
}

// ─── CMD48 camera online bitmask notification (voluntary) ────────────────────
static void send_cmd48_notification(uint8_t bitmask) {
  uint8_t pkt[] = {0xAA, 0xAA, 0x00, 0x02, PKT_TYPE_CONFIG, PKT_CMD_48,
                   0x01, bitmask, 0x00, 0xBB, 0xBB};
  pkt[8] = crc8(&pkt[4], 3); // CRC over CMD + direction + bitmask
  uart_handler_write_bytes(pkt, sizeof(pkt));
  ESP_LOGI(TAG, "CMD48: bitmask=0x%02X (voluntary)", bitmask);
}

// Check if online bitmask changed and notify central if so (30s cooldown)
static void check_and_notify_bitmask_change(void) {
  static TickType_t last_notify_time = 0;
  TickType_t now = xTaskGetTickCount();

  if (last_notify_time != 0 &&
      (now - last_notify_time) < pdMS_TO_TICKS(30000)) {
    return; // cooldown active, ignore changes
  }

  uint8_t current = onvif_events_get_online_bitmask();
  if (current != s_last_online_bitmask) {
    s_last_online_bitmask = current;
    send_cmd48_notification(current);
    last_notify_time = now;
  }
}

// ─── TCP Probe ───────────────────────────────────────────────────────────────
// Quick check if camera is reachable (TCP handshake only, no data).
//
// Dual-interface aware: with WiFi and Ethernet both up (and possibly on the same
// subnet), LwIP picks the egress interface by its own routing rules, which may
// choose an interface that can't physically reach the camera. To make
// reachability reflect reality, we probe over each active interface (pinning the
// socket's source address to that interface's IP) and report the camera online
// if any interface reaches it. Degrades to identical behavior when only one
// interface has an IP.

// Probe a camera over one specific interface. Returns true only if the TCP
// handshake actually completes on that interface. A NULL netif is skipped.
static bool tcp_probe_via(const char *ip, int timeout_ms, esp_netif_t *netif) {
  if (netif == NULL)
    return false;

  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0)
    return false; // interface has no IP → not usable

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return false;

  // Pin egress to this interface by binding the source address to its IP.
  // (Source-address bind, not SO_BINDTODEVICE, so no extra sdkconfig options
  // are required.)
  struct sockaddr_in src = {
      .sin_family = AF_INET,
      .sin_port = 0, // ephemeral
  };
  src.sin_addr.s_addr = ip_info.ip.addr;
  if (bind(sock, (struct sockaddr *)&src, sizeof(src)) != 0) {
    close(sock);
    return false;
  }

  struct sockaddr_in dst = {
      .sin_family = AF_INET,
      .sin_port = htons(80),
  };
  inet_aton(ip, &dst.sin_addr);

  // Non-blocking connect
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  connect(sock, (struct sockaddr *)&dst, sizeof(dst));

  // Wait for the connection to complete (or time out)
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(sock, &wfds);
  struct timeval tv = {
      .tv_sec = timeout_ms / 1000,
      .tv_usec = (timeout_ms % 1000) * 1000,
  };

  int ret = select(sock + 1, NULL, &wfds, NULL, &tv);

  // select() also wakes on connection *failure* (refused/unreachable). Confirm
  // the socket actually connected before declaring success — otherwise a
  // refused connection would be reported as "camera online".
  bool ok = false;
  if (ret > 0) {
    int so_err = 0;
    socklen_t len = sizeof(so_err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 &&
        so_err == 0)
      ok = true;
  }

  close(sock);
  return ok;
}

// Probe across the known interfaces and return the FIRST one that reaches the
// camera, or NULL if none do. Ethernet is tried before WiFi so a camera
// reachable both ways prefers the wired path. The returned netif is stored on
// the camera and used to pin all its HTTP traffic (see cam_http_bind_iface).
static esp_netif_t *tcp_probe_pick(const char *ip, int timeout_ms) {
  esp_netif_t *ifaces[] = {ethernet_get_netif(), wifi_get_netif()};
  for (size_t i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
    if (tcp_probe_via(ip, timeout_ms, ifaces[i]))
      return ifaces[i]; // first interface that reaches it is enough
  }
  return NULL;
}

// Fill an ifreq with the LwIP implementation name of the camera's pinned netif
// (e.g. "st1" for WiFi, "en1" for Ethernet) for use as esp_http_client_config_t
// .if_name. Returns true when a valid interface name was written; on false the
// caller should pass if_name = NULL, which falls back to default routing.
static bool cam_http_bind_iface(const cam_ctx_t *cam, struct ifreq *out) {
  if (cam->netif == NULL)
    return false;
  memset(out, 0, sizeof(*out));
  if (esp_netif_get_netif_impl_name(cam->netif, out->ifr_name) != ESP_OK)
    return false;
  return (out->ifr_name[0] != '\0');
}

// ─── WSSE UsernameToken ──────────────────────────────────────────────────────
static int build_wsse_token(char *buf, size_t buf_size, const char *username,
                            const char *password) {
  uint8_t nonce_raw[16];
  esp_fill_random(nonce_raw, sizeof(nonce_raw));

  char nonce_b64[25];
  size_t nonce_b64_len = 0;
  mbedtls_base64_encode((unsigned char *)nonce_b64, sizeof(nonce_b64),
                        &nonce_b64_len, nonce_raw, sizeof(nonce_raw));
  nonce_b64[nonce_b64_len] = '\0';

  time_t now;
  time(&now);
  struct tm tm_info;
  gmtime_r(&now, &tm_info);
  char created[22];
  strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

  uint8_t sha_input[16 + 20 + 17];
  size_t sha_len = 0;
  memcpy(sha_input, nonce_raw, 16);
  sha_len += 16;
  size_t created_len = strlen(created);
  memcpy(sha_input + sha_len, created, created_len);
  sha_len += created_len;
  size_t pass_len = password ? strlen(password) : 0;
  if (pass_len > 0) {
    memcpy(sha_input + sha_len, password, pass_len);
    sha_len += pass_len;
  }

  uint8_t sha_out[20];
  sw_sha1(sha_input, sha_len, sha_out);

  char digest_b64[29];
  size_t digest_b64_len = 0;
  mbedtls_base64_encode((unsigned char *)digest_b64, sizeof(digest_b64),
                        &digest_b64_len, sha_out, 20);
  digest_b64[digest_b64_len] = '\0';

  return snprintf(
      buf, buf_size,
      "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
      "oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
      "<wsse:UsernameToken>"
      "<wsse:Username>%s</wsse:Username>"
      "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
      "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
      "%s</wsse:Password>"
      "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
      "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
      "%s</wsse:Nonce>"
      "<wsu:Created xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
      "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
      "%s</wsu:Created>"
      "</wsse:UsernameToken>"
      "</wsse:Security>",
      username ? username : "", digest_b64, nonce_b64, created);
}

// ─── SOAP Envelope Builder ───────────────────────────────────────────────────
static int build_soap_envelope(char *buf, size_t buf_size, const char *action,
                               const char *to_url, const char *username,
                               const char *password, const char *body) {
  static char wsse_buf[700];
  build_wsse_token(wsse_buf, sizeof(wsse_buf), username, password);

  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  uint32_t r3 = esp_random();
  uint32_t r4 = esp_random();

  int len = snprintf(
      buf, buf_size,
      "<?xml version='1.0' encoding='utf-8'?>\n"
      "<soap-env:Envelope"
      " xmlns:soap-env=\"http://www.w3.org/2003/05/soap-envelope\""
      " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""
      " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\">"
      "<soap-env:Header>"
      "<wsa:Action>%s</wsa:Action>"
      "<wsa:MessageID>urn:uuid:%08lx-%04lx-%04lx-%04lx-%04lx%08lx"
      "</wsa:MessageID>"
      "<wsa:To>%s</wsa:To>"
      "%s"
      "</soap-env:Header>"
      "<soap-env:Body>%s</soap-env:Body>"
      "</soap-env:Envelope>",
      action, (unsigned long)r1, (unsigned long)(r2 & 0xFFFF),
      (unsigned long)(r2 >> 16), (unsigned long)(r3 & 0xFFFF),
      (unsigned long)(r3 >> 16), (unsigned long)r4, to_url, wsse_buf, body);

  if (len < 0 || (size_t)len >= buf_size) {
    ESP_LOGE(TAG, "SOAP overflow (%d >= %d)", len, (int)buf_size);
    return -1;
  }
  return len;
}

// ─── Streaming Address parser ────────────────────────────────────────────────
typedef struct {
  char *dest;
  size_t dest_size;
  size_t dest_pos;
  size_t total_bytes; // track total response size
  bool in_address;
  bool found;
} addr_parse_ctx_t;

static void parse_address_chunk(const char *data, int len,
                                addr_parse_ctx_t *p) {
  for (int i = 0; i < len; i++) {
    if (p->found)
      return;
    if (!p->in_address) {
      const char *remain = data + i;
      int remain_len = len - i;
      const char *gt = memchr(remain, '>', remain_len);
      if (!gt)
        return;
      int span = (int)(gt - remain);
      if (span > 6) {
        for (const char *c = remain; c <= gt - 7; c++) {
          if (memcmp(c, "Address", 7) == 0) {
            p->in_address = true;
            p->dest_pos = 0;
            i = (int)(gt - data);
            break;
          }
        }
      }
      if (!p->in_address)
        i = (int)(gt - data);
    } else {
      if (data[i] == '<') {
        p->in_address = false;
        if (p->dest_pos > 0) {
          p->dest[p->dest_pos] = '\0';
          p->found = true;
        }
      } else if (p->dest_pos < p->dest_size - 1) {
        p->dest[p->dest_pos++] = data[i];
      }
    }
  }
}

static esp_err_t _subscribe_http_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 &&
      evt->user_data) {
    addr_parse_ctx_t *ctx = (addr_parse_ctx_t *)evt->user_data;
    ctx->total_bytes += evt->data_len;
    if (ctx->total_bytes > ONVIF_MAX_RESPONSE_SIZE) {
      ESP_LOGW(TAG, "Subscribe response too large (%d bytes), aborting",
               (int)ctx->total_bytes);
      return ESP_FAIL; // signals esp_http_client to stop reading
    }
    parse_address_chunk((char *)evt->data, evt->data_len, ctx);
  }
  return ESP_OK;
}

// ─── Unsubscribe ─────────────────────────────────────────────────────────────
static void unsubscribe(cam_ctx_t *cam) {
  // Destroy the persistent PullMessages client — the pullpoint URL is about
  // to change, so the old client's connection is no longer valid.
  cam_destroy_http_client(cam);

  if (cam->pullpoint_url[0] == '\0')
    return;

  static const char *action =
      "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/"
      "UnsubscribeRequest";
  static const char *body =
      "<wsnt:Unsubscribe xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\"/>";

  int len = build_soap_envelope(s_soap_buf, sizeof(s_soap_buf), action,
                                cam->pullpoint_url, cam->user, cam->pass, body);
  if (len < 0)
    goto clear;

  // Use a temporary client for unsubscribe (infrequent operation)
  struct ifreq ifr;
  bool pinned = cam_http_bind_iface(cam, &ifr);
  esp_http_client_config_t cfg = {
      .url = cam->pullpoint_url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = ONVIF_SUBSCRIBE_TIMEOUT_MS,
      .buffer_size = ONVIF_HTTP_RX_BUF_SIZE,
      .if_name = pinned ? &ifr : NULL,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == NULL) {
    ESP_LOGE(TAG, "[%d] Unsubscribe: http_client_init failed", cam->slot);
    goto clear;
  }
  esp_http_client_set_header(client, "Content-Type",
                             "application/soap+xml; charset=utf-8");
  esp_http_client_set_header(client, "Accept", "*/*");
  esp_http_client_set_post_field(client, s_soap_buf, len);
  esp_http_client_perform(client);
  esp_http_client_cleanup(client);

clear:
  cam->pullpoint_url[0] = '\0';
}

// ─── CreatePullPointSubscription ─────────────────────────────────────────────
static bool create_subscription(cam_ctx_t *cam) {

  static const char *action =
      "http://www.onvif.org/ver10/events/wsdl/EventPortType/"
      "CreatePullPointSubscriptionRequest";
  static const char *body =
      "<ns0:CreatePullPointSubscription"
      " xmlns:ns0=\"http://www.onvif.org/ver10/events/wsdl\">"
      "<ns0:InitialTerminationTime>" ONVIF_SUBSCRIPTION_TIMEOUT_STR
      "</ns0:InitialTerminationTime>"
      "</ns0:CreatePullPointSubscription>";

  // Build path list: known working path first
  const char *paths[EVENT_PATHS_COUNT];
  int path_count = 0;
  if (cam->event_svc_path[0] != '\0') {
    paths[path_count++] = cam->event_svc_path;
    for (int i = 0; i < (int)EVENT_PATHS_COUNT; i++) {
      if (strcmp(EVENT_PATHS[i], cam->event_svc_path) != 0)
        paths[path_count++] = EVENT_PATHS[i];
    }
  } else {
    for (int i = 0; i < (int)EVENT_PATHS_COUNT; i++)
      paths[path_count++] = EVENT_PATHS[i];
  }

  char temp_url[ONVIF_URL_MAX_LEN] = {0};

  for (int i = 0; i < path_count; i++) {
    const char *path = paths[i];
    char to_url[128];
    snprintf(to_url, sizeof(to_url), "http://%s:%d%s", cam->ip, 80, path);

    int len = build_soap_envelope(s_soap_buf, sizeof(s_soap_buf), action, to_url,
                                  cam->user, cam->pass, body);
    if (len < 0)
      continue;

    addr_parse_ctx_t parse = {
        .dest = temp_url,
        .dest_size = sizeof(temp_url),
        .dest_pos = 0,
        .total_bytes = 0,
        .in_address = false,
        .found = false,
    };

    struct ifreq ifr;
    bool pinned = cam_http_bind_iface(cam, &ifr);
    esp_http_client_config_t cfg = {
        .url = to_url,
        .method = HTTP_METHOD_POST,
        .event_handler = _subscribe_http_handler,
        .user_data = &parse,
        .timeout_ms = ONVIF_SUBSCRIBE_TIMEOUT_MS,
        .buffer_size = ONVIF_HTTP_RX_BUF_SIZE,
        .if_name = pinned ? &ifr : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
      ESP_LOGE(TAG, "[%d] Subscribe: http_client_init failed (path=%s)",
               cam->slot, path);
      continue;
    }
    esp_http_client_set_header(
        client, "Content-Type",
        "application/soap+xml; charset=utf-8; action=\""
        "http://www.onvif.org/ver10/events/wsdl/EventPortType/"
        "CreatePullPointSubscriptionRequest\"");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_post_field(client, s_soap_buf, len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200)
      continue;

    if (!parse.found || temp_url[0] == '\0')
      continue;

    // Success
    strncpy(cam->pullpoint_url, temp_url, sizeof(cam->pullpoint_url) - 1);
    cam->pullpoint_url[sizeof(cam->pullpoint_url) - 1] = '\0';
    strncpy(cam->event_svc_path, path, sizeof(cam->event_svc_path) - 1);
    cam->event_svc_path[sizeof(cam->event_svc_path) - 1] = '\0';

    ESP_LOGI(TAG, "[%d] Subscribed: %s", cam->slot, cam->pullpoint_url);
    return true;
  }

  ESP_LOGW(TAG, "[%d] Subscribe failed", cam->slot);
  return false;
}

// ─── Full subscribe (unsubscribe old + create new) ───────────────────────────
static bool full_subscribe(cam_ctx_t *cam) {
  unsubscribe(cam);
  return create_subscription(cam);
}

// ─── Motion parser (streaming) ───────────────────────────────────────────────
static void parse_motion_chunk(const char *data, int len, cam_ctx_t *cam) {
  if (len > 0 && memmem(data, len, "Name=\"IsMotion\" Value=\"true\"", 28)) {
    cam->no_motion_count = 0;
    if (!cam->motion_active) {
      cam->motion_active = true;
      cam->motion_detected_edge = true;
    }
  } else {
    if (cam->motion_active) {
      cam->no_motion_count++;
      if (cam->no_motion_count >= ONVIF_NO_MOTION_THRESHOLD) {
        cam->motion_active = false;
        cam->no_motion_count = 0;
      }
    }
  }
}

static esp_err_t _pull_http_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 &&
      evt->user_data) {
    cam_ctx_t *cam = (cam_ctx_t *)evt->user_data;
    cam->response_bytes += evt->data_len;
    if (cam->response_bytes > ONVIF_MAX_RESPONSE_SIZE) {
      ESP_LOGW(TAG, "[%d] PullMessages response too large (%d bytes), aborting",
               cam->slot, (int)cam->response_bytes);
      return ESP_FAIL;
    }
    parse_motion_chunk((char *)evt->data, evt->data_len, cam);
  }
  return ESP_OK;
}

// ─── HTTP Client Lifecycle Helpers ────────────────────────────────────────────
// The persistent HTTP client is used exclusively for PullMessages (the hot path).
// Subscribe/Unsubscribe are infrequent and use temporary clients.

static void cam_destroy_http_client(cam_ctx_t *cam) {
  if (cam->http_client != NULL) {
    esp_http_client_cleanup(cam->http_client);
    cam->http_client = NULL;
  }
}

// Create (or recreate) the persistent HTTP client for PullMessages.
// Returns true if the client is ready to use.
static bool cam_ensure_http_client(cam_ctx_t *cam) {
  if (cam->http_client != NULL)
    return true;

  if (cam->pullpoint_url[0] == '\0')
    return false;

  struct ifreq ifr;
  bool pinned = cam_http_bind_iface(cam, &ifr);
  esp_http_client_config_t cfg = {
      .url = cam->pullpoint_url,
      .method = HTTP_METHOD_POST,
      .event_handler = _pull_http_handler,
      .user_data = cam,
      .timeout_ms = ONVIF_HTTP_TIMEOUT_MS,
      .buffer_size = ONVIF_HTTP_RX_BUF_SIZE,
      .keep_alive_enable = true,
      .if_name = pinned ? &ifr : NULL,
  };

  cam->http_client = esp_http_client_init(&cfg);
  if (cam->http_client == NULL) {
    ESP_LOGE(TAG, "[%d] Failed to create persistent HTTP client", cam->slot);
    return false;
  }

  // Set headers once — they persist across perform() calls
  esp_http_client_set_header(
      cam->http_client, "Content-Type",
      "application/soap+xml; charset=utf-8; action=\""
      "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
      "PullMessagesRequest\"");
  esp_http_client_set_header(cam->http_client, "Accept", "*/*");

  return true;
}

// ─── PullMessages ────────────────────────────────────────────────────────────
static poll_result_t pull_messages(cam_ctx_t *cam) {
  if (cam->pullpoint_url[0] == '\0')
    return POLL_FAIL_NETWORK; // no subscription — needs re-subscribe via OFFLINE path

  static const char *action =
      "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/"
      "PullMessagesRequest";
  static const char *body =
      "<ns0:PullMessages xmlns:ns0=\"http://www.onvif.org/ver10/events/wsdl\">"
      "<ns0:Timeout>" ONVIF_PULL_TIMEOUT_STR "</ns0:Timeout>"
      "<ns0:MessageLimit>10</ns0:MessageLimit>"
      "</ns0:PullMessages>";

  int len = build_soap_envelope(s_soap_buf, sizeof(s_soap_buf), action,
                                cam->pullpoint_url, cam->user, cam->pass, body);
  if (len < 0)
    return POLL_FAIL_NETWORK; // envelope build error (not a heap-alloc failure)

  cam->response_bytes = 0; // reset before request

  // Capture whether this poll will run on a brand-new client or a reused
  // keep-alive one. Must be read BEFORE cam_ensure_http_client(), which sets
  // cam->http_client non-NULL. A failure on a reused client (fresh=0) points at
  // a stale keep-alive socket; a failure on a fresh client (fresh=1) points at
  // the network path or a request-level problem.
  bool was_fresh = (cam->http_client == NULL);

  // Ensure we have a persistent HTTP client. Failure here means esp_http_client
  // could not allocate — the module is out of heap, NOT the camera's fault.
  if (!cam_ensure_http_client(cam))
    return POLL_DEFER_NOMEM;

  // Capture the handle locally. Another task (start_camera) may NULL
  // cam->http_client while we're blocked in perform(). Using a local
  // prevents dereferencing NULL after perform returns.
  esp_http_client_handle_t client = cam->http_client;
  if (client == NULL)
    return POLL_FAIL_NETWORK; // reconfigured out from under us; retry path

  // Update URL in case pullpoint changed after re-subscribe
  esp_http_client_set_url(client, cam->pullpoint_url);
  esp_http_client_set_post_field(client, s_soap_buf, len);

  // Time the perform() call. Near-instant failure = connection refused/reset
  // (stale socket or middlebox RST); failure near ONVIF_HTTP_TIMEOUT_MS = the
  // camera accepted the connection but never sent a header.
  int64_t perform_t0 = esp_timer_get_time();
  esp_err_t err = esp_http_client_perform(client);
  int64_t perform_ms = (esp_timer_get_time() - perform_t0) / 1000;

  // After perform(), check if our handle was destroyed by another task.
  // If cam->http_client was NULLed, the camera was reconfigured — clean up
  // our local handle (which is the old one) and bail out.
  if (cam->http_client == NULL) {
    ESP_LOGW(TAG, "[%d] HTTP client destroyed during perform (camera reconfigured)",
             cam->slot);
    esp_http_client_cleanup(client);
    return POLL_FAIL_NETWORK;
  }

  int status = esp_http_client_get_status_code(client);

  if (err != ESP_OK || status != 200) {
    // Capture the low-level socket errno *before* cleanup. ESP_ERR_HTTP_FETCH_HEADER
    // is generic; the underlying errno tells us how the connection actually died:
    //   104 ECONNRESET   — peer sent RST (stale keep-alive socket / middlebox reset)
    //   113 ECONNABORTED — local stack aborted the connection
    //   110 ETIMEDOUT    — connected but no response header within timeout
    //   128 ENOTCONN     — socket was never really connected
    int sock_errno = esp_http_client_get_errno(client);
    ESP_LOGW(TAG,
             "[%d] PullMessages failed (err=%s, status=%d, errno=%d, fresh=%d, "
             "perform_ms=%lld)",
             cam->slot, esp_err_to_name(err), status, sock_errno, was_fresh,
             perform_ms);
    // Destroy client on error — it will be recreated on next attempt.
    // This handles stale TCP connections, camera reboots, etc.
    cam_destroy_http_client(cam);
    return POLL_FAIL_NETWORK;
  }

  return POLL_OK;
}

// ─── Priority camera selection ───────────────────────────────────────────────
// Returns the camera that should be polled next, or NULL if none are due.
// Priority: WATCHING > TRIGGERED > OFFLINE, but overdue lower-priority cameras
// are served before re-polling a same-priority camera that was just served.
// This prevents WATCHING cameras from starving TRIGGERED/OFFLINE polls.
static cam_ctx_t *s_last_polled = NULL;

static cam_ctx_t *select_next_camera(void) {
  TickType_t now = xTaskGetTickCount();
  cam_ctx_t *best_high = NULL; // best WATCHING camera
  TickType_t best_high_overdue = 0;
  cam_ctx_t *best_low = NULL; // best TRIGGERED/OFFLINE camera (overdue only)
  TickType_t best_low_overdue = 0;

  for (int i = 0; i < MAX_CAMERAS; i++) {
    cam_ctx_t *cam = &s_cameras[i];
    if (cam->state == CAM_STATE_INACTIVE)
      continue;

    // Not due yet?
    if ((int32_t)(now - cam->next_poll_due) < 0)
      continue;

    TickType_t overdue = now - cam->next_poll_due;

    if (cam->state == CAM_STATE_WATCHING) {
      if (overdue > best_high_overdue) {
        best_high = cam;
        best_high_overdue = overdue;
      }
    } else {
      // TRIGGERED or OFFLINE
      if (overdue > best_low_overdue) {
        best_low = cam;
        best_low_overdue = overdue;
      }
    }
  }

  // If a lower-priority camera is overdue, serve it before re-polling
  // the same high-priority camera we just served.
  if (best_low != NULL && (best_high == NULL || s_last_polled == best_high)) {
    s_last_polled = best_low;
    return best_low;
  }

  if (best_high != NULL) {
    s_last_polled = best_high;
    return best_high;
  }

  return best_low; // may be NULL
}

// ─── Save event service path to NVS (only if changed) ────────────────────────
static void save_event_path_to_nvs(cam_ctx_t *cam) {
  nvs_handle_t h;
  if (nvs_open("camera", NVS_READWRITE, &h) != ESP_OK)
    return;

  char key[16];
  char stored[32] = {0};
  size_t stored_len = sizeof(stored);
  snprintf(key, sizeof(key), "evsvc%d", cam->slot);
  nvs_get_str(h, key, stored, &stored_len);

  if (strcmp(stored, cam->event_svc_path) != 0) {
    nvs_set_str(h, key, cam->event_svc_path);
    nvs_commit(h);
  }
  nvs_close(h);
}

// ─── Handle WATCHING / TRIGGERED state ───────────────────────────────────────
static void handle_poll(cam_ctx_t *cam) {
  TickType_t now = xTaskGetTickCount();

  poll_result_t result = pull_messages(cam);

  if (result == POLL_OK) {
    // Successful poll — clear both failure counters.
    cam->consecutive_failures = 0;
    cam->nomem_defer_count = 0;

    if (cam->state == CAM_STATE_WATCHING) {
      // Check for motion rising edge
      if (cam->motion_detected_edge) {
        cam->motion_detected_edge = false;
        ESP_LOGI(TAG, "[%d] MOTION (camera 0x%08lX)", cam->slot,
                 (unsigned long)cam->id_camera);
        send_cmd47_violation(cam->id_camera, 0x01);
        cam->state = CAM_STATE_TRIGGERED;
        cam->next_poll_due =
            now + pdMS_TO_TICKS(ONVIF_TRIGGERED_POLL_INTERVAL_MS);
      } else {
        // Still watching, poll again after minimum interval
        cam->next_poll_due = now + pdMS_TO_TICKS(ONVIF_WATCHING_POLL_INTERVAL_MS);
      }
    } else {
      // TRIGGERED — check if motion ended
      if (!cam->motion_active) {
        cam->state = CAM_STATE_WATCHING;
        cam->next_poll_due = now + pdMS_TO_TICKS(ONVIF_WATCHING_POLL_INTERVAL_MS);
        ESP_LOGI(TAG, "[%d] Motion ended, back to WATCHING", cam->slot);
        send_cmd47_violation(cam->id_camera, 0x00);
      } else {
        cam->next_poll_due =
            now + pdMS_TO_TICKS(ONVIF_TRIGGERED_POLL_INTERVAL_MS);
      }
    }
  } else if (result == POLL_DEFER_NOMEM) {
    // Module was out of heap — this is NOT the camera's fault, so do not touch
    // consecutive_failures or drive OFFLINE on it. Keep the camera WATCHING and
    // retry after a modest interval, giving the heap time to recover.
    cam->nomem_defer_count++;

    if (cam->nomem_defer_count >= ONVIF_MAX_NOMEM_DEFERRALS) {
      // Sustained starvation: surface it. Force OFFLINE so the host sees the
      // camera drop off the CMD48 bitmask instead of it staying "online" while
      // silently blind. The OFFLINE path will re-probe/re-subscribe later.
      ESP_LOGW(TAG,
               "[%d] -> OFFLINE after %u consecutive no-mem deferrals "
               "(sustained heap shortage)",
               cam->slot, cam->nomem_defer_count);
      cam->nomem_defer_count = 0;
      cam->consecutive_failures = 0;
      cam->state = CAM_STATE_OFFLINE;
      cam->backoff_ms = ONVIF_BACKOFF_INITIAL_MS;
      cam->next_poll_due =
          now + pdMS_TO_TICKS(ONVIF_BACKOFF_INITIAL_MS +
                              cam->slot * ONVIF_RETRY_STAGGER_MS);
    } else {
      ESP_LOGW(TAG, "[%d] poll deferred: out of heap (defer %u/%u)", cam->slot,
               cam->nomem_defer_count, ONVIF_MAX_NOMEM_DEFERRALS);
      cam->next_poll_due = now + pdMS_TO_TICKS(ONVIF_NOMEM_RETRY_INTERVAL_MS);
    }
  } else {
    // POLL_FAIL_NETWORK — the camera is unreachable. This is what OFFLINE +
    // backoff exists for. A no-mem deferral does NOT reach here, so a busy
    // module can no longer push a healthy camera offline.
    cam->consecutive_failures++;
    if (cam->consecutive_failures >= ONVIF_MAX_PULL_FAILURES) {
      ESP_LOGW(TAG, "[%d] -> OFFLINE (backoff %dms)", cam->slot,
               ONVIF_BACKOFF_INITIAL_MS);
      cam->state = CAM_STATE_OFFLINE;
      cam->backoff_ms = ONVIF_BACKOFF_INITIAL_MS;
      cam->next_poll_due =
          now + pdMS_TO_TICKS(ONVIF_BACKOFF_INITIAL_MS +
                              cam->slot * ONVIF_RETRY_STAGGER_MS);
    } else {
      // Quick retry once before declaring offline
      cam->next_poll_due = now + pdMS_TO_TICKS(2000);
    }
  }
}

// ─── Handle OFFLINE state (probe + re-subscribe) ─────────────────────────────
static void handle_offline(cam_ctx_t *cam) {
  TickType_t now = xTaskGetTickCount();

  // TCP probe first — cheap check before full SOAP. Remember which interface
  // reached the camera so all subsequent HTTP is pinned to it.
  esp_netif_t *reached_on = tcp_probe_pick(cam->ip, ONVIF_TCP_PROBE_TIMEOUT_MS);
  if (reached_on == NULL) {
    // Still unreachable — increase backoff
    cam->backoff_ms = (cam->backoff_ms * 2 > ONVIF_BACKOFF_MAX_MS)
                          ? ONVIF_BACKOFF_MAX_MS
                          : cam->backoff_ms * 2;
    cam->next_poll_due = now + pdMS_TO_TICKS(cam->backoff_ms);
    return;
  }
  cam->netif = reached_on;

  // Camera is reachable — attempt full subscribe
  {
    char ifn[8] = "?";
    esp_netif_get_netif_impl_name(cam->netif, ifn);
    ESP_LOGI(TAG, "[%d] Reachable via %s, re-subscribing...", cam->slot, ifn);
  }

  if (full_subscribe(cam)) {
    cam->state = CAM_STATE_WATCHING;
    cam->consecutive_failures = 0;
    cam->backoff_ms = ONVIF_BACKOFF_INITIAL_MS;
    cam->next_poll_due = now + pdMS_TO_TICKS(ONVIF_WATCHING_POLL_INTERVAL_MS);
    save_event_path_to_nvs(cam);
    ESP_LOGI(TAG, "[%d] Recovered -> %s", cam->slot, cam->pullpoint_url);
  } else {
    // Subscribe failed (camera reachable but ONVIF broken)
    cam->backoff_ms = (cam->backoff_ms * 2 > ONVIF_BACKOFF_MAX_MS)
                          ? ONVIF_BACKOFF_MAX_MS
                          : cam->backoff_ms * 2;
    cam->next_poll_due = now + pdMS_TO_TICKS(cam->backoff_ms);
    ESP_LOGW(TAG, "[%d] Subscribe failed, backoff %dms", cam->slot,
             cam->backoff_ms);
  }
}

// ─── Main poll task ──────────────────────────────────────────────────────────
static void onvif_poll_task(void *param) {
  (void)param;
  ESP_LOGI(TAG, "Poll task started");

  // Initial subscribe for all configured cameras (staggered)
  for (int i = 0; i < MAX_CAMERAS && !s_task_stop_requested; i++) {
    cam_ctx_t *cam = &s_cameras[i];
    if (cam->state == CAM_STATE_INACTIVE)
      continue;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (full_subscribe(cam)) {
      cam->state = CAM_STATE_WATCHING;
      cam->next_poll_due = xTaskGetTickCount() + pdMS_TO_TICKS(ONVIF_WATCHING_POLL_INTERVAL_MS);
      save_event_path_to_nvs(cam);
    } else {
      cam->state = CAM_STATE_OFFLINE;
      cam->backoff_ms = ONVIF_BACKOFF_INITIAL_MS;
      cam->next_poll_due =
          xTaskGetTickCount() + pdMS_TO_TICKS(ONVIF_BACKOFF_INITIAL_MS);
    }
    xSemaphoreGive(s_mutex);

    vTaskDelay(pdMS_TO_TICKS(ONVIF_BOOT_STAGGER_MS));
  }

  // Notify central of initial camera online state after boot subscribe
  check_and_notify_bitmask_change();

  // Main polling loop
  while (!s_task_stop_requested) {
    cam_ctx_t *cam = NULL;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cam = select_next_camera();
    xSemaphoreGive(s_mutex);

    if (cam == NULL) {
      // Nothing due — short sleep
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Dispatch based on state
    switch (cam->state) {
    case CAM_STATE_WATCHING:
    case CAM_STATE_TRIGGERED:
      handle_poll(cam);
      break;
    case CAM_STATE_OFFLINE:
      handle_offline(cam);
      break;
    default:
      break;
    }

    // Notify central if camera online bitmask changed
    check_and_notify_bitmask_change();

    // Brief yield to let other FreeRTOS tasks run
    taskYIELD();
  }

  // Cleanup: unsubscribe all active cameras
  for (int i = 0; i < MAX_CAMERAS; i++) {
    if (s_cameras[i].state != CAM_STATE_INACTIVE)
      unsubscribe(&s_cameras[i]);
  }

  ESP_LOGI(TAG, "Poll task stopped");
  s_poll_task_handle = NULL;
  vTaskDelete(NULL);
}

// ─── Public API ──────────────────────────────────────────────────────────────

void onvif_events_stop_camera(uint8_t slot) {
  if (slot >= MAX_CAMERAS)
    return;

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  cam_ctx_t *cam = &s_cameras[slot];
  if (cam->state != CAM_STATE_INACTIVE) {
    // Detach client — poll task will clean up its local copy
    cam->http_client = NULL;
    unsubscribe(cam);
    cam->state = CAM_STATE_INACTIVE;
    ESP_LOGI(TAG, "[%d] Stopped", slot);
  }
  xSemaphoreGive(s_mutex);
}

void onvif_events_stop_all(void) {
  ESP_LOGI(TAG, "Stopping...");

  // Signal the task to stop
  s_task_stop_requested = true;

  // Wait for task to exit (it unsubscribes all cameras on exit).
  // The task may be blocked inside esp_http_client_perform() which has a
  // timeout of ONVIF_SUBSCRIBE_TIMEOUT_MS (5 s). Give it enough time to
  // finish and clean up its HTTP client handles to avoid memory leaks.
  if (s_poll_task_handle) {
    for (int i = 0; i < 80 && s_poll_task_handle != NULL; i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_poll_task_handle) {
      // Task is still alive — it's likely stuck in a network call.
      // Do NOT force-delete: that would leak esp_http_client handles and
      // TLS buffers. Let it exit on its own when the HTTP timeout fires.
      ESP_LOGW(TAG, "Poll task still running after 8s, it will self-terminate");
    }
  }

  // Mark all slots inactive
  for (int i = 0; i < MAX_CAMERAS; i++) {
    s_cameras[i].state = CAM_STATE_INACTIVE;
  }

  ESP_LOGI(TAG, "All stopped");
}

bool onvif_events_start_camera(uint8_t slot, const char *ip,
                               const char *username, const char *password,
                               uint32_t id_camera) {
  if (slot >= MAX_CAMERAS) {
    ESP_LOGW(TAG, "Invalid slot %d", slot);
    return false;
  }
  if (!ip || ip[0] == '\0' || id_camera == 0) {
    ESP_LOGW(TAG, "Invalid params for slot %d", slot);
    return false;
  }
  if (s_mutex == NULL) {
    ESP_LOGE(TAG, "ONVIF module not initialized");
    return false;
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);

  cam_ctx_t *cam = &s_cameras[slot];

  // Save the old HTTP client handle — we must NOT destroy it here because
  // the poll task may be using it right now in esp_http_client_perform().
  // The poll task will detect the NULL handle and clean up on its own.
  esp_http_client_handle_t old_client = cam->http_client;

  // If already active, send unsubscribe (but skip client destruction)
  if (cam->state != CAM_STATE_INACTIVE) {
    // Detach the client before unsubscribe so unsubscribe doesn't free it
    cam->http_client = NULL;
    unsubscribe(cam);
  }

  // Initialize context (zeros http_client pointer too)
  memset(cam, 0, sizeof(cam_ctx_t));
  strncpy(cam->ip, ip, sizeof(cam->ip) - 1);
  strncpy(cam->user, username ? username : "", sizeof(cam->user) - 1);
  strncpy(cam->pass, password ? password : "", sizeof(cam->pass) - 1);
  cam->id_camera = id_camera;
  cam->slot = slot;
  cam->backoff_ms = ONVIF_BACKOFF_INITIAL_MS;

  // Now it's safe to destroy the old client — the poll task holds a local
  // copy but will detect cam->http_client == NULL after perform() returns
  // and will not use the handle further. We can free it here because
  // perform() has either not started yet (poll task blocked on mutex or
  // scheduler) or has returned with an error (socket closed).
  // However, if perform() is mid-flight we'd corrupt it. So instead,
  // we leave cleanup to the poll task which holds the local copy.
  // The poll task will free it when it detects the NULL.
  (void)old_client; // Poll task will clean up via its local copy

  // Load cached event service path from NVS
  {
    nvs_handle_t h;
    if (nvs_open("camera", NVS_READONLY, &h) == ESP_OK) {
      char key[16];
      size_t len = sizeof(cam->event_svc_path);
      snprintf(key, sizeof(key), "evsvc%d", slot);
      nvs_get_str(h, key, cam->event_svc_path, &len);
      nvs_close(h);
    }
  }

  // Mark as OFFLINE with immediate due time — the poll task (which has
  // sufficient stack for SOAP/HTTP) will pick it up and subscribe.
  // NOTE: We must NOT call full_subscribe() here because this function may
  // be called from the pkt_proc task which only has 4 KB of stack, and
  // full_subscribe() allocates ~2.5 KB of stack buffers (SOAP + WSSE).
  cam->state = CAM_STATE_OFFLINE;
  cam->next_poll_due = 0; // due immediately on next poll cycle

  xSemaphoreGive(s_mutex);

  ESP_LOGI(TAG, "[%d] Added (camera 0x%08lX, IP=%s)", slot,
           (unsigned long)id_camera, ip);
  return true;
}

void onvif_events_init(void) {
  ESP_LOGI(TAG, "Initializing...");

  // Create mutex
  if (s_mutex == NULL) {
    s_mutex = xSemaphoreCreateMutex();
    assert(s_mutex != NULL);
  }

  s_task_stop_requested = false;

  // Load cameras from NVS
  nvs_handle_t h;
  if (nvs_open("camera", NVS_READONLY, &h) == ESP_OK) {
    for (uint8_t slot = 0; slot < MAX_CAMERAS; slot++) {
      char key[16];
      uint32_t id_camera = 0;
      char ip[17] = {0};
      char user[17] = {0};
      char pass[17] = {0};

      snprintf(key, sizeof(key), "id_cam%d", slot);
      if (nvs_get_u32(h, key, &id_camera) != ESP_OK || id_camera == 0)
        continue;

      size_t len = sizeof(ip);
      snprintf(key, sizeof(key), "ip%d", slot);
      if (nvs_get_str(h, key, ip, &len) != ESP_OK || ip[0] == '\0')
        continue;

      len = sizeof(user);
      snprintf(key, sizeof(key), "user%d", slot);
      nvs_get_str(h, key, user, &len);

      len = sizeof(pass);
      snprintf(key, sizeof(key), "pass%d", slot);
      nvs_get_str(h, key, pass, &len);

      // Populate context (don't subscribe yet — task will do it)
      cam_ctx_t *cam = &s_cameras[slot];
      memset(cam, 0, sizeof(cam_ctx_t));
      strncpy(cam->ip, ip, sizeof(cam->ip) - 1);
      strncpy(cam->user, user, sizeof(cam->user) - 1);
      strncpy(cam->pass, pass, sizeof(cam->pass) - 1);
      cam->id_camera = id_camera;
      cam->slot = slot;
      cam->state = CAM_STATE_OFFLINE; // task will subscribe on start
      cam->backoff_ms = ONVIF_BACKOFF_INITIAL_MS;
      cam->next_poll_due = 0;

      // Load cached event path
      len = sizeof(cam->event_svc_path);
      snprintf(key, sizeof(key), "evsvc%d", slot);
      nvs_get_str(h, key, cam->event_svc_path, &len);

      ESP_LOGI(TAG, "Boot: slot %d (camera 0x%08lX, IP=%s)", slot,
               (unsigned long)id_camera, ip);
    }
    nvs_close(h);
  }

  // Start the poll task
  BaseType_t ret = xTaskCreate(onvif_poll_task, "onvif_poll", 6144, NULL, 5,
                               &s_poll_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create poll task");
    return;
  }

  ESP_LOGI(TAG, "Init complete");
}

uint8_t onvif_events_active_camera_count(void) {
  uint8_t count = 0;
  for (int i = 0; i < MAX_CAMERAS; i++) {
    if (s_cameras[i].state != CAM_STATE_INACTIVE)
      count++;
  }
  return count;
}

uint8_t onvif_events_get_online_bitmask(void) {
  uint8_t bitmask = 0;
  for (int i = 0; i < MAX_CAMERAS; i++) {
    if (s_cameras[i].state == CAM_STATE_WATCHING ||
        s_cameras[i].state == CAM_STATE_TRIGGERED) {
      bitmask |= (1 << i);
    }
  }
  return bitmask;
}
