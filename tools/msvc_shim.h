// Build-only shim for the MSVC test build (not part of the AnkhCore source).
// The Windows SDK's rpcndr.h does `#define small char`, which breaks ak::Fonts::small.
// llvm-mingw does not trigger this. Pull the SDK headers in first, then drop the macro.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#include <winsock2.h>   // before windows.h: iphlpapi.h only declares the Vista+ interface tables with Winsock 2
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <srrestoreptapi.h>
#include <wbemidl.h>
#include <taskschd.h>
#undef small
