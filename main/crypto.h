#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

void decrypt_data_field(uint8_t *pacote, int length);
void encrypt_data_field(uint8_t *pacote, int length);
bool validate_crc_data_field(uint8_t *pacote, int length);
// static void test_aes(void);