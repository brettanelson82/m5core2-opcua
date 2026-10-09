/*
 * ns0_fixups.c - fill in standard (namespace 0) node values that are empty
 * in this build of open62541.
 *
 * Why: the bundled open62541 was generated with XML encoding switched off
 * (UA_ENABLE_XML_ENCODING=OFF, to save flash). The generated namespace 0
 * stores some node values as XML and only decodes them when XML encoding is
 * compiled in, so those nodes come up with an empty value of no type.
 * Other nodes are simply left empty by the reduced namespace 0. Most clients
 * never notice, but strict ones do: ABB 800xA rejects the empty, untyped
 * InputArguments of Server.ResendData (i=12874).
 *
 * What: after the server is created, write a value of the correct type into
 * every such node, in three groups:
 *   A. values lost with the XML: copied from the OPC UA standard nodeset
 *      (the same values the generated code would have decoded)
 *   B. real instances under Server that are empty in this build: typed,
 *      empty values (no software certificates, no binary dictionary)
 *   C. templates inside type definitions (InstanceDeclarations): typed,
 *      neutral defaults. They have no value in the standard nodeset either,
 *      but a typed default keeps strict clients from reading "null".
 *
 * This lives in the application, not in open62541.c, so the library stays
 * unmodified and the fix survives regenerating it. If open62541 is ever
 * regenerated with XML encoding on, group A becomes redundant but harmless.
 * The list was made by browsing the whole server and reading every
 * variable's value (all variables with an empty, untyped value).
 */
#include <stdio.h>

#include "ns0_fixups.h"

/* Write value into ns=0;i=id. Logs and returns false on failure. */
static bool writeNs0(UA_Server *server, UA_UInt32 id, const UA_Variant *value) {
    UA_StatusCode rc = UA_Server_writeValue(server, UA_NODEID_NUMERIC(0, id), *value);
    if(rc != UA_STATUSCODE_GOOD) {
        printf("[opcua] ns0 fix i=%u failed: %s\n", (unsigned)id, UA_StatusCode_name(rc));
        return false;
    }
    return true;
}

/* Scalar / array short-hands. The value is copied by the server, so the
 * caller's locals may go out of scope afterwards. */
static bool writeScalar(UA_Server *s, UA_UInt32 id, const void *p, const UA_DataType *t) {
    UA_Variant v;
    UA_Variant_setScalar(&v, (void *)p, t);
    return writeNs0(s, id, &v);
}
static bool writeArray(UA_Server *s, UA_UInt32 id, const void *p, size_t n, const UA_DataType *t) {
    UA_Variant v;
    /* An empty array still needs a non-NULL pointer to count as "typed, 0 entries" */
    UA_Variant_setArray(&v, n ? (void *)p : UA_EMPTY_ARRAY_SENTINEL, n, t);
    return writeNs0(s, id, &v);
}

/* Localized text with no locale, as in the standard nodeset */
static UA_LocalizedText lt(const char *text) { return UA_LOCALIZEDTEXT("", (char *)text); }

/* Method argument "name : dataType", scalar (valueRank -1) or 1-D array (1) */
static UA_Argument arg(const char *name, UA_UInt32 dataType, UA_Int32 valueRank) {
    UA_Argument a;
    UA_Argument_init(&a);
    a.name = UA_STRING((char *)name);
    a.dataType = UA_NODEID_NUMERIC(0, dataType);
    a.valueRank = valueRank;
    return a;
}

/* One enum value with its display name and description */
static UA_EnumValueType enumValue(UA_Int64 value, const char *name, const char *description) {
    UA_EnumValueType e;
    e.value = value;
    e.displayName = lt(name);
    e.description = lt(description);
    return e;
}

/* --- A. Values that were stored as XML in the generated namespace 0 --- */
static int fillLostXmlValues(UA_Server *server) {
    int ok = 0;

    /* Method arguments (DataType i=7 = UInt32) */
    UA_Argument subscriptionId = arg("SubscriptionId", UA_NS0ID_UINT32, UA_VALUERANK_SCALAR);
    ok += writeArray(server, 12874, &subscriptionId, 1, &UA_TYPES[UA_TYPES_ARGUMENT]); /* Server.ResendData */
    ok += writeArray(server, 11493, &subscriptionId, 1, &UA_TYPES[UA_TYPES_ARGUMENT]); /* Server.GetMonitoredItems in */
    UA_UInt32 anyLength = 0;
    UA_Argument handles[2] = {
        arg("ServerHandles", UA_NS0ID_UINT32, UA_VALUERANK_ONE_DIMENSION),
        arg("ClientHandles", UA_NS0ID_UINT32, UA_VALUERANK_ONE_DIMENSION),
    };
    for(int i = 0; i < 2; i++) { handles[i].arrayDimensionsSize = 1; handles[i].arrayDimensions = &anyLength; }
    ok += writeArray(server, 11494, handles, 2, &UA_TYPES[UA_TYPES_ARGUMENT]);        /* Server.GetMonitoredItems out */

    /* EnumStrings of RedundancySupport and AxisScaleEnumeration */
    UA_LocalizedText redundancy[6] = {
        lt("None"), lt("Cold"), lt("Warm"), lt("Hot"), lt("Transparent"), lt("HotAndMirrored"),
    };
    ok += writeArray(server, 7611, redundancy, 6, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
    UA_LocalizedText axisScale[3] = { lt("Linear"), lt("Log"), lt("Ln") };
    ok += writeArray(server, 12078, axisScale, 3, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);

    /* EnumValues of TrustListMasks and NamingRuleType */
    UA_EnumValueType trustListMasks[6] = {
        enumValue(0,  "None",                "No fields are provided."),
        enumValue(1,  "TrustedCertificates", "The TrustedCertificates are provided."),
        enumValue(2,  "TrustedCrls",         "The TrustedCrls are provided."),
        enumValue(4,  "IssuerCertificates",  "The IssuerCertificates are provided."),
        enumValue(8,  "IssuerCrls",          "The IssuerCrls are provided."),
        enumValue(15, "All",                 "All fields are provided."),
    };
    ok += writeArray(server, 12553, trustListMasks, 6, &UA_TYPES[UA_TYPES_ENUMVALUETYPE]);
    UA_EnumValueType namingRules[3] = {
        enumValue(1, "Mandatory",  "The BrowseName must appear in all instances of the type."),
        enumValue(2, "Optional",   "The BrowseName may appear in an instance of the type."),
        enumValue(3, "Constraint", "The modelling rule defines a constraint and the BrowseName "
                                   "is not used in an instance of the type."),
    };
    ok += writeArray(server, 12169, namingRules, 3, &UA_TYPES[UA_TYPES_ENUMVALUETYPE]);

    /* NamingRule of each modelling rule (1 Mandatory, 2 Optional, 3 Constraint) */
    static const struct { UA_UInt32 id; UA_NamingRuleType rule; } naming[] = {
        {111,   UA_NAMINGRULETYPE_MANDATORY}, /* ModellingRuleType (default) */
        {112,   UA_NAMINGRULETYPE_MANDATORY}, /* ModellingRule Mandatory */
        {113,   UA_NAMINGRULETYPE_OPTIONAL},  /* ModellingRule Optional */
        {114,   UA_NAMINGRULETYPE_CONSTRAINT},/* ModellingRule ExposesItsArray */
        {11509, UA_NAMINGRULETYPE_OPTIONAL},  /* ModellingRule OptionalPlaceholder */
        {11511, UA_NAMINGRULETYPE_MANDATORY}, /* ModellingRule MandatoryPlaceholder */
    };
    for(size_t i = 0; i < sizeof(naming) / sizeof(naming[0]); i++)
        ok += writeScalar(server, naming[i].id, &naming[i].rule, &UA_TYPES[UA_TYPES_NAMINGRULETYPE]);

    /* Type descriptions in the (legacy) OPC Binary dictionary Opc.Ua */
    UA_String argName = UA_STRING("Argument"), enumName = UA_STRING("EnumValueType");
    ok += writeScalar(server, 7650, &argName, &UA_TYPES[UA_TYPES_STRING]);
    ok += writeScalar(server, 7656, &enumName, &UA_TYPES[UA_TYPES_STRING]);
    return ok;
}

/* --- B. Real instances that are empty in this build --- */
static int fillEmptyInstances(UA_Server *server) {
    int ok = 0;
    /* Server.ServerCapabilities.SoftwareCertificates: we have none */
    ok += writeArray(server, 3704, NULL, 0, &UA_TYPES[UA_TYPES_SIGNEDSOFTWARECERTIFICATE]);
    /* OPC Binary dictionary "Opc.Ua" (deprecated since OPC UA 1.04): not provided */
    UA_ByteString none = { 0, (UA_Byte *)"" };
    ok += writeScalar(server, 7617, &none, &UA_TYPES[UA_TYPES_BYTESTRING]);
    return ok;
}

/* --- C. Templates (InstanceDeclarations) inside type definitions --- */
static int fillTypeTemplates(UA_Server *server) {
    int ok = 0;
    UA_String empty = UA_STRING("");
    UA_ByteString emptyBytes = { 0, (UA_Byte *)"" };
    UA_LocalizedText emptyText = lt("");

    /* DataTypeDescriptionType / DataTypeDictionaryType properties */
    ok += writeScalar(server, 104, &empty, &UA_TYPES[UA_TYPES_STRING]);         /* DataTypeVersion */
    ok += writeScalar(server, 105, &emptyBytes, &UA_TYPES[UA_TYPES_BYTESTRING]);/* DictionaryFragment */
    ok += writeScalar(server, 106, &empty, &UA_TYPES[UA_TYPES_STRING]);         /* DataTypeVersion */
    ok += writeScalar(server, 107, &empty, &UA_TYPES[UA_TYPES_STRING]);         /* NamespaceUri */

    /* DataItemType and its subtypes */
    ok += writeScalar(server, 2366, &empty, &UA_TYPES[UA_TYPES_STRING]);        /* Definition */
    UA_Range noRange = { 0.0, 0.0 };
    ok += writeScalar(server, 2369, &noRange, &UA_TYPES[UA_TYPES_RANGE]);       /* EURange */
    ok += writeScalar(server, 2370, &noRange, &UA_TYPES[UA_TYPES_RANGE]);       /* InstrumentRange */
    UA_EUInformation noUnit;
    UA_EUInformation_init(&noUnit);
    noUnit.namespaceUri = empty;
    noUnit.unitId = -1;                                                         /* -1 = no unit */
    noUnit.displayName = emptyText;
    noUnit.description = emptyText;
    ok += writeScalar(server, 2371, &noUnit, &UA_TYPES[UA_TYPES_EUINFORMATION]);/* EngineeringUnits */
    ok += writeScalar(server, 2374, &emptyText, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]); /* FalseState */
    ok += writeScalar(server, 2375, &emptyText, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]); /* TrueState */
    ok += writeArray (server, 2377, NULL, 0, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);    /* EnumStrings */
    ok += writeArray (server, 11241, NULL, 0, &UA_TYPES[UA_TYPES_ENUMVALUETYPE]);   /* EnumValues */
    ok += writeScalar(server, 11461, &emptyText, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);/* ValueAsText */
    return ok;
}

void ns0_fill_missing_values(UA_Server *server) {
    int ok = fillLostXmlValues(server) + fillEmptyInstances(server) + fillTypeTemplates(server);
    printf("[opcua] filled %d empty namespace-0 values\n", ok);
}
