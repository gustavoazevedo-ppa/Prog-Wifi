/*
 * Key helper: edge detection. See selector_keys.h.
 */

#include "selector_keys.h"

void selector_keys_init(selector_keys_t *k)
{
    k->prev = 0;
}

uint8_t selector_keys_update(selector_keys_t *k, uint8_t pressed, uint8_t *released)
{
    uint8_t changed = (uint8_t)(k->prev ^ pressed);
    if (released != NULL)
    {
        *released = (uint8_t)(changed & k->prev);
    }
    k->prev = pressed;
    return (uint8_t)(changed & pressed);
}
