/*
 * Selector protocol wire layer: frame parser, checksum and message encode / decode.
 * Pure C, no ESP-IDF dependency.
 * Protocol reference: docs/selector-protocol.md. Design: docs/selector-bridge-design.md.
 */
#ifndef SELECTOR_WIRE_H
#define SELECTOR_WIRE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SELECTOR_START 0x7E
#define SELECTOR_STOP 0x81     // the default tail
#define SELECTOR_STOP_ALT 0x51 // legacy tail (decimal 81 by mistake), seen on some centrals in the field
#define SELECTOR_ID 0x01

#define SELECTOR_MIN_FRAME 6   // start, size, id, function, checksum, stop
#define SELECTOR_MAX_FRAME 64
#define SELECTOR_LCD_COLS 16
#define SELECTOR_LCD_ROWS 2
#define SELECTOR_SCREEN_FRAME 38 // 4 header + 32 text + checksum + stop
#define SELECTOR_REPLY_FRAME 8
#define SELECTOR_KEYS_IDLE_BYTE 0xB4

#define SELECTOR_FUNC_SCREEN 'S'
#define SELECTOR_FUNC_POLL '?'
#define SELECTOR_FUNC_BEEP 'B'
#define SELECTOR_FUNC_BACKLIGHT 'L'

// Canonical key bits (1 = pressed). The wire is active-low.
//  - MINUS / PLUS: Data0 bit 7 / bit 4, the "-" and "+" keys.
//  - OK / SEL: Data1 bit 7 / bit 4. On some selectors the same two keys are labelled with
//    arrows: OK = "<-" (left arrow), SEL = "->" (right arrow). Confirmed on a real selector.
// See docs/selector-protocol.md section 4.
#define SELECTOR_KEY_MINUS 0x01
#define SELECTOR_KEY_PLUS 0x02
#define SELECTOR_KEY_OK 0x04 //Also named Left Arrow
#define SELECTOR_KEY_SEL 0x08 //Also named Right Arrow

typedef enum
{
    SELECTOR_DROP_NONE = 0,
    SELECTOR_DROP_BAD_SIZE,
    SELECTOR_DROP_BAD_STOP,
    SELECTOR_DROP_BAD_CHECKSUM,
    SELECTOR_DROP_BAD_ID,
    SELECTOR_DROP_BAD_LENGTH,
    SELECTOR_DROP_UNKNOWN_FUNCTION,
    SELECTOR_DROP_PARTIAL_TIMEOUT,
    SELECTOR_DROP_REASON_COUNT
} selector_drop_reason_t;

// Downstream = central to selector (screen, poll, beep, backlight).
// Upstream = selector to central (key state).
typedef enum
{
    SELECTOR_DIR_DOWNSTREAM = 0,
    SELECTOR_DIR_UPSTREAM
} selector_dir_t;

typedef struct
{
    uint8_t bytes[SELECTOR_MAX_FRAME];
    uint8_t len;
} selector_frame_t;

typedef struct
{
    char row[SELECTOR_LCD_ROWS][SELECTOR_LCD_COLS]; // space padded, not NUL terminated
} selector_screen_t;

typedef enum
{
    SELECTOR_MSG_SCREEN = 0,
    SELECTOR_MSG_POLL,
    SELECTOR_MSG_BEEP,
    SELECTOR_MSG_BACKLIGHT,
    SELECTOR_MSG_KEYS
} selector_msg_type_t;

typedef struct
{
    selector_msg_type_t type;
    union
    {
        selector_screen_t screen;
        struct
        {
            uint8_t on_ms;
            uint8_t off_ms;
            uint8_t count;
        } beep;
        struct
        {
            uint8_t level;
        } backlight;
        struct
        {
            uint8_t pressed; // SELECTOR_KEY_* bits
        } keys;
    } u;
} selector_msg_t;

typedef enum
{
    SELECTOR_DECODE_MSG = 0, // msg is filled
    SELECTOR_DECODE_ALIVE,   // valid reply that carries no key state (replies to 'B' / 'L')
    SELECTOR_DECODE_DROP     // *reason says why
} selector_decode_result_t;

// Checksum over frame[0 .. len-3] (XOR, complemented). len includes checksum and stop.
uint8_t selector_checksum(const uint8_t *frame, size_t len);

// Checks the checksum against the known-good vector and a poll frame round trip.
bool selector_selftest(void);

// Byte-fed frame parser. Bounded: size is range checked before waiting for bytes, bad
// frames resync on the next start byte, partial frames time out.
typedef struct
{
    uint8_t buf[SELECTOR_MAX_FRAME];
    uint8_t count;
    uint8_t id;
    uint32_t partial_timeout_ms;
    uint32_t last_byte_ms;
    uint32_t drops[SELECTOR_DROP_REASON_COUNT];
} selector_framer_t;

void selector_framer_init(selector_framer_t *f, uint8_t id, uint32_t partial_timeout_ms);

// Feeds one byte. Returns true and fills out when a valid frame is complete. After true,
// call selector_framer_next() until it returns false (a resync may expose more frames).
bool selector_framer_feed(selector_framer_t *f, uint8_t byte, uint32_t now_ms, selector_frame_t *out);
bool selector_framer_next(selector_framer_t *f, selector_frame_t *out);

// Decodes a valid frame. For a short 'S' frame the text is written over *base (the previous
// screen, or spaces when base is NULL), as the real selector is assumed to do (unverified).
selector_decode_result_t selector_decode(const selector_frame_t *frame, selector_dir_t dir,
                                         const selector_screen_t *base, selector_msg_t *msg,
                                         selector_drop_reason_t *reason);

// Encodes a downstream message (SCREEN, POLL, BEEP, BACKLIGHT) into out, which needs
// SELECTOR_MAX_FRAME bytes. Returns the frame length, or 0 if the message is not encodable.
size_t selector_encode(const selector_msg_t *msg, uint8_t *out);

// Encodes the 8-byte reply to a command: the function echoed plus the key state. stop is the
// last byte of the frame being answered (SELECTOR_STOP or SELECTOR_STOP_ALT): a central that
// sends the legacy 0x51 tail needs the answer with 0x51. Any other value gives SELECTOR_STOP.
// Frames the Prog originates (toward the real selector, which accepts both) always use SELECTOR_STOP.
size_t selector_encode_reply(uint8_t function, uint8_t pressed, uint8_t stop, uint8_t *out);

#endif // SELECTOR_WIRE_H
