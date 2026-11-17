/*
 * Selector macros: file validator, run-length encoder (key mask per counted reply to tokens)
 * and script iterator. Pure C, no ESP-IDF dependency.
 * File format and rules: docs/selector-bridge-design.md section 7c "File format".
 *
 *   #MODEL X/V1.2
 *   3 P 2 p 4 S 1 s 10 O 2 o 30
 *
 * A number is a count of replies sent with the current key state (two in a row add up). A letter
 * is a key token (M P O S press, m p o s release) that takes effect on the next reply.
 */
#ifndef SELECTOR_MACRO_H
#define SELECTOR_MACRO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SELECTOR_MACRO_MAX 2048    // same as PRESET_BIN_MAX, the preset scratchpad
#define SELECTOR_MACRO_NUM_MAX 65535
#define SELECTOR_MACRO_POPUP_MAX 40 // buffer for a normalized pop-up (two rows of 16 plus "/" plus NUL)

typedef enum
{
    SELECTOR_MACRO_OK = 0,
    SELECTOR_MACRO_ERR_TOO_LONG, // more than SELECTOR_MACRO_MAX bytes
    SELECTOR_MACRO_ERR_NO_HEADER,
    SELECTOR_MACRO_ERR_BAD_CHAR, // a character or token that is not part of the format
    SELECTOR_MACRO_ERR_BAD_NUMBER // outside 1 to 65535
} selector_macro_result_t;

// Checks a whole macro file. An optional UTF-8 BOM and leading blanks before the '#' are
// accepted. The length limit is checked before any byte is read.
selector_macro_result_t selector_macro_validate(const uint8_t *data, size_t len);

// Normalizes a start screen (two rows of 16, space padded, not NUL terminated) to the pop-up
// form: each row trimmed of leading and trailing spaces, empty rows dropped, rows joined with
// '/', bytes outside printable ASCII replaced by '?'. Writes a NUL terminated string and returns
// its length (0 for a blank screen). cap must be at least SELECTOR_MACRO_POPUP_MAX.
size_t selector_macro_normalize_popup(const char rows[2][16], char *out, size_t cap);

// Run-length encoder. Feed it the key mask sent in each counted reply, then finish.
typedef struct
{
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint8_t mask;  // key mask of the current run (starts released)
    uint32_t run;  // replies in the current run, not written yet
    bool full;     // a token did not fit: nothing more is written
} selector_macro_enc_t;

// Starts a macro in buf with the header line "#<popup>\n". Returns false if it does not fit.
bool selector_macro_enc_begin(selector_macro_enc_t *e, uint8_t *buf, size_t cap, const char *popup);

// Adds one counted reply. Returns false once the buffer is full (the reply was not recorded
// and everything up to the last complete token is kept).
bool selector_macro_enc_reply(selector_macro_enc_t *e, uint8_t mask);

// Writes the trailing wait and the release of keys still held. Returns the macro length.
size_t selector_macro_enc_finish(selector_macro_enc_t *e);

// Script iterator over a validated macro.
typedef struct
{
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint8_t mask;       // key state for the next reply
    uint32_t remaining; // replies still owed with the current state
} selector_macro_iter_t;

// Positions the iterator after the header line. Returns false if there is no header.
bool selector_macro_iter_init(selector_macro_iter_t *it, const uint8_t *data, size_t len);

// Gives the key mask for the next counted reply. Returns false when the script has ended.
bool selector_macro_iter_next(selector_macro_iter_t *it, uint8_t *mask);

// Fixed-vector self-test: encoder, validator, iterator round trip and rejected inputs.
bool selector_macro_selftest(void);

#endif // SELECTOR_MACRO_H
