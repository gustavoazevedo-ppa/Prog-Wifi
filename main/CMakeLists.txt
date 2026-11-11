idf_component_register(
    SRCS
        "main.c"
        "ethernet_manager.c"
        "wifi_manager.c"
        "nvs_schema.c"
        "ota_button.c"
        "preset_server.c"
        "log_server.c"
        "admin_api.c"
        "http_log.c"
        "selector.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        freertos
        driver
        esp_system
        lwip
        esp_timer
        esp_eth
        esp_netif
        esp_http_client
        esp_https_ota
        app_update
        json
        esp_encrypted_img
    PRIV_REQUIRES esp_driver_spi enc28j60 esp-tls mbedtls esp_http_server
    # Pre-encrypted OTA: embed the on-device RSA-3072 private key so the decrypt
    # layer in ota_button.c can unwrap the image's session key. Generates the
    # linker symbols _binary_ota_decrypt_priv_pem_start/_end. The .pem is a
    # dev-only secret (gitignored); release engineer swaps in the real key.
    # index.html: static device-info page served from flash (see preset_server.c).
    EMBED_TXTFILES "ota_decrypt_priv.pem" "index.html"
)
