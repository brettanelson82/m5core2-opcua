/*
 * ns0_fixups.h - fill in standard (namespace 0) node values that are empty
 * in this build of open62541. See ns0_fixups.c for why and which nodes.
 */
#pragma once

#include "open62541.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write the missing values into namespace 0. Call once, right after
 * UA_Server_newWithConfig() and before clients can connect. Failures are
 * logged and skipped (the server still works, those nodes just stay empty). */
void ns0_fill_missing_values(UA_Server *server);

#ifdef __cplusplus
}
#endif
