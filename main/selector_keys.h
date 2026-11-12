/*
 * Key helper: edge detection on the canonical key mask (SELECTOR_KEY_* bits, 1 = pressed).
 * Pure C, no ESP-IDF dependency.
 */
#ifndef SELECTOR_KEYS_H
#define SELECTOR_KEYS_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint8_t prev;
} selector_keys_t;

void selector_keys_init(selector_keys_t *k);

// Stores the new key state. Returns the keys newly pressed since the previous call.
// *released (may be NULL) gets the keys newly released.
uint8_t selector_keys_update(selector_keys_t *k, uint8_t pressed, uint8_t *released);

#endif // SELECTOR_KEYS_H
