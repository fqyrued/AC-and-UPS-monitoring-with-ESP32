// Copy this file to config.h and fill in the real values.
// config.h is listed in .gitignore and is never committed.

#pragma once

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

#define WIFI_SSID_VALUE "your-ssid"
#define WIFI_PASS_VALUE "your-password"

// ---------------------------------------------------------------------------
// SNMP community used for the UPS units (read only)
// ---------------------------------------------------------------------------

#define SNMP_COMMUNITY_VALUE "public"

// ---------------------------------------------------------------------------
// Addresses
//
// AC units use their own web interface address.
// UPS units use the address of their network management card.
// ---------------------------------------------------------------------------

#define AC2_HOST  "192.0.2.12"
#define AC3_HOST  "192.0.2.13"
#define AC4_HOST  "192.0.2.14"
#define AC5_HOST  "192.0.2.15"
#define AC6_HOST  "192.0.2.16"

#define AC1_HOST  "192.0.2.11"
#define AC7_HOST  "192.0.2.17"
#define AC8_HOST  "192.0.2.18"
#define AC9_HOST  "192.0.2.19"
#define AC10_HOST "192.0.2.20"

#define UPS1_HOST "192.0.2.31"
#define UPS2_HOST "192.0.2.32"
#define UPS3_HOST "192.0.2.33"
#define UPS4_HOST "192.0.2.34"
#define UPS5_HOST "192.0.2.35"
