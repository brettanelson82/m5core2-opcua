/*
 * opcua_security.h - optional certificate security for the OPC UA server
 * (see opcua_security.c). Used only by opcua_server.c, on the OPC UA task.
 */
#pragma once

#include <stdbool.h>
#include "open62541.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill an empty (zeroed) server config with the None endpoint plus secure
 * endpoints, the server certificate (created on first use) and a trust list
 * that rejects unknown clients. ip is the device's current IP address (goes
 * into a newly created certificate). Returns false if anything fails; the
 * config is then cleared again and the caller should fall back to None only. */
bool opcua_security_setup(UA_ServerConfig *config, UA_UInt16 port,
                          const char *applicationUri, const char *ip);

/* Call regularly from the server's repeated callback. Offers newly rejected
 * client certificates for approval on the screen and carries out the user's
 * decision. Does nothing if security is off. */
void opcua_security_poll(UA_Server *server);

#ifdef __cplusplus
}
#endif
