/*
 * opcua_server.h - the OPC UA server (see opcua_server.c for the address
 * space). It exchanges data with the rest of the firmware only through
 * g_state (device_state.h).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the address space and runs the OPC UA server forever on port 4840.
 * Call it from its own task once Wi-Fi is up (it needs a stack of ~24 kB).
 * Only returns if the server could not be created or started. */
void opcua_server_run(void);

#ifdef __cplusplus
}
#endif
