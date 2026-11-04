/****************************************************************************
 * Central Firmware Slot Sender — implementation
 *
 * See fw_slot_sender.h for the protocol/format overview.
 ****************************************************************************/

#include "fw_slot_sender.h"
#include "cJSON.h"
#include "device_info.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "ota_button.h"     /* OTA_MANIFEST_URL convention / HW_VERSION */
#include "packet_manager.h" /* shared crc8() — same CRC-8/0x1D LUT */
#include "uart_handler.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TAG "FW_SLOT"

/* -------------------------------------------------------------------------
 * Tunables / limits
 * ------------------------------------------------------------------------- */

/* Manifest for the central firmware (per hardware version). Mirrors the OTA
 * manifest convention in ota_button.h, different filename. */
#define CENTRAL_FW_MANIFEST_URL                                                \
  "https://download.citrox.com.br/fw/hw" STRINGIFY(HW_VERSION) "/central.json"

#define FW_MAX_BYTES (2 * 1024 * 1024) /* hard cap on the encrypted blob */
#define FW_FILE_HEADER_SIZE 12         /* perm(4) + size(4 LE) + crc32(4 LE) */
#define FW_SLOT_SIZE 512 /* bytes per slot (2-byte BE LENGTH allows this) */

#define FW_TYPE_BYTE 0x02 /* "type"/id_user: plaintext/config family */
#define FW_HEADER_B0 0xAA
#define FW_HEADER_B1 0xAA
#define FW_TAIL_B0 0xBB
#define FW_TAIL_B1 0xBB

#define FW_ACK_TIMEOUT_MS 15000 /* per-packet ACK wait */
#define FW_MANIFEST_MAX_SIZE 1024
#define FW_HTTP_TIMEOUT_MS 15000 /* per-read socket timeout */
#define FW_TOTAL_DEADLINE_MS                                                   \
  (10 * 60 * 1000) /* wall-clock cap for the whole transfer */

/* Max outbound packet:
 * header(2)+len(2)+type(1)+cmd(1)+index(4)+slot(512)+crc8(1)+tail(2) */
#define FW_PACKET_MAX (2 + 2 + 1 + 1 + 4 + FW_SLOT_SIZE + 1 + 2)

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static volatile bool s_busy = false;
static QueueHandle_t s_ack_queue = NULL; /* carries the ACK status byte */
static fw_slot_status_cb_t s_status_cb = NULL;

static void fw_report(const char *status, const char *detail) {
  if (s_status_cb)
    s_status_cb(status, detail);
}

/* -------------------------------------------------------------------------
 * CRC8: the module reuses the project's shared crc8() from packet_manager
 * (CRC-8 with 0x1D poly, init 0xFF, XOR-out 0xFF). The central team's firmware-
 * update table is now standardized to this same LUT, so there is no separate
 * copy here.
 * ------------------------------------------------------------------------- */

/* Running CRC32 (zlib / CRC-32/ISO-HDLC, reflected, init+xor 0xFFFFFFFF) ported
 * from protocol.py calculate_crc32(). Chaining it across the slots yields the
 * same value as a single pass over the whole blob, so the final accumulator must
 * equal the header crc32 — this is the transfer's integrity verdict (gates the
 * MQTT success/failure report). The table is generated at first use to avoid a
 * static 1 KB LUT in the image. */
static uint32_t s_crc32_tab[256];
static bool s_crc32_tab_ready = false;

static void fw_crc32_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    s_crc32_tab[i] = c;
  }
  s_crc32_tab_ready = true;
}

/* Matches protocol.py: crc = (crc ^ 0xFFFFFFFF); fold bytes; return crc ^ 0xFFFFFFFF.
 * Call with initial=0 for the first slot, then feed the previous return value. */
static uint32_t fw_crc32_update(uint32_t initial, const uint8_t *data, size_t len) {
  if (!s_crc32_tab_ready)
    fw_crc32_init();
  uint32_t crc = initial ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
    crc = s_crc32_tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

/* -------------------------------------------------------------------------
 * Packet framing (module -> central): 2-byte BIG-endian LENGTH, type 0x02.
 * The payload (cmd + data) is in the clear; crc8 covers cmd + data.
 * ------------------------------------------------------------------------- */

/* Builds an envelope from an already-assembled payload-without-crc8.
 * length_field is the protocol LENGTH value (metadata = 0x0C, slot = 4+n).
 * Returns total bytes written into out (caller sizes out >= FW_PACKET_MAX). */
static size_t fw_wrap_envelope(uint8_t *out, const uint8_t *payload,
                               size_t payload_len, uint16_t length_field) {
  size_t n = 0;
  out[n++] = FW_HEADER_B0;
  out[n++] = FW_HEADER_B1;
  out[n++] = (uint8_t)(length_field >> 8);   /* BE high byte */
  out[n++] = (uint8_t)(length_field & 0xFF); /* BE low byte  */
  out[n++] = FW_TYPE_BYTE;
  memcpy(&out[n], payload, payload_len);
  n += payload_len;
  /* Shared crc8() over cmd + data. Cast away const: crc8() does not modify
   * the buffer; its signature predates const-correctness. */
  out[n++] = crc8((uint8_t *)payload, (uint16_t)payload_len);
  out[n++] = FW_TAIL_B0;
  out[n++] = FW_TAIL_B1;
  return n;
}

/* metadata payload: cmd(0x0A) + perm(4) + firmware_size(4 LE) + crc32(4 LE) */
static size_t fw_build_metadata(uint8_t *out, const uint8_t perm[4],
                                uint32_t firmware_size, uint32_t crc32) {
  uint8_t payload[1 + 4 + 4 + 4];
  size_t p = 0;
  payload[p++] = FW_SLOT_CMD;
  memcpy(&payload[p], perm, 4);
  p += 4;
  payload[p++] = (uint8_t)(firmware_size & 0xFF);
  payload[p++] = (uint8_t)((firmware_size >> 8) & 0xFF);
  payload[p++] = (uint8_t)((firmware_size >> 16) & 0xFF);
  payload[p++] = (uint8_t)((firmware_size >> 24) & 0xFF);
  payload[p++] = (uint8_t)(crc32 & 0xFF);
  payload[p++] = (uint8_t)((crc32 >> 8) & 0xFF);
  payload[p++] = (uint8_t)((crc32 >> 16) & 0xFF);
  payload[p++] = (uint8_t)((crc32 >> 24) & 0xFF);
  /* LENGTH = perm(4) + firmware_size(4) + crc32(4) = 0x0C (cmd/crc8 excluded)
   */
  return fw_wrap_envelope(out, payload, p, 0x000C);
}

/* slot payload: cmd(0x0A) + slot_index(4 LE) + slot_bytes */
static size_t fw_build_slot(uint8_t *out, uint32_t slot_index,
                            const uint8_t *slot_bytes, size_t n) {
  static uint8_t payload[1 + 4 + FW_SLOT_SIZE];
  size_t p = 0;
  payload[p++] = FW_SLOT_CMD;
  payload[p++] = (uint8_t)(slot_index & 0xFF);
  payload[p++] = (uint8_t)((slot_index >> 8) & 0xFF);
  payload[p++] = (uint8_t)((slot_index >> 16) & 0xFF);
  payload[p++] = (uint8_t)((slot_index >> 24) & 0xFF);
  memcpy(&payload[p], slot_bytes, n);
  p += n;
  /* LENGTH = slot_index(4) + len(slot_bytes) (cmd/crc8 excluded) */
  return fw_wrap_envelope(out, payload, p, (uint16_t)(4 + n));
}

/* -------------------------------------------------------------------------
 * ACK wait — drains the queue (fed by handle_config_packet via deliver_ack)
 * ------------------------------------------------------------------------- */

static esp_err_t fw_wait_ack(void) {
  uint8_t status = 0xFF;
  if (xQueueReceive(s_ack_queue, &status, pdMS_TO_TICKS(FW_ACK_TIMEOUT_MS)) !=
      pdTRUE) {
    ESP_LOGE(TAG, "Timeout (%d ms) waiting for ACK", FW_ACK_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
  }
  if (status != 0) {
    ESP_LOGE(TAG, "Central rejected packet (status=0x%02X)", status);
    return ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;
}

static void fw_send_packet(const uint8_t *pkt, size_t len) {
  /* Clear any stale ACK before sending so we only read this packet's reply. */
  xQueueReset(s_ack_queue);
  uart_handler_write_bytes(pkt, len);
}

/* -------------------------------------------------------------------------
 * Manifest fetch — reuses the generic download pattern (version check skipped)
 * ------------------------------------------------------------------------- */

static esp_err_t fw_download_manifest(char *out_buf, size_t buf_size) {
  esp_http_client_config_t config = {
      .url = CENTRAL_FW_MANIFEST_URL,
      .timeout_ms = 10000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 1024,
      .buffer_size_tx = 512,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client)
    return ESP_FAIL;

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Manifest open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }
  int content_length = esp_http_client_fetch_headers(client);
  if (content_length < 0 || (size_t)content_length >= buf_size) {
    ESP_LOGE(TAG, "Manifest too large or fetch failed (len=%d)",
             content_length);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_ERR_NO_MEM;
  }
  int read_len = esp_http_client_read(client, out_buf, buf_size - 1);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (read_len <= 0) {
    ESP_LOGE(TAG, "Failed to read manifest body");
    return ESP_FAIL;
  }
  out_buf[read_len] = '\0';
  return ESP_OK;
}

/* Parses central.json -> url (and optional hw guard). Caller supplies url_buf.
 * Version is intentionally ignored (the central firmware has no tracked
 * version). */
static esp_err_t fw_parse_manifest(const char *json, char *url_buf,
                                   size_t url_size) {
  cJSON *root = cJSON_Parse(json);
  if (!root) {
    ESP_LOGE(TAG, "Manifest JSON parse failed");
    return ESP_ERR_INVALID_RESPONSE;
  }

  cJSON *hw = cJSON_GetObjectItem(root, "hw");
  if (cJSON_IsNumber(hw) && hw->valueint != HW_VERSION) {
    ESP_LOGE(TAG, "Manifest hw=%d != device hw=%d", hw->valueint, HW_VERSION);
    cJSON_Delete(root);
    return ESP_ERR_INVALID_VERSION;
  }

  cJSON *ver =
      cJSON_GetObjectItem(root, "version"); /* logged only, not checked */
  if (cJSON_IsString(ver)) {
    ESP_LOGI(TAG, "Central firmware manifest version: %s (not checked)",
             ver->valuestring);
  }

  cJSON *url = cJSON_GetObjectItem(root, "url");
  if (!cJSON_IsString(url)) {
    ESP_LOGE(TAG, "Manifest missing 'url'");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }
  if (strlen(url->valuestring) >= url_size) {
    ESP_LOGE(TAG, "Manifest url too long");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_SIZE;
  }
  strcpy(url_buf, url->valuestring);
  cJSON_Delete(root);
  return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Read exactly `want` bytes from the HTTP stream (handles short reads / EOF).
 * Returns bytes read (== want on success, < want on EOF, -1 on error).
 * ------------------------------------------------------------------------- */

static int fw_read_full(esp_http_client_handle_t client, uint8_t *buf,
                        int want) {
  int got = 0;
  while (got < want) {
    int n = esp_http_client_read(client, (char *)buf + got, want - got);
    if (n < 0)
      return -1;
    if (n == 0)
      break; /* EOF */
    got += n;
  }
  return got;
}

/* -------------------------------------------------------------------------
 * Core: download the file and stream it to the central as metadata + slots.
 * ------------------------------------------------------------------------- */

static esp_err_t fw_do_transfer(const char *url) {
  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = FW_HTTP_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 1024,
      .buffer_size_tx = 1024,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client)
    return ESP_FAIL;

  esp_err_t ret = ESP_FAIL;
  int64_t t_start = esp_timer_get_time();

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "File open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }

  int content_length = esp_http_client_fetch_headers(client);

  /* ----- Size gate: cross-check header + Content-Length against the cap.
   * Reject BEFORE sending any metadata so the central never sees a bad file. */
  if (content_length >= 0) {
    if (content_length < FW_FILE_HEADER_SIZE) {
      ESP_LOGE(TAG, "File shorter than header (%d bytes)", content_length);
      ret = ESP_ERR_INVALID_SIZE;
      goto done;
    }
    long blob_len = (long)content_length - FW_FILE_HEADER_SIZE;
    if (blob_len > FW_MAX_BYTES) {
      ESP_LOGE(TAG, "File blob %ld bytes exceeds cap %d — abort", blob_len,
               FW_MAX_BYTES);
      ret = ESP_ERR_INVALID_SIZE;
      goto done;
    }
    if ((blob_len % 16) != 0) {
      ESP_LOGE(TAG, "Blob %ld not a multiple of 16 (AES block) — abort",
               blob_len);
      ret = ESP_ERR_INVALID_SIZE;
      goto done;
    }
  } else {
    ESP_LOGW(TAG, "No Content-Length (chunked?); relying on running cap");
  }

  /* ----- Read the 12-byte header. */
  uint8_t header[FW_FILE_HEADER_SIZE];
  if (fw_read_full(client, header, FW_FILE_HEADER_SIZE) !=
      FW_FILE_HEADER_SIZE) {
    ESP_LOGE(TAG, "Failed to read 12-byte file header");
    ret = ESP_FAIL;
    goto done;
  }
  uint8_t perm[4];
  memcpy(perm, &header[0], 4);
  uint32_t firmware_size = (uint32_t)header[4] | ((uint32_t)header[5] << 8) |
                           ((uint32_t)header[6] << 16) |
                           ((uint32_t)header[7] << 24);
  uint32_t crc32 = (uint32_t)header[8] | ((uint32_t)header[9] << 8) |
                   ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24);

  if (firmware_size == 0 || firmware_size > FW_MAX_BYTES) {
    ESP_LOGE(TAG, "Header firmware_size=%" PRIu32 " out of range — abort",
             firmware_size);
    ret = ESP_ERR_INVALID_SIZE;
    goto done;
  }
  ESP_LOGI(TAG,
           "Header: perm=%02X%02X%02X%02X size=%" PRIu32 " crc32=0x%08" PRIX32,
           perm[0], perm[1], perm[2], perm[3], firmware_size, crc32);

  /* ----- METADATA. */
  uint8_t pkt[FW_PACKET_MAX];
  size_t pkt_len = fw_build_metadata(pkt, perm, firmware_size, crc32);
  ESP_LOGI(TAG, "Sending metadata (%u bytes)...", (unsigned)pkt_len);
  fw_send_packet(pkt, pkt_len);
  ret = fw_wait_ack();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Metadata ACK failed: %s", esp_err_to_name(ret));
    goto done;
  }

  /* ----- SLOTS: stream the encrypted blob. */
  uint8_t slot_buf[FW_SLOT_SIZE];
  uint32_t index = 0;
  long total_blob = 0;
  uint32_t running_crc32 = 0; /* chained across slots == whole-blob CRC32 */
  for (;;) {
    /* wall-clock guard */
    if ((esp_timer_get_time() - t_start) / 1000 > FW_TOTAL_DEADLINE_MS) {
      ESP_LOGE(TAG, "Transfer exceeded wall-clock deadline — abort");
      ret = ESP_ERR_TIMEOUT;
      goto done;
    }
    int n = fw_read_full(client, slot_buf, FW_SLOT_SIZE);
    if (n < 0) {
      ESP_LOGE(TAG, "Stream read error");
      ret = ESP_FAIL;
      goto done;
    }
    if (n == 0)
      break; /* EOF */

    total_blob += n;
    if (total_blob > FW_MAX_BYTES) {
      ESP_LOGE(TAG, "Blob exceeded cap mid-stream (%ld) — abort", total_blob);
      ret = ESP_ERR_INVALID_SIZE;
      goto done;
    }

    pkt_len = fw_build_slot(pkt, index, slot_buf, (size_t)n);
    fw_send_packet(pkt, pkt_len);
    ret = fw_wait_ack();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Slot %" PRIu32 " ACK failed: %s", index,
               esp_err_to_name(ret));
      goto done;
    }
    /* Chain the running CRC32 (mirrors firmware_sender.py's per-slot trace). */
    running_crc32 = fw_crc32_update(running_crc32, slot_buf, (size_t)n);
    ESP_LOGI(TAG,
             "Slot %" PRIu32 " (%d bytes) OK — total %ld — CRC32 acc 0x%08" PRIX32,
             index, n, total_blob, running_crc32);
    index++;

    if (n < FW_SLOT_SIZE)
      break; /* short read == last slot */
  }

  if (total_blob == 0) {
    ESP_LOGE(TAG, "No blob bytes after header — abort");
    ret = ESP_ERR_INVALID_SIZE;
    goto done;
  }

  /* Download + relay finished; completion is logged as such. The terminal
   * success/failure verdict is the CRC32 check below. */
  ESP_LOGI(TAG, "Download complete: %" PRIu32 " slots, %ld blob bytes", index,
           total_blob);

  /* Integrity verdict: the chained running CRC32 must equal the header crc32.
   * This gates the MQTT success report (see fw_slot_task). */
  if (running_crc32 != crc32) {
    ESP_LOGE(TAG,
             "CRC32 MISMATCH: accumulated 0x%08" PRIX32
             " != header 0x%08" PRIX32 " — relayed bytes differ from header",
             running_crc32, crc32);
    ret = ESP_ERR_INVALID_CRC;
    goto done;
  }
  ESP_LOGI(TAG, "CRC32 OK: accumulated 0x%08" PRIX32 " matches header",
           running_crc32);
  ret = ESP_OK;

done:
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ret;
}

/* -------------------------------------------------------------------------
 * Task + public API
 * ------------------------------------------------------------------------- */

static void fw_slot_task(void *arg) {
  (void)arg;
  char manifest[FW_MANIFEST_MAX_SIZE];
  char url[256];

  esp_err_t err = fw_download_manifest(manifest, sizeof(manifest));
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Manifest: %s", manifest);
    err = fw_parse_manifest(manifest, url, sizeof(url));
  }
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Downloading central firmware from: %s", url);
    err = fw_do_transfer(url);
  }

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Central firmware update succeeded (CRC32 verified)");
    fw_report("central_done", "crc32_verified");
  } else {
    ESP_LOGE(TAG, "Central firmware update failed: %s", esp_err_to_name(err));
    fw_report("central_failed", esp_err_to_name(err));
  }

  s_busy = false;
  vTaskDelete(NULL);
}

void fw_slot_sender_start(void) {
  if (s_busy) {
    ESP_LOGW(TAG, "Central firmware update already in progress, ignoring");
    return;
  }
  if (s_ack_queue == NULL) {
    s_ack_queue = xQueueCreate(4, sizeof(uint8_t));
    if (s_ack_queue == NULL) {
      ESP_LOGE(TAG, "Failed to create ACK queue");
      return;
    }
  }
  s_busy = true;
  /* TLS needs a large stack (same budget OTA uses). */
  if (xTaskCreate(fw_slot_task, "fw_slot", 8192, NULL, 5, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create fw_slot task");
    s_busy = false;
  }
}

void fw_slot_sender_set_status_cb(fw_slot_status_cb_t cb) { s_status_cb = cb; }

void fw_slot_sender_deliver_ack(uint8_t status) {
  if (!s_busy || s_ack_queue == NULL)
    return;
  xQueueSend(s_ack_queue, &status, 0);
}

bool fw_slot_sender_busy(void) { return s_busy; }
