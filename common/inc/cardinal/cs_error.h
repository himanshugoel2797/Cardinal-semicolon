// Copyright (c) 2026 Himanshu Goel
//
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_CS_ERROR_H
#define CARDINAL_CS_ERROR_H

// Unified error type and codes for all Sys* module APIs. Lives in common/inc so
// it is reachable everywhere (common/inc is a SYSTEM include for every target).
// CS_OK is 0; every failure is negative, so `if (err < 0)` reliably detects any
// error (the old per-module enums used positive codes, which broke that check).

typedef int cs_error;

#define CS_OK 0
#define CS_UNKN (-1)         // unspecified failure
#define CS_OUTOFMEM (-2)     // allocation failed
#define CS_INVALIDARG (-3)   // bad argument
#define CS_DNE (-4)          // does not exist / not found
#define CS_EXISTS (-5)       // already exists
#define CS_FAILURE (-6)      // operation failed
#define CS_TYPEMISMATCH (-7) // value present but wrong type
#define CS_ALREADYMAPPED (-8)
#define CS_NOMAPPING (-9)
#define CS_CONTINUE (-10)    // internal: keep walking (not a terminal error)

#endif
