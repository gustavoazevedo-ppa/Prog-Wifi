idf_component_register(
    SRCS
        "main.c"
        "uart_handler.c"
        "ble_gatt.c"
        "ethernet_manager.c"
        "wifi_manager.c"
        "mqtt_manager.c"
        "onvif_events.c"
        "packet_manager.c"
        "packet_actions.c"
        "udp_broadcast.c"
        "nvs_schema.c"
        "monip_client.c"
        "ota_button.c"
        "telemetry.c"
        "ota_mqtt.c"
        "fw_slot_sender.c"
        "device_restart.c"
    INCLUDE_DIRS "."
    REQUIRES
        mqtt
        nvs_flash
        esp_wifi
        esp_event
        freertos
        driver
        bt
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
    PRIV_REQUIRES esp_driver_spi enc28j60
    # Pre-encrypted OTA: embed the on-device RSA-3072 private key so the decrypt
    # layer in ota_button.c can unwrap the image's session key. Generates the
    # linker symbols _binary_ota_decrypt_priv_pem_start/_end. The .pem is a
    # dev-only secret (gitignored); release engineer swaps in the real key.
    EMBED_TXTFILES "ota_decrypt_priv.pem"
)

