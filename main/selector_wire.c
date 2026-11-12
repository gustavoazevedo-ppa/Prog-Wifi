/*
 * Selector protocol wire layer. See selector_wire.h.
 */

#include <string.h>
#include "selector_wire.h"

// Wire key bits (active low): Data0 = MINUS / PLUS levels, Data1 = OK ("<-") / SEL ("->") levels
#define WIRE_MINUS_LEVEL 0x80
#define WIRE_PLUS_LEVEL 0x10
#define WIRE_OK_LEVEL 0x80
#define WIRE_SEL_LEVEL 0x10

uint8_t selector_checksum(const uint8_t *frame, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i + 2 < len; i++)
    {
        x ^= frame[i];
    }
    return x ^ 0xFF;
}

static void frame_begin(uint8_t *out, size_t len, uint8_t function)
{
    out[0] = SELECTOR_START;
    out[1] = (uint8_t)len;
    out[2] = SELECTOR_ID;
    out[3] = function;
}

static size_t frame_finish(uint8_t *out, size_t len)
{
    out[len - 2] = selector_checksum(out, len);
    out[len - 1] = SELECTOR_STOP;
    return len;
}

static uint8_t keys_from_wire(uint8_t d0, uint8_t d1)
{
    uint8_t pressed = 0;
    if (!(d0 & WIRE_MINUS_LEVEL))
    {
        pressed |= SELECTOR_KEY_MINUS;
    }
    if (!(d0 & WIRE_PLUS_LEVEL))
    {
        pressed |= SELECTOR_KEY_PLUS;
    }
    if (!(d1 & WIRE_OK_LEVEL))
    {
        pressed |= SELECTOR_KEY_OK;
    }
    if (!(d1 & WIRE_SEL_LEVEL))
    {
        pressed |= SELECTOR_KEY_SEL;
    }
    return pressed;
}

// The pulse bits keep their idle value: their meaning is unverified.
static void keys_to_wire(uint8_t pressed, uint8_t *d0, uint8_t *d1)
{
    uint8_t a = SELECTOR_KEYS_IDLE_BYTE;
    uint8_t b = SELECTOR_KEYS_IDLE_BYTE;
    if (pressed & SELECTOR_KEY_MINUS)
    {
        a &= (uint8_t)~WIRE_MINUS_LEVEL;
    }
    if (pressed & SELECTOR_KEY_PLUS)
    {
        a &= (uint8_t)~WIRE_PLUS_LEVEL;
    }
    if (pressed & SELECTOR_KEY_OK)
    {
        b &= (uint8_t)~WIRE_OK_LEVEL;
    }
    if (pressed & SELECTOR_KEY_SEL)
    {
        b &= (uint8_t)~WIRE_SEL_LEVEL;
    }
    *d0 = a;
    *d1 = b;
}

size_t selector_encode(const selector_msg_t *msg, uint8_t *out)
{
    switch (msg->type)
    {
    case SELECTOR_MSG_SCREEN:
        frame_begin(out, SELECTOR_SCREEN_FRAME, SELECTOR_FUNC_SCREEN);
        memcpy(&out[4], msg->u.screen.row, SELECTOR_LCD_ROWS * SELECTOR_LCD_COLS);
        return frame_finish(out, SELECTOR_SCREEN_FRAME);

    case SELECTOR_MSG_POLL:
        frame_begin(out, 8, SELECTOR_FUNC_POLL);
        out[4] = SELECTOR_KEYS_IDLE_BYTE;
        out[5] = SELECTOR_KEYS_IDLE_BYTE;
        return frame_finish(out, 8);

    case SELECTOR_MSG_BEEP:
        frame_begin(out, 9, SELECTOR_FUNC_BEEP);
        out[4] = msg->u.beep.on_ms;
        out[5] = msg->u.beep.off_ms;
        out[6] = msg->u.beep.count;
        return frame_finish(out, 9);

    case SELECTOR_MSG_BACKLIGHT:
        frame_begin(out, 7, SELECTOR_FUNC_BACKLIGHT);
        out[4] = msg->u.backlight.level;
        return frame_finish(out, 7);

    default:
        return 0;
    }
}

size_t selector_encode_reply(uint8_t function, uint8_t pressed, uint8_t *out)
{
    frame_begin(out, SELECTOR_REPLY_FRAME, function);
    keys_to_wire(pressed, &out[4], &out[5]);
    return frame_finish(out, SELECTOR_REPLY_FRAME);
}

selector_decode_result_t selector_decode(const selector_frame_t *frame, selector_dir_t dir,
                                         const selector_screen_t *base, selector_msg_t *msg,
                                         selector_drop_reason_t *reason)
{
    memset(msg, 0, sizeof(*msg));
    *reason = SELECTOR_DROP_NONE;

    if (frame->len < SELECTOR_MIN_FRAME)
    {
        *reason = SELECTOR_DROP_BAD_LENGTH;
        return SELECTOR_DECODE_DROP;
    }

    uint8_t function = frame->bytes[3];
    const uint8_t *data = &frame->bytes[4];
    size_t n = (size_t)frame->len - SELECTOR_MIN_FRAME; // data bytes

    if (dir == SELECTOR_DIR_DOWNSTREAM)
    {
        switch (function)
        {
        case SELECTOR_FUNC_SCREEN:
            if (n < 1 || n > SELECTOR_LCD_ROWS * SELECTOR_LCD_COLS)
            {
                *reason = SELECTOR_DROP_BAD_LENGTH;
                return SELECTOR_DECODE_DROP;
            }
            msg->type = SELECTOR_MSG_SCREEN;
            if (base != NULL)
            {
                msg->u.screen = *base;
            }
            else
            {
                memset(msg->u.screen.row, ' ', sizeof(msg->u.screen.row));
            }
            memcpy(msg->u.screen.row, data, n);
            return SELECTOR_DECODE_MSG;

        case SELECTOR_FUNC_POLL:
            if (n != 2)
            {
                *reason = SELECTOR_DROP_BAD_LENGTH;
                return SELECTOR_DECODE_DROP;
            }
            msg->type = SELECTOR_MSG_POLL;
            return SELECTOR_DECODE_MSG;

        case SELECTOR_FUNC_BEEP:
            if (n != 3)
            {
                *reason = SELECTOR_DROP_BAD_LENGTH;
                return SELECTOR_DECODE_DROP;
            }
            msg->type = SELECTOR_MSG_BEEP;
            msg->u.beep.on_ms = data[0];
            msg->u.beep.off_ms = data[1];
            msg->u.beep.count = data[2];
            return SELECTOR_DECODE_MSG;

        case SELECTOR_FUNC_BACKLIGHT:
            if (n != 1)
            {
                *reason = SELECTOR_DROP_BAD_LENGTH;
                return SELECTOR_DECODE_DROP;
            }
            msg->type = SELECTOR_MSG_BACKLIGHT;
            msg->u.backlight.level = data[0];
            return SELECTOR_DECODE_MSG;

        default:
            *reason = SELECTOR_DROP_UNKNOWN_FUNCTION;
            return SELECTOR_DECODE_DROP;
        }
    }

    // Upstream: replies are 8 bytes (two key bytes)
    if (function != SELECTOR_FUNC_SCREEN && function != SELECTOR_FUNC_POLL &&
        function != SELECTOR_FUNC_BEEP && function != SELECTOR_FUNC_BACKLIGHT)
    {
        *reason = SELECTOR_DROP_UNKNOWN_FUNCTION;
        return SELECTOR_DECODE_DROP;
    }
    if (n != 2)
    {
        *reason = SELECTOR_DROP_BAD_LENGTH;
        return SELECTOR_DECODE_DROP;
    }
    if (function == SELECTOR_FUNC_SCREEN || function == SELECTOR_FUNC_POLL)
    {
        msg->type = SELECTOR_MSG_KEYS;
        msg->u.keys.pressed = keys_from_wire(data[0], data[1]);
        return SELECTOR_DECODE_MSG;
    }
    // Replies to 'B' and 'L' only count as link alive
    return SELECTOR_DECODE_ALIVE;
}

bool selector_selftest(void)
{
    static const uint8_t vec[8] = {0x7E, 0x08, 0x01, 0x3F, 0xB4, 0xB4, 0xB7, 0x81};
    uint8_t out[SELECTOR_MAX_FRAME];
    selector_msg_t poll;

    memset(&poll, 0, sizeof(poll));
    poll.type = SELECTOR_MSG_POLL;

    if (selector_checksum(vec, sizeof(vec)) != 0xB7)
    {
        return false;
    }
    if (selector_encode(&poll, out) != sizeof(vec) || memcmp(out, vec, sizeof(vec)) != 0)
    {
        return false;
    }
    return true;
}

void selector_framer_init(selector_framer_t *f, uint8_t id, uint32_t partial_timeout_ms)
{
    memset(f, 0, sizeof(*f));
    f->id = id;
    f->partial_timeout_ms = partial_timeout_ms;
}

static void framer_shift(selector_framer_t *f, uint8_t n)
{
    if (n >= f->count)
    {
        f->count = 0;
        return;
    }
    memmove(f->buf, f->buf + n, f->count - n);
    f->count = (uint8_t)(f->count - n);
}

static void framer_drop(selector_framer_t *f, selector_drop_reason_t reason)
{
    f->drops[reason]++;
}

bool selector_framer_next(selector_framer_t *f, selector_frame_t *out)
{
    while (f->count > 0)
    {
        // Bytes before a start byte are noise
        if (f->buf[0] != SELECTOR_START)
        {
            framer_shift(f, 1);
            continue;
        }
        if (f->count < 2)
        {
            return false;
        }

        // Range check the size before waiting for that many bytes: never stalls
        uint8_t size = f->buf[1];
        if (size < SELECTOR_MIN_FRAME || size > SELECTOR_MAX_FRAME)
        {
            framer_drop(f, SELECTOR_DROP_BAD_SIZE);
            framer_shift(f, 1);
            continue;
        }
        if (f->count < size)
        {
            return false;
        }

        uint8_t stop = f->buf[size - 1];
        if (stop != SELECTOR_STOP && stop != SELECTOR_STOP_ALT)
        {
            framer_drop(f, SELECTOR_DROP_BAD_STOP);
            framer_shift(f, 1);
            continue;
        }
        if (selector_checksum(f->buf, size) != f->buf[size - 2])
        {
            framer_drop(f, SELECTOR_DROP_BAD_CHECKSUM);
            framer_shift(f, 1);
            continue;
        }
        if (f->buf[2] != f->id)
        {
            // A well formed frame for another device: skip all of it
            framer_drop(f, SELECTOR_DROP_BAD_ID);
            framer_shift(f, size);
            continue;
        }

        memcpy(out->bytes, f->buf, size);
        out->len = size;
        framer_shift(f, size);
        return true;
    }
    return false;
}

bool selector_framer_feed(selector_framer_t *f, uint8_t byte, uint32_t now_ms, selector_frame_t *out)
{
    if (f->count > 0 && (uint32_t)(now_ms - f->last_byte_ms) > f->partial_timeout_ms)
    {
        framer_drop(f, SELECTOR_DROP_PARTIAL_TIMEOUT);
        f->count = 0;
    }
    f->last_byte_ms = now_ms;

    if (f->count >= SELECTOR_MAX_FRAME)
    {
        f->count = 0; // defensive: cannot happen, the size check bounds the buffer
    }
    f->buf[f->count++] = byte;
    return selector_framer_next(f, out);
}
