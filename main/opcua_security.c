/*
 * opcua_security.c - optional certificate security for the OPC UA server.
 *
 * When security is switched on (System page on the screen, saved in flash,
 * applied at start-up) the server offers:
 *   - None                      (still offered, by choice: clients without
 *                                certificates keep working)
 *   - Basic256Sha256            Sign, Sign & Encrypt
 *   - Aes128_Sha256_RsaOaep     Sign, Sign & Encrypt
 *   - Aes256_Sha256_RsaPss      Sign, Sign & Encrypt
 * The deprecated Basic128Rsa15 and Basic256 policies are left out.
 * Login stays anonymous.
 *
 * Server certificate: created on the device the first time security is
 * switched on (self-signed RSA 2048, valid 10 years), then kept in flash
 * (security_store.c). Its names: the ApplicationUri, the hostname and the
 * IP address at the time it was created. Both the ApplicationUri and the
 * hostname contain the device ID, so every Core2 has its own certificate.
 * A stored certificate whose URI no longer matches is replaced at start-up.
 *
 * Client certificates: checked against a trust list kept in flash. An
 * unknown certificate is refused and lands on the trust store's "rejected"
 * list. opcua_security_poll() picks it up and asks the user on the screen;
 * if they tap Trust it is added to the trust list (live, and in flash) and
 * the client succeeds on its next attempt. Each certificate is asked about
 * once per boot. Clearing the trust list is done from the screen too.
 *
 * Note: the library's own "with security" setup helpers fall back to
 * accepting every client when the trust list is empty, so the trust store
 * is set up here explicitly instead.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "opcua_security.h"
#include "security_store.h"
#include "device_state.h"
#include "wifi_net.h"

#define CERT_KEY_BITS     2048  /* RSA 2048: secure, and creatable on the ESP32 in under a minute */
#define CERT_VALID_DAYS   3650  /* 10 years */
#define MAX_REJECTED      8     /* rejected client certificates kept in RAM for approval */
#define MAX_HANDLED       16    /* certificates already asked about this boot */
#define THUMB_LEN         40    /* SHA-1 thumbprint as hex */

static bool s_active;                         /* security set up successfully */
static UA_ByteString s_pendingCert;           /* certificate the user is being asked about */
static char s_handled[MAX_HANDLED][THUMB_LEN + 1]; /* thumbprints already decided */
static int  s_handledCount;

static void setStatus(uint8_t status) {
    state_lock(); g_state.security_status = status; state_unlock();
}

/* Create the server's self-signed certificate and key (DER). Takes several
 * seconds to a minute: finding RSA primes is slow on a microcontroller. */
static bool createServerCertificate(const char *applicationUri, const char *ip,
                                    UA_ByteString *cert, UA_ByteString *key) {
    char uri[96], dns[48], ipName[32];
    snprintf(uri, sizeof(uri), "URI:%s", applicationUri);
    snprintf(dns, sizeof(dns), "DNS:%s", wifi_hostname());
    snprintf(ipName, sizeof(ipName), "IP:%s", ip);
    UA_String subject[2] = { UA_STRING("CN=M5Core2 OPC UA Server"), UA_STRING("O=m5core2-opcua") };
    UA_String altNames[3] = { UA_STRING(uri), UA_STRING(dns), UA_STRING(ipName) };
    size_t altNamesSize = ip[0] ? 3 : 2;

    UA_UInt16 bits = CERT_KEY_BITS, days = CERT_VALID_DAYS;
    UA_KeyValuePair kv[2];
    kv[0].key = UA_QUALIFIEDNAME(0, "key-size-bits");
    UA_Variant_setScalar(&kv[0].value, &bits, &UA_TYPES[UA_TYPES_UINT16]);
    kv[1].key = UA_QUALIFIEDNAME(0, "expires-in-days");
    UA_Variant_setScalar(&kv[1].value, &days, &UA_TYPES[UA_TYPES_UINT16]);
    UA_KeyValueMap params;
    params.mapSize = 2;
    params.map = kv;

    int64_t t0 = esp_timer_get_time();
    UA_StatusCode rc = UA_CreateCertificate(UA_Log_Stdout, subject, 2, altNames, altNamesSize,
                                            UA_CERTIFICATEFORMAT_DER, &params, key, cert);
    printf("[opcua] server certificate %s after %.1f s\n",
           rc == UA_STATUSCODE_GOOD ? "created" : UA_StatusCode_name(rc),
           (esp_timer_get_time() - t0) / 1e6);
    return rc == UA_STATUSCODE_GOOD;
}

/* Zero a private key before freeing it */
static void clearKey(UA_ByteString *key) {
    if(key->data) memset(key->data, 0, key->length);
    UA_ByteString_clear(key);
}

bool opcua_security_setup(UA_ServerConfig *config, UA_UInt16 port,
                          const char *applicationUri, const char *ip) {
    UA_ByteString cert, key;
    bool haveCert = sec_store_load_server_cert(&cert, &key);
    /* A stored certificate made for a different ApplicationUri (e.g. from
     * firmware before the device ID was added) must be replaced, or clients
     * reject it. Only a definite URI mismatch counts: if the check fails for
     * another reason (such as low memory), keep the certificate. */
    UA_String uri = UA_STRING((char *)applicationUri);
    if(haveCert && UA_CertificateUtils_verifyApplicationUri(&cert, &uri) ==
                   UA_STATUSCODE_BADCERTIFICATEURIINVALID) {
        printf("[opcua] stored certificate is for another ApplicationUri, replacing it\n");
        UA_ByteString_clear(&cert);
        clearKey(&key);
        haveCert = false;
    }
    if(!haveCert) {
        setStatus(SEC_STATUS_CREATING_CERT);
        printf("[opcua] creating server certificate (RSA %d), this can take a minute...\n", CERT_KEY_BITS);
        if(!createServerCertificate(applicationUri, ip, &cert, &key)) {
            setStatus(SEC_STATUS_ERROR);
            return false;
        }
        if(!sec_store_save_server_cert(&cert, &key))
            printf("[opcua] could not save the server certificate; a new one is made next boot\n");
    }

    /* Base config with the None policy (carrying our certificate, so clients
     * can fetch it from GetEndpoints) */
    UA_StatusCode rc = UA_ServerConfig_setMinimal(config, port, &cert);

    /* Trust list: stored client certificates. Unknown ones are rejected and
     * kept (up to MAX_REJECTED) for approval. */
    UA_ByteString *trusted = NULL;
    size_t trustedSize = sec_store_load_trusted(&trusted);
    UA_TrustListDataType trustList;
    UA_TrustListDataType_init(&trustList);
    trustList.specifiedLists = UA_TRUSTLISTMASKS_TRUSTEDCERTIFICATES;
    trustList.trustedCertificates = trusted;
    trustList.trustedCertificatesSize = trustedSize;
    UA_UInt32 maxRejected = MAX_REJECTED;
    UA_KeyValuePair kv;
    kv.key = UA_QUALIFIEDNAME(0, "max-rejected-listsize");
    UA_Variant_setScalar(&kv.value, &maxRejected, &UA_TYPES[UA_TYPES_UINT32]);
    UA_KeyValueMap params;
    params.mapSize = 1;
    params.map = &kv;
    UA_NodeId group = UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTAPPLICATIONGROUP);
    if(rc == UA_STATUSCODE_GOOD)
        rc = UA_CertificateGroup_Memorystore(&config->secureChannelPKI, &group, &trustList,
                                             config->logging, &params);
    UA_TrustListDataType_clear(&trustList); /* the store keeps its own copy */

    /* Secure policies (the deprecated Basic128Rsa15/Basic256 are left out) */
    if(rc == UA_STATUSCODE_GOOD) rc = UA_ServerConfig_addSecurityPolicyBasic256Sha256(config, &cert, &key);
    if(rc == UA_STATUSCODE_GOOD) rc = UA_ServerConfig_addSecurityPolicyAes128Sha256RsaOaep(config, &cert, &key);
    if(rc == UA_STATUSCODE_GOOD) rc = UA_ServerConfig_addSecurityPolicyAes256Sha256RsaPss(config, &cert, &key);
    UA_ByteString_clear(&cert);
    clearKey(&key);

    /* Endpoints: replace the None-only list from setMinimal with None plus
     * Sign and Sign & Encrypt for every secure policy */
    if(rc == UA_STATUSCODE_GOOD) {
        UA_Array_delete(config->endpoints, config->endpointsSize,
                        &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
        config->endpoints = NULL;
        config->endpointsSize = 0;
        rc = UA_ServerConfig_addAllEndpoints(config);
    }
    /* Access control again, so anonymous login covers the new endpoints */
    if(rc == UA_STATUSCODE_GOOD)
        rc = UA_AccessControl_default(config, true, NULL, 0, NULL);

    if(rc != UA_STATUSCODE_GOOD) {
        printf("[opcua] security setup failed: %s\n", UA_StatusCode_name(rc));
        UA_ServerConfig_clear(config);
        memset(config, 0, sizeof(*config));
        setStatus(SEC_STATUS_ERROR);
        return false;
    }

    state_lock();
    g_state.security_status = SEC_STATUS_READY;
    g_state.trusted_count = (uint32_t)trustedSize;
    state_unlock();
    s_active = true;
    printf("[opcua] security on: %u trusted client(s), %u kB internal RAM free\n",
           (unsigned)trustedSize, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

/* Hex SHA-1 thumbprint of a certificate into out (THUMB_LEN + 1 bytes) */
static bool thumbprint(UA_ByteString *cert, char *out) {
    UA_String tp = { THUMB_LEN, (UA_Byte *)out };
    out[THUMB_LEN] = '\0';
    return UA_CertificateUtils_getThumbprint(cert, &tp) == UA_STATUSCODE_GOOD;
}

static bool alreadyHandled(const char *tp) {
    for(int i = 0; i < s_handledCount; i++)
        if(strcmp(s_handled[i], tp) == 0) return true;
    return false;
}

static void markHandled(const char *tp) {
    if(s_handledCount < MAX_HANDLED)
        strcpy(s_handled[s_handledCount++], tp);
}

/* Short name for the screen: the CN= part of the subject, else the subject */
static void displayName(UA_ByteString *cert, char *out, size_t outSize) {
    UA_String subject = UA_STRING_NULL;
    snprintf(out, outSize, "(unnamed certificate)");
    if(UA_CertificateUtils_getSubjectName(cert, &subject) != UA_STATUSCODE_GOOD)
        return;
    char buf[160];
    size_t n = subject.length < sizeof(buf) - 1 ? subject.length : sizeof(buf) - 1;
    memcpy(buf, subject.data, n);
    buf[n] = '\0';
    UA_String_clear(&subject);
    const char *cn = strstr(buf, "CN=");
    const char *start = cn ? cn + 3 : buf;
    size_t len = strcspn(start, ",");
    if(len >= outSize) len = outSize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

void opcua_security_poll(UA_Server *server) {
    if(!s_active)
        return;

    state_lock();
    bool pending = g_state.approval_pending;
    uint8_t decision = g_state.approval_decision;
    state_unlock();

    /* 1. The user answered: carry it out */
    if(pending && decision != APPROVAL_NONE) {
        char tp[THUMB_LEN + 1] = "";
        thumbprint(&s_pendingCert, tp);
        markHandled(tp);
        if(decision == APPROVAL_TRUST) {
            UA_NodeId group = UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTAPPLICATIONGROUP);
            UA_StatusCode rc = UA_Server_addCertificates(server, group, &s_pendingCert, 1,
                                                         NULL, 0, true, true);
            bool saved = rc == UA_STATUSCODE_GOOD && sec_store_add_trusted(&s_pendingCert);
            printf("[opcua] client certificate %s: %s%s\n", tp,
                   rc == UA_STATUSCODE_GOOD ? "trusted" : UA_StatusCode_name(rc),
                   rc == UA_STATUSCODE_GOOD && !saved ? " (not saved: trust list full?)" : "");
        } else {
            printf("[opcua] client certificate %s: rejected\n", tp);
        }
        UA_ByteString_clear(&s_pendingCert);
        state_lock();
        g_state.approval_pending = false;
        g_state.approval_decision = APPROVAL_NONE;
        g_state.trusted_count = (uint32_t)sec_store_trusted_count();
        state_unlock();
        return;
    }
    if(pending)
        return; /* still waiting for the user */

    /* 2. Any newly rejected client certificate to ask about? (newest first) */
    UA_CertificateGroup *pki = &UA_Server_getConfig(server)->secureChannelPKI;
    UA_ByteString *rejected = NULL;
    size_t rejectedSize = 0;
    if(!pki->getRejectedList || pki->getRejectedList(pki, &rejected, &rejectedSize) != UA_STATUSCODE_GOOD)
        return;
    for(size_t i = rejectedSize; i-- > 0; ) {
        char tp[THUMB_LEN + 1];
        if(!thumbprint(&rejected[i], tp) || alreadyHandled(tp))
            continue;
        if(UA_ByteString_copy(&rejected[i], &s_pendingCert) != UA_STATUSCODE_GOOD)
            break;
        char name[sizeof(g_state.pending_client)];
        displayName(&rejected[i], name, sizeof(name));
        state_lock();
        strcpy(g_state.pending_client, name);
        snprintf(g_state.pending_thumb, sizeof(g_state.pending_thumb), "%.8s", tp);
        g_state.approval_decision = APPROVAL_NONE;
        g_state.approval_pending = true;
        g_state.wake_request = true; /* wake the screen to ask */
        state_unlock();
        printf("[opcua] untrusted client certificate \"%s\" (%s): asking on screen\n", name, tp);
        break;
    }
    UA_Array_delete(rejected, rejectedSize, &UA_TYPES[UA_TYPES_BYTESTRING]);
}
