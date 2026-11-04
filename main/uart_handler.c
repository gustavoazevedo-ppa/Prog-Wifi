#include "uart_handler.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/semphr.h"
#include "packet_actions.h"
#include "packet_manager.h"


#define TAG "UARTH"

#define PREPARE_BUF_MAX_SIZE 1024

#define UART_NUM UART_NUM_2 // Usando UART2
#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)
#define BUF_SIZE (1024) // Tamanho do buffer
#define PACKET_HEADER 0xAAAA
#define PACKET_TAIL 0xBBBB

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0xFB // 251 bytes

// FIFO and mutex (internal to module)
static QueueHandle_t uart_queue;
static uint8_t fifo_buffer[BUF_SIZE];
static int fifo_index = 0;
static SemaphoreHandle_t fifo_mutex;

void uart_event_task(void *arg) {
  uart_event_t event;
  static uint8_t data[BUF_SIZE];
  uint8_t noCmdCounter = 0;

  // Subscribe this task to the Task Watchdog Timer
  esp_task_wdt_add(NULL);

  while (1) {
    // Feed watchdog every iteration (loop runs at least every 5s due to queue timeout)
    esp_task_wdt_reset();

    if (xQueueReceive(uart_queue, &event,
                      pdMS_TO_TICKS(5000))) // portMAX_DELAY, pdMS_TO_TICKS(500)
    {
      if (event.type == UART_DATA) {
        int len = uart_read_bytes(UART_NUM, data, event.size, portMAX_DELAY);

        if (xSemaphoreTake(fifo_mutex, portMAX_DELAY)) { // Take the mutex
          if (fifo_index + len >= BUF_SIZE) {
            int overflow = (fifo_index + len) - (BUF_SIZE - 1);
            ESP_LOGW(TAG, "FIFO buffer full! Discarding %d oldest bytes.",
                     overflow);

            // Shift buffer left to discard old bytes
            memmove(fifo_buffer, fifo_buffer + overflow, BUF_SIZE - overflow);
            fifo_index -= overflow;
          }
          memcpy(&fifo_buffer[fifo_index], data, len);
          fifo_index += len;
          xSemaphoreGive(fifo_mutex); // Release the mutex
        }

        // Search for packet in fifo_buffer
        int i = 0;
        while (fifo_index >= 4 && i <= fifo_index - 4) {
          // Look for header
          if (fifo_buffer[i] == 0xAA && fifo_buffer[i + 1] == 0xAA) {
            // Validate packet_type field to reject false headers
            if (i + 3 < fifo_index) {
              uint8_t pt = fifo_buffer[i + 3];
              if (pt != PKT_TYPE_BLE && pt != PKT_TYPE_CLOUD &&
                  pt != PKT_TYPE_CONFIG && pt != PKT_TYPE_CONTACTID) {
                ++i;
                continue;
              }
            }
            // Look for tail after header
            bool packet_processed = false;
            for (int j = i + 2; j <= fifo_index - 2; ++j) {
              if (fifo_buffer[j] == 0xBB && fifo_buffer[j + 1] == 0xBB) {

                int length = j + 2 - i;

                // Validate packet length using 3rd byte
                uint16_t packet_length = fifo_buffer[i + 2];

                int expected_length = 0;

                uint8_t packet_type = fifo_buffer[i + 3];

                if (packet_type == PKT_TYPE_CONFIG ||
                    packet_type == PKT_TYPE_CONTACTID) // plaintext packets use
                                                       // actual length
                {
                  expected_length = packet_length;
                } else {
                  // Crypt packet length rules

                  if (packet_length <= 16) {
                    expected_length = 38;
                  } else if (packet_length >= 17 && packet_length <= 32) {
                    expected_length = 54;
                  } else if (packet_length >= 33 && packet_length <= 48) {
                    expected_length = 70;
                  } else if (packet_length >= 49 && packet_length <= 64) {
                    expected_length = 86;
                  } else if (packet_length >= 65 && packet_length <= 80) {
                    expected_length = 102;
                  } else if (packet_length >= 81 && packet_length <= 96) {
                    expected_length = 118;
                  } else if (packet_length >= 97 && packet_length <= 112) {
                    expected_length = 134;
                  } else if (packet_length >= 113 && packet_length <= 128) {
                    expected_length = 150;
                  } else if (packet_length >= 129 && packet_length <= 144) {
                    expected_length = 166;
                  } else if (packet_length >= 145 && packet_length <= 160) {
                    expected_length = 182;
                  } else if (packet_length >= 161 && packet_length <= 176) {
                    expected_length = 198;
                  } else if (packet_length >= 177 && packet_length <= 192) {
                    expected_length = 214;
                  } else if (packet_length >= 193 && packet_length <= 208) {
                    expected_length = 230;
                  } else if (packet_length >= 209 && packet_length <= 224) {
                    expected_length = 246;
                  }

                  if (packet_type == PKT_TYPE_BLE) // if packet type is command,
                                                   // use fixed length rules
                  {
                    expected_length -=
                        16; // BLE packets have 16 bytes less data (JOB ID)
                  }
                }

                // Keep incomplete packets in buffer and wait for more data
                if (length < expected_length) {
                  ESP_LOGW(TAG,
                           "Incomplete packet detected (length %d, expected "
                           "%d). Waiting for more data...",
                           length, expected_length);
                  continue; // Keep data and wait
                }

                int pkt_len = length; // value from get_packet
                uint8_t *heap_buf = pvPortMalloc(pkt_len);

                if (heap_buf) {
                  memcpy(heap_buf, &fifo_buffer[i],
                         pkt_len); // packet_buffer holds the packet returned by
                                   // get_packet
                  packet_t pkt = {.source = PACKET_SOURCE_UART,
                                  .data = heap_buf,
                                  .length = pkt_len,
                                  .packet_type = heap_buf[3]};
                  if (!packet_manager_submit(&pkt, 0)) {
                    vPortFree(heap_buf); // queue full / failed
                  }
                } else {
                  ESP_LOGE(TAG, "Failed to allocate memory for UART packet");
                }

                // Remove processed packet from buffer
                memmove(fifo_buffer, fifo_buffer + j + 2, fifo_index - (j + 2));
                fifo_index -= (j + 2);
                i = 0; // Restart search from beginning
                packet_processed = true;

                break;
              }
            }
            if (!packet_processed) {
              break; // Header found but no valid tail, wait for more data
            }
          } else {
            ++i;
          }
        }
      }
    } else {
      if (!isCmd40_received()) {
        ESP_LOGW(TAG, "No CMD40 data received within timeout.");
        uint8_t response[] = {0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                              PKT_CMD_40, 0x00, 0x00, 0xBB, 0xBB};

        response[7] =
            crc8(&response[0],
                 7); // Calculate CRC for header, length, packet_type and cmd

        uart_handler_write_bytes(response, sizeof(response));
      } else if (!isCmd43_received() &&
                 noCmdCounter <
                     3) // Wait a bit more for CMD43, which may come after CMD40
      {
        ESP_LOGW(TAG, "No CMD43 data received within timeout.");
        uint8_t response[] = {
            0xAA,       0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
            PKT_CMD_43, 0x01, // 0x01 means a response is expected, since this
                              // is a query packet
            0x00,       0xBB, 0xBB};

        response[7] =
            crc8(&response[0],
                 7); // Calculate CRC for header, length, packet_type and cmd

        uart_handler_write_bytes(response, sizeof(response));
        noCmdCounter++;
      } else if (!isCmd10_received() &&
                 noCmdCounter < 6) // Poll for CMD10H (MONIP config)
      {
        ESP_LOGW(TAG, "No CMD10H data received within timeout.");
        uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, PKT_TYPE_CONTACTID,
                              PKT_CMD_20H, 0x00, 0x00, 0xBB, 0xBB};

        response[7] = crc8(&response[0], 7); // CRC over header through CMD

        uart_handler_write_bytes(response, sizeof(response));
        noCmdCounter++;
      } else if (!isCmd46_received() &&
                 noCmdCounter < 9) // Poll for CMD46 (camera configs)
      {
        ESP_LOGW(TAG, "No CMD46 data received within timeout.");
        uint8_t response[] = {0xAA, 0xAA, 0x00, 0x01, PKT_TYPE_CONFIG,
                              PKT_CMD_49, 0x01, 0x00, 0xBB, 0xBB};

        response[7] = crc8(&response[0], 7); // CRC over header through CMD

        uart_handler_write_bytes(response, sizeof(response));
        noCmdCounter++;
      }
    }
  }
}

void uart_handler_init() {
  fifo_mutex = xSemaphoreCreateMutex();
  if (fifo_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create uart fifo mutex");
    return;
  }

  uart_config_t uart_config = {.baud_rate = 115200,
                               .data_bits = UART_DATA_8_BITS,
                               .parity = UART_PARITY_DISABLE,
                               .stop_bits = UART_STOP_BITS_1,
                               .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};
  uart_param_config(UART_NUM, &uart_config);
  uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
  gpio_set_pull_mode(RXD_PIN, GPIO_PULLUP_ONLY);
  uart_driver_install(UART_NUM, BUF_SIZE, BUF_SIZE, 10, &uart_queue, 0);

  if (xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 10, NULL) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create uart_event_task - host communication is DOWN");
  }
}

void uart_handler_write_bytes(const void *data, size_t len) {
  uart_write_bytes(UART_NUM, (const char *)data, len);
}