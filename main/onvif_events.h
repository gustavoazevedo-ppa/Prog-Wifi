#ifndef ONVIF_EVENTS_H
#define ONVIF_EVENTS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ─── Configuration ───────────────────────────────────────────────────────────

#define MAX_CAMERAS 8

// SOAP envelope buffer size (stack-allocated per request)
#define ONVIF_SOAP_BUF_SIZE 1536

// Maximum URL length for pullpoint subscription address
#define ONVIF_URL_MAX_LEN 192

// PullMessages timeout sent to camera
#define ONVIF_PULL_TIMEOUT_STR "PT1S"

// HTTP timeout for PullMessages (must be > pull timeout)
#define ONVIF_HTTP_TIMEOUT_MS 3000

// HTTP timeout for subscribe/unsubscribe operations
#define ONVIF_SUBSCRIBE_TIMEOUT_MS 5000

// HTTP receive buffer size — limits per-chunk heap usage during streaming.
// Larger values reduce SPI/TCP round-trips; smaller values protect heap.
#define ONVIF_HTTP_RX_BUF_SIZE 1024

// Maximum total response bytes we'll accept from a camera before aborting.
// Protects against cameras that send unexpectedly large XML payloads which
// would exhaust heap or flood the ENC28J60 buffer.
#define ONVIF_MAX_RESPONSE_SIZE 12288

// TCP probe timeout for checking if camera is reachable
#define ONVIF_TCP_PROBE_TIMEOUT_MS 1000

// Subscription lifetime requested (camera may ignore and give less)
#define ONVIF_SUBSCRIPTION_TIMEOUT_STR "PT10M"

// Consecutive PullMessages *network* failures before going OFFLINE.
// A network failure means the camera itself is unreachable (perform error or
// non-200). This bound is small: a genuinely down camera should be parked in
// OFFLINE backoff quickly.
#define ONVIF_MAX_PULL_FAILURES 2

// No-memory deferral tuning. A no-mem deferral is NOT the camera's fault — the
// module simply couldn't allocate the HTTP client this cycle. We keep the
// camera WATCHING and retry, rather than punishing it with OFFLINE backoff.
//
// Retry interval for a no-mem deferral (ms): slower than a normal WATCHING poll
// (so we don't spin and churn the heap while it's tight) but far faster than
// OFFLINE backoff (so motion detection resumes promptly once heap recovers).
#define ONVIF_NOMEM_RETRY_INTERVAL_MS 1500

// Consecutive no-mem deferrals before we give up and force the camera OFFLINE.
// This is deliberately large and independent of ONVIF_MAX_PULL_FAILURES: a
// transient heap dip is absorbed silently, but a *sustained* shortage must
// eventually surface to the host (via the CMD48 online bitmask) rather than
// leaving the camera silently blind while still reported "online".
// At 1500ms/retry, 40 deferrals ≈ 60s of continuous starvation before OFFLINE.
#define ONVIF_MAX_NOMEM_DEFERRALS 40

// Minimum interval between polls in WATCHING state (ms) — prevents buffer
// exhaustion on the ENC28J60 by spacing out HTTP transactions
#define ONVIF_WATCHING_POLL_INTERVAL_MS 150

// Consecutive no-motion responses before clearing motion_active flag
#define ONVIF_NO_MOTION_THRESHOLD 4

// TRIGGERED state poll interval (ms) — just checking for IDLE
#define ONVIF_TRIGGERED_POLL_INTERVAL_MS 3000

// OFFLINE backoff range (ms)
#define ONVIF_BACKOFF_INITIAL_MS 5000
#define ONVIF_BACKOFF_MAX_MS 60000

// Boot stagger between camera subscriptions (ms)
#define ONVIF_BOOT_STAGGER_MS 500

// Retry stagger per slot when multiple cameras go OFFLINE (ms)
#define ONVIF_RETRY_STAGGER_MS 500

// Event service paths to try (order: strict cameras first)
#define ONVIF_EVENT_PATH_0 "/onvif/event_service"
#define ONVIF_EVENT_PATH_1 "/onvif/Events"

// ─── Types ───────────────────────────────────────────────────────────────────

typedef enum {
  CAM_STATE_INACTIVE = 0, // Slot not configured
  CAM_STATE_WATCHING,     // Healthy, polling for new motion events (high priority)
  CAM_STATE_TRIGGERED,    // Motion fired, polling slowly for IDLE (low priority)
  CAM_STATE_OFFLINE,      // Unreachable, exponential backoff (lowest priority)
} cam_state_t;

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * Initialize the ONVIF events module.
 * Creates the poll task and loads camera configs from NVS.
 * Call after NVS init and network is ready.
 */
void onvif_events_init(void);

/**
 * Add or update a camera in the polling rotation.
 * If the slot already has a camera, it is replaced (unsubscribed first).
 *
 * @param slot      Camera slot index (0..7)
 * @param ip        Camera IP address string
 * @param username  HTTP username
 * @param password  HTTP password
 * @param id_camera Unique camera ID (used in CMD47 violation reports)
 * @return true on success
 */
bool onvif_events_start_camera(uint8_t slot, const char *ip,
                               const char *username, const char *password,
                               uint32_t id_camera);

/**
 * Remove a camera from the polling rotation and unsubscribe.
 */
void onvif_events_stop_camera(uint8_t slot);

/**
 * Stop all cameras and the poll task.
 */
void onvif_events_stop_all(void);

/**
 * Returns the number of cameras currently active (not INACTIVE).
 */
uint8_t onvif_events_active_camera_count(void);

/**
 * Returns a bitmask of cameras currently online (WATCHING or TRIGGERED).
 * Bit N set = camera slot N is online.
 */
uint8_t onvif_events_get_online_bitmask(void);

/**
 * Returns the total number of camera poll cycles that were deferred because the
 * module was out of heap. Monotonic since boot. A rising value indicates the
 * device is operating near its heap floor and shedding camera polls to protect
 * higher-priority paths. Surface in health telemetry for observability.
 */
uint32_t onvif_events_get_nomem_defer_count(void);

#endif // ONVIF_EVENTS_H
