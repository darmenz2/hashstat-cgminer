/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HASHSTAT_VERSION_H
#define HASHSTAT_VERSION_H
#define HASHSTAT_VERSION "0.0.1-dev"
#define HASHSTAT_MINER_NAME "HashMiner"
#define HASHSTAT_UPSTREAM_CORE_VERSION "4.11.1"
#define HASHSTAT_BUILD_INFO_JSON \
    "{\"schemaVersion\":1,\"component\":\"HashMiner\",\"version\":\"" HASHSTAT_VERSION "\"," \
    "\"upstream\":{\"name\":\"cgminer\",\"version\":\"" HASHSTAT_UPSTREAM_CORE_VERSION "\"," \
    "\"commit\":\"b8491c66e7e22f23a9edf095dd1337ee581e88bd\"}," \
    "\"buildKind\":\"research\",\"hardwareStartupImplemented\":false," \
    "\"fullApiCompatible\":false,\"miningReady\":false}"
#endif
