/*
 * Selector macros. See selector_macro.h.
 */

#include <stdio.h>
#include <string.h>
#include "selector_wire.h"
#include "selector_macro.h"

#define GROUP_MAX 48 // scratch for one number plus the key tokens of one change

// Key tokens in file order (the order tokens are written for a chord): B C E D
static const struct
{
    char press;
    char release;
    uint8_t bit;
} s_keys[] = {
    {'B', 'b', SELECTOR_KEY_MINUS}, // Baixo
    {'C', 'c', SELECTOR_KEY_PLUS},  // Cima
    {'E', 'e', SELECTOR_KEY_LEFT},    // Esquerda
    {'D', 'd', SELECTOR_KEY_RIGHT},   // Direita
};
#define KEY_COUNT (sizeof(s_keys) / sizeof(s_keys[0]))

typedef enum
{
    TOK_END = 0,
    TOK_NUM,
    TOK_PRESS,
    TOK_RELEASE,
    TOK_BAD_CHAR,
    TOK_BAD_NUM
} tok_t;

static bool is_blank(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Skips an optional BOM and blanks, requires '#', and leaves *pos at the end of the header line
static bool body_start(const uint8_t *d, size_t len, size_t *pos)
{
    size_t i = 0;
    if (len >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF)
    {
        i = 3;
    }
    while (i < len && is_blank(d[i]))
    {
        i++;
    }
    if (i >= len || d[i] != '#')
    {
        return false;
    }
    while (i < len && d[i] != '\r' && d[i] != '\n')
    {
        i++;
    }
    *pos = i;
    return true;
}

// Reads the next token. *val is the number (TOK_NUM) or the key bit (TOK_PRESS, TOK_RELEASE).
static tok_t next_token(const uint8_t *d, size_t len, size_t *pos, uint32_t *val)
{
    size_t i = *pos;

    while (i < len && is_blank(d[i]))
    {
        i++;
    }
    if (i >= len)
    {
        *pos = i;
        return TOK_END;
    }

    tok_t tok = TOK_BAD_CHAR;
    if (d[i] >= '0' && d[i] <= '9')
    {
        uint32_t n = 0;
        while (i < len && d[i] >= '0' && d[i] <= '9')
        {
            if (n <= SELECTOR_MACRO_NUM_MAX)
            {
                n = n * 10 + (uint32_t)(d[i] - '0'); // stays above the limit once it passes it
            }
            i++;
        }
        *val = n;
        tok = (n >= 1 && n <= SELECTOR_MACRO_NUM_MAX) ? TOK_NUM : TOK_BAD_NUM;
    }
    else
    {
        for (size_t k = 0; k < KEY_COUNT; k++)
        {
            if (d[i] == (uint8_t)s_keys[k].press || d[i] == (uint8_t)s_keys[k].release)
            {
                tok = d[i] == (uint8_t)s_keys[k].press ? TOK_PRESS : TOK_RELEASE;
                *val = s_keys[k].bit;
                i++;
                break;
            }
        }
    }

    // Blanks between tokens are optional: "3C2c" is 3, C, 2, c and "CD" is C, D. A number ends at
    // the first character that is not a digit, so "1 5" is two numbers (1 and 5, which add up).
    *pos = i;
    return tok;
}

selector_macro_result_t selector_macro_validate(const uint8_t *data, size_t len)
{
    if (len > SELECTOR_MACRO_MAX)
    {
        return SELECTOR_MACRO_ERR_TOO_LONG;
    }
    size_t pos = 0;
    if (data == NULL || !body_start(data, len, &pos))
    {
        return SELECTOR_MACRO_ERR_NO_HEADER;
    }
    while (1)
    {
        uint32_t val = 0;
        switch (next_token(data, len, &pos, &val))
        {
        case TOK_END:
            return SELECTOR_MACRO_OK;
        case TOK_BAD_NUM:
            return SELECTOR_MACRO_ERR_BAD_NUMBER;
        case TOK_BAD_CHAR:
            return SELECTOR_MACRO_ERR_BAD_CHAR;
        default:
            break;
        }
    }
}

size_t selector_macro_normalize_popup(const char rows[2][16], char *out, size_t cap)
{
    size_t n = 0;

    if (cap < SELECTOR_MACRO_POPUP_MAX)
    {
        if (cap > 0)
        {
            out[0] = '\0';
        }
        return 0;
    }
    for (int r = 0; r < 2; r++)
    {
        int first = 0;
        int last = 15;
        while (first <= last && rows[r][first] == ' ')
        {
            first++;
        }
        while (last >= first && rows[r][last] == ' ')
        {
            last--;
        }
        if (first > last)
        {
            continue; // empty row
        }
        if (n > 0)
        {
            out[n++] = '/';
        }
        for (int i = first; i <= last; i++)
        {
            unsigned char c = (unsigned char)rows[r][i];
            out[n++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
        }
    }
    out[n] = '\0';
    return n;
}

// ─── Encoder ────────────────────────────────────────────────────────────────

// Appends a number (split in chunks of at most 65535, which add up) to a scratch group
static bool group_add_number(char *g, size_t *n, uint32_t count)
{
    while (count > 0)
    {
        uint32_t part = count > SELECTOR_MACRO_NUM_MAX ? SELECTOR_MACRO_NUM_MAX : count;
        int w = snprintf(g + *n, GROUP_MAX - *n, "%s%u", *n > 0 ? " " : "", (unsigned)part);
        if (w < 0 || (size_t)w >= GROUP_MAX - *n)
        {
            return false;
        }
        *n += (size_t)w;
        count -= part;
    }
    return true;
}

// Writes the pending run, then one key token per key that differs between e->mask and
// new_mask. All or nothing: a group that does not fit marks the encoder full.
// Unless final, it keeps room for the closing group of enc_finish (separator, trailing wait of
// two number chunks, one release token per key held in new_mask), so a full buffer still ends
// with the wait and the releases.
static bool enc_flush(selector_macro_enc_t *e, uint8_t new_mask, bool final)
{
    char g[GROUP_MAX];
    size_t n = 0;

    if (!group_add_number(g, &n, e->run))
    {
        e->full = true;
        return false;
    }
    uint8_t changed = (uint8_t)(e->mask ^ new_mask);
    for (size_t k = 0; k < KEY_COUNT; k++)
    {
        if (changed & s_keys[k].bit)
        {
            if (n + 3 > GROUP_MAX)
            {
                e->full = true;
                return false;
            }
            if (n > 0)
            {
                g[n++] = ' ';
            }
            g[n++] = (new_mask & s_keys[k].bit) ? s_keys[k].press : s_keys[k].release;
        }
    }
    if (n == 0)
    {
        return true;
    }

    // A separator is needed unless this is the first token after the header line
    bool sep = e->len > 0 && e->buf[e->len - 1] != '\n';
    size_t limit = e->cap;
    if (!final)
    {
        size_t reserve = 13;
        for (size_t k = 0; k < KEY_COUNT; k++)
        {
            if (new_mask & s_keys[k].bit)
            {
                reserve += 2;
            }
        }
        limit = e->cap > reserve ? e->cap - reserve : 0;
    }
    if (e->len + n + (sep ? 1 : 0) > limit)
    {
        e->full = true;
        return false;
    }
    if (sep)
    {
        e->buf[e->len++] = ' ';
    }
    memcpy(e->buf + e->len, g, n);
    e->len += n;
    return true;
}

bool selector_macro_enc_begin(selector_macro_enc_t *e, uint8_t *buf, size_t cap, const char *popup)
{
    memset(e, 0, sizeof(*e));
    e->buf = buf;
    e->cap = cap;

    size_t plen = popup != NULL ? strlen(popup) : 0;
    if (cap < plen + 2)
    {
        e->full = true;
        return false;
    }
    buf[e->len++] = '#';
    memcpy(buf + e->len, popup, plen);
    e->len += plen;
    buf[e->len++] = '\n';
    return true;
}

bool selector_macro_enc_reply(selector_macro_enc_t *e, uint8_t mask)
{
    if (e->full)
    {
        return false;
    }
    if (mask == e->mask)
    {
        e->run++;
        return true;
    }
    if (!enc_flush(e, mask, false))
    {
        return false;
    }
    e->mask = mask;
    e->run = 1;
    return true;
}

size_t selector_macro_enc_finish(selector_macro_enc_t *e)
{
    // Also after the buffer filled up: run and mask still describe the last complete token, and
    // enc_flush kept room for this closing group.
    if (e->run > 0 || e->mask != 0)
    {
        // The trailing wait stays as recorded, and keys still held get their release token
        if (enc_flush(e, 0, true))
        {
            e->run = 0;
            e->mask = 0;
        }
    }
    return e->len;
}

// ─── Iterator ───────────────────────────────────────────────────────────────

bool selector_macro_iter_init(selector_macro_iter_t *it, const uint8_t *data, size_t len)
{
    memset(it, 0, sizeof(*it));
    it->data = data;
    it->len = len;
    return data != NULL && body_start(data, len, &it->pos);
}

bool selector_macro_iter_next(selector_macro_iter_t *it, uint8_t *mask)
{
    while (it->remaining == 0)
    {
        uint32_t val = 0;
        switch (next_token(it->data, it->len, &it->pos, &val))
        {
        case TOK_NUM:
            it->remaining = val;
            break;
        case TOK_PRESS:
            it->mask |= (uint8_t)val;
            break;
        case TOK_RELEASE:
            it->mask &= (uint8_t)~val;
            break;
        default: // end of script (a validated macro has no bad tokens)
            return false;
        }
    }
    it->remaining--;
    *mask = it->mask;
    return true;
}

// ─── Self-test ──────────────────────────────────────────────────────────────

static void st_feed(selector_macro_enc_t *e, uint8_t mask, uint32_t count)
{
    while (count-- > 0)
    {
        selector_macro_enc_reply(e, mask);
    }
}

// Encodes, then compares with the expected text and checks that it validates
static bool st_encode_is(const char *popup, const uint8_t *masks, const uint32_t *counts, size_t n,
                         const char *expected, selector_macro_enc_t *e, uint8_t *buf, size_t cap)
{
    if (!selector_macro_enc_begin(e, buf, cap, popup))
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        st_feed(e, masks[i], counts[i]);
    }
    size_t len = selector_macro_enc_finish(e);
    return len == strlen(expected) && memcmp(buf, expected, len) == 0 &&
           selector_macro_validate(buf, len) == SELECTOR_MACRO_OK;
}

bool selector_macro_selftest(void)
{
    uint8_t buf[96];
    selector_macro_enc_t e;
    selector_macro_iter_t it;
    uint8_t m;

    // Round trip 1: waits, a PLUS press, a trailing wait
    {
        static const uint8_t masks[] = {0, SELECTOR_KEY_PLUS, 0};
        static const uint32_t counts[] = {3, 2, 4};
        static const uint8_t want[] = {0, 0, 0, SELECTOR_KEY_PLUS, SELECTOR_KEY_PLUS, 0, 0, 0, 0};
        if (!st_encode_is("MODEL X/V1.2", masks, counts, 3, "#MODEL X/V1.2\n3 C 2 c 4", &e, buf, sizeof(buf)))
        {
            return false;
        }
        if (!selector_macro_iter_init(&it, buf, e.len))
        {
            return false;
        }
        for (size_t i = 0; i < sizeof(want); i++)
        {
            if (!selector_macro_iter_next(&it, &m) || m != want[i])
            {
                return false;
            }
        }
        if (selector_macro_iter_next(&it, &m))
        {
            return false;
        }
    }

    // Round trip 2: a chord changes in one reply (tokens in B C E D order), and a key still held
    // at the end gets its release token
    {
        static const uint8_t masks[] = {0, SELECTOR_KEY_MINUS | SELECTOR_KEY_PLUS};
        static const uint32_t counts[] = {1, 1};
        if (!st_encode_is("A", masks, counts, 2, "#A\n1 B C 1 b c", &e, buf, sizeof(buf)))
        {
            return false;
        }
        if (!selector_macro_iter_init(&it, buf, e.len) || !selector_macro_iter_next(&it, &m) || m != 0 ||
            !selector_macro_iter_next(&it, &m) || m != (SELECTOR_KEY_MINUS | SELECTOR_KEY_PLUS) ||
            selector_macro_iter_next(&it, &m))
        {
            return false;
        }
    }

    // A run longer than 65535 is split in numbers that add up
    {
        static const uint8_t masks[] = {0, SELECTOR_KEY_RIGHT};
        static const uint32_t counts[] = {70000, 1};
        if (!st_encode_is("H", masks, counts, 2, "#H\n65535 4465 D 1 d", &e, buf, sizeof(buf)))
        {
            return false;
        }
        uint32_t total = 0;
        uint8_t last = 0;
        if (!selector_macro_iter_init(&it, buf, e.len))
        {
            return false;
        }
        while (selector_macro_iter_next(&it, &m))
        {
            total++;
            last = m;
        }
        if (total != 70001 || last != SELECTOR_KEY_RIGHT)
        {
            return false;
        }
    }

    // A full buffer keeps everything up to the last complete token and still validates
    {
        uint8_t small[24];
        bool refused = false;
        if (!selector_macro_enc_begin(&e, small, sizeof(small), "H"))
        {
            return false;
        }
        for (int i = 0; i < 50; i++)
        {
            if (!selector_macro_enc_reply(&e, (i & 1) ? 0 : SELECTOR_KEY_LEFT))
            {
                refused = true;
            }
        }
        size_t len = selector_macro_enc_finish(&e);
        if (!refused || len > sizeof(small) || small[0] != '#' ||
            selector_macro_validate(small, len) != SELECTOR_MACRO_OK)
        {
            return false;
        }
    }

    // A full buffer with keys held still ends with the trailing wait and every release token
    {
        uint8_t small[40];
        int held[26] = {0};
        bool refused = false;
        if (!selector_macro_enc_begin(&e, small, sizeof(small), "H"))
        {
            return false;
        }
        for (int i = 0; i < 60; i++)
        {
            uint8_t k = (i & 1) ? (uint8_t)(SELECTOR_KEY_LEFT | SELECTOR_KEY_RIGHT) : SELECTOR_KEY_LEFT;
            if (!selector_macro_enc_reply(&e, k))
            {
                refused = true;
            }
        }
        size_t len = selector_macro_enc_finish(&e);
        if (!refused || len > sizeof(small) || selector_macro_validate(small, len) != SELECTOR_MACRO_OK)
        {
            return false;
        }
        bool body = false;
        for (size_t i = 0; i < len; i++)
        {
            char c = (char)small[i];
            if (c == '\n')
            {
                body = true;
            }
            else if (body && c >= 'A' && c <= 'Z')
            {
                held[c - 'A']++;
            }
            else if (body && c >= 'a' && c <= 'z')
            {
                held[c - 'a']--;
            }
        }
        for (int i = 0; i < 26; i++)
        {
            if (held[i] != 0)
            {
                return false;
            }
        }
    }

    // A header that does not fit
    if (selector_macro_enc_begin(&e, buf, 4, "TOO LONG"))
    {
        return false;
    }

    // Pop-up normalization
    {
        char rows[2][16];
        char out[SELECTOR_MACRO_POPUP_MAX];
        memcpy(rows[0], "  MODEL X       ", 16);
        memcpy(rows[1], " V1.2           ", 16);
        if (selector_macro_normalize_popup(rows, out, sizeof(out)) != 12 || strcmp(out, "MODEL X/V1.2") != 0)
        {
            return false;
        }
        memcpy(rows[1], "                ", 16);
        if (selector_macro_normalize_popup(rows, out, sizeof(out)) != 7 || strcmp(out, "MODEL X") != 0)
        {
            return false;
        }
        memcpy(rows[0], "                ", 16);
        if (selector_macro_normalize_popup(rows, out, sizeof(out)) != 0 || out[0] != '\0')
        {
            return false;
        }
        memcpy(rows[1], "A\x01\n            Z", 16); // control characters become '?'
        if (selector_macro_normalize_popup(rows, out, sizeof(out)) != 16 || strncmp(out, "A??", 3) != 0)
        {
            return false;
        }
    }

    // Accepted and rejected files
    {
        static const struct
        {
            const char *text;
            selector_macro_result_t want;
        } vec[] = {
            {"#H\n3 C 2 c 4", SELECTOR_MACRO_OK},
            {"#H", SELECTOR_MACRO_OK},
            {"#H\r\n3\r\nC\tb", SELECTOR_MACRO_OK},
            {"\xEF\xBB\xBF#H\n1", SELECTOR_MACRO_OK},
            {"#H\n65535", SELECTOR_MACRO_OK},
            {"", SELECTOR_MACRO_ERR_NO_HEADER},
            {"3 P", SELECTOR_MACRO_ERR_NO_HEADER},
            {"#H\n3C", SELECTOR_MACRO_OK},
            {"#H\n3C2c4D1d10E2e30", SELECTOR_MACRO_OK},
            {"#H\nCD", SELECTOR_MACRO_OK},
            {"#H\n3C\r\n2\tc", SELECTOR_MACRO_OK},
            {"#H\n1 5", SELECTOR_MACRO_OK},
            {"#H\n3 X", SELECTOR_MACRO_ERR_BAD_CHAR},
            {"#H\n3CX", SELECTOR_MACRO_ERR_BAD_CHAR},
            {"#H\n3C#", SELECTOR_MACRO_ERR_BAD_CHAR},
            {"#H\n#", SELECTOR_MACRO_ERR_BAD_CHAR},
            {"#H\n1 0", SELECTOR_MACRO_ERR_BAD_NUMBER},
            {"#H\n3C0", SELECTOR_MACRO_ERR_BAD_NUMBER},
            {"#H\n0", SELECTOR_MACRO_ERR_BAD_NUMBER},
            {"#H\n65536", SELECTOR_MACRO_ERR_BAD_NUMBER},
            {"#H\n99999999999", SELECTOR_MACRO_ERR_BAD_NUMBER},
        };
        for (size_t i = 0; i < sizeof(vec) / sizeof(vec[0]); i++)
        {
            if (selector_macro_validate((const uint8_t *)vec[i].text, strlen(vec[i].text)) != vec[i].want)
            {
                return false;
            }
        }
        // Tokens without blanks decode like the same tokens with blanks, and two numbers split by
        // a blank add up
        {
            static const char compact[] = "#H\n3C2c";
            static const uint8_t want[] = {0, 0, 0, SELECTOR_KEY_PLUS, SELECTOR_KEY_PLUS};
            selector_macro_iter_t it;
            uint8_t m;
            if (!selector_macro_iter_init(&it, (const uint8_t *)compact, strlen(compact)))
            {
                return false;
            }
            for (size_t i = 0; i < sizeof(want); i++)
            {
                if (!selector_macro_iter_next(&it, &m) || m != want[i])
                {
                    return false;
                }
            }
            if (selector_macro_iter_next(&it, &m))
            {
                return false;
            }
            static const char split[] = "#H\n1 5";
            uint32_t total = 0;
            if (!selector_macro_iter_init(&it, (const uint8_t *)split, strlen(split)))
            {
                return false;
            }
            while (selector_macro_iter_next(&it, &m))
            {
                total++;
            }
            if (total != 6)
            {
                return false;
            }
        }
        // The size limit is checked before any byte is read
        if (selector_macro_validate((const uint8_t *)"#H\n1", SELECTOR_MACRO_MAX + 1) !=
            SELECTOR_MACRO_ERR_TOO_LONG)
        {
            return false;
        }
    }
    return true;
}
