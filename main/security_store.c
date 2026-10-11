/*
 * security_store.c - flash storage for the security settings and
 * certificates (see security_store.h).
 *
 * Layout in NVS partition "certs", namespace "opcua_sec":
 *   enabled          u8    security on (1) / off (0)
 *   srv_cert         blob  server certificate (DER)
 *   srv_key          blob  server private key (DER)
 *   trust_n          u32   number of trusted client certificates
 *   trust_0..trust_N blob  trusted client certificates (DER)
 *
 * The private key is stored unencrypted: anyone with the device and a USB
 * cable can read it out. Flash/NVS encryption would be needed to prevent
 * that.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "security_store.h"

static const char *TAG = "sec_store";
#define PARTITION "certs"
#define NAMESPACE "opcua_sec"

static bool open_ns(nvs_open_mode_t mode, nvs_handle_t *h) {
    esp_err_t err = nvs_open_from_partition(PARTITION, NAMESPACE, mode, h);
    if(err != ESP_OK && !(mode == NVS_READONLY && err == ESP_ERR_NVS_NOT_FOUND))
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

/* Read a blob into a newly allocated UA_ByteString. False if missing. */
static bool get_blob(nvs_handle_t h, const char *key, UA_ByteString *out) {
    size_t len = 0;
    if(nvs_get_blob(h, key, NULL, &len) != ESP_OK || len == 0)
        return false;
    if(UA_ByteString_allocBuffer(out, len) != UA_STATUSCODE_GOOD)
        return false;
    if(nvs_get_blob(h, key, out->data, &len) != ESP_OK) {
        UA_ByteString_clear(out);
        return false;
    }
    return true;
}

bool sec_store_init(void) {
    esp_err_t err = nvs_flash_init_partition(PARTITION);
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "partition unreadable, erasing it");
        nvs_flash_erase_partition(PARTITION);
        err = nvs_flash_init_partition(PARTITION);
    }
    if(err != ESP_OK)
        ESP_LOGE(TAG, "init failed: %s (security stays off)", esp_err_to_name(err));
    return err == ESP_OK;
}

bool sec_store_get_enabled(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if(open_ns(NVS_READONLY, &h)) {
        nvs_get_u8(h, "enabled", &v);
        nvs_close(h);
    }
    return v != 0;
}

bool sec_store_set_enabled(bool enabled) {
    nvs_handle_t h;
    if(!open_ns(NVS_READWRITE, &h)) return false;
    bool ok = nvs_set_u8(h, "enabled", enabled ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool sec_store_load_server_cert(UA_ByteString *cert, UA_ByteString *key) {
    nvs_handle_t h;
    UA_ByteString_init(cert);
    UA_ByteString_init(key);
    if(!open_ns(NVS_READONLY, &h)) return false;
    bool ok = get_blob(h, "srv_cert", cert) && get_blob(h, "srv_key", key);
    nvs_close(h);
    if(!ok) { UA_ByteString_clear(cert); UA_ByteString_clear(key); }
    return ok;
}

bool sec_store_save_server_cert(const UA_ByteString *cert, const UA_ByteString *key) {
    nvs_handle_t h;
    if(!open_ns(NVS_READWRITE, &h)) return false;
    bool ok = nvs_set_blob(h, "srv_cert", cert->data, cert->length) == ESP_OK &&
              nvs_set_blob(h, "srv_key", key->data, key->length) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

size_t sec_store_trusted_count(void) {
    nvs_handle_t h;
    uint32_t n = 0;
    if(open_ns(NVS_READONLY, &h)) {
        nvs_get_u32(h, "trust_n", &n);
        nvs_close(h);
    }
    return n;
}

size_t sec_store_load_trusted(UA_ByteString **list) {
    *list = NULL;
    nvs_handle_t h;
    if(!open_ns(NVS_READONLY, &h)) return 0;
    uint32_t n = 0;
    nvs_get_u32(h, "trust_n", &n);
    size_t loaded = 0;
    if(n > 0) {
        *list = (UA_ByteString *)UA_Array_new(n, &UA_TYPES[UA_TYPES_BYTESTRING]);
        for(uint32_t i = 0; *list && i < n; i++) {
            char key[20];  /* "trust_" + up to 10 digits */
            snprintf(key, sizeof(key), "trust_%u", (unsigned)i);
            if(get_blob(h, key, &(*list)[loaded]))
                loaded++;
        }
    }
    nvs_close(h);
    if(loaded == 0 && *list) {
        UA_Array_delete(*list, n, &UA_TYPES[UA_TYPES_BYTESTRING]);
        *list = NULL;
    }
    /* Entries that failed to load leave empty slots at the end of the array.
     * They own no memory, so freeing it with size `loaded` is correct. */
    return loaded;
}

bool sec_store_add_trusted(const UA_ByteString *cert) {
    nvs_handle_t h;
    if(!open_ns(NVS_READWRITE, &h)) return false;
    uint32_t n = 0;
    nvs_get_u32(h, "trust_n", &n);
    bool ok = false;
    if(n < SEC_MAX_TRUSTED) {
        char key[20];  /* "trust_" + up to 10 digits */
        snprintf(key, sizeof(key), "trust_%u", (unsigned)n);
        ok = nvs_set_blob(h, key, cert->data, cert->length) == ESP_OK &&
             nvs_set_u32(h, "trust_n", n + 1) == ESP_OK &&
             nvs_commit(h) == ESP_OK;
    } else {
        ESP_LOGW(TAG, "trust list full (%d)", SEC_MAX_TRUSTED);
    }
    nvs_close(h);
    return ok;
}

bool sec_store_clear_trusted(void) {
    nvs_handle_t h;
    if(!open_ns(NVS_READWRITE, &h)) return false;
    uint32_t n = 0;
    nvs_get_u32(h, "trust_n", &n);
    for(uint32_t i = 0; i < n; i++) {
        char key[20];  /* "trust_" + up to 10 digits */
        snprintf(key, sizeof(key), "trust_%u", (unsigned)i);
        nvs_erase_key(h, key);
    }
    bool ok = nvs_set_u32(h, "trust_n", 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
