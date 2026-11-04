#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

void uart_handler_init(void);
void uart_handler_write_bytes(const void *data, size_t len);