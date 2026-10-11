/*
 * security_store.h - everything security keeps in flash (the "certs" NVS
 * partition, see partitions.csv): the on/off setting, the server's own
 * certificate and private key, and the trusted client certificates.
 *
 * Certificates and keys are DER-encoded. Safe to call from any task
 * (NVS does its own locking).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "open62541.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEC_MAX_TRUSTED 16   /* most client certificates we keep */

/* Open the partition. Call once at start-up, before anything else here. */
bool sec_store_init(void);

/* Security on/off setting (default off). Takes effect at the next restart. */
bool sec_store_get_enabled(void);
bool sec_store_set_enabled(bool enabled);

/* The server's own certificate and private key. load returns false if none
 * is stored yet; on success the caller owns both (UA_ByteString_clear). */
bool sec_store_load_server_cert(UA_ByteString *cert, UA_ByteString *key);
bool sec_store_save_server_cert(const UA_ByteString *cert, const UA_ByteString *key);

/* Trusted client certificates. load returns a new array (free it with
 * UA_Array_delete(list, n, &UA_TYPES[UA_TYPES_BYTESTRING])) and its size. */
size_t sec_store_load_trusted(UA_ByteString **list);
size_t sec_store_trusted_count(void);
bool   sec_store_add_trusted(const UA_ByteString *cert);  /* false if full or on error */
bool   sec_store_clear_trusted(void);

#ifdef __cplusplus
}
#endif
