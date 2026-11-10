// LiDE50 Scan - loading Windows' own DLLs from the system folder only.
//
// Windows looks for most DLLs in the program's folder first. For a portable program
// that runs from Downloads or a USB stick, a file with the right name placed there
// would be loaded and run (and with administrator rights during scanner setup).
// The DLLs that are not on Windows' protected "known DLLs" list are therefore loaded
// here at run time, by full path from System32, instead of being linked.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#pragma once
#include <windows.h>

// Call first thing in wWinMain: restricts every later LoadLibrary to System32.
void sysdll_restrict_search();

// Loads a DLL from System32 only (nullptr if it isn't there).
HMODULE sysdll_load(const wchar_t* name);

// d2d1.dll / dwrite.dll / dwmapi.dll entry points; each returns E_NOTIMPL if unavailable.
HRESULT sys_D2D1CreateFactory(int factory_type, REFIID riid, const void* options, void** factory);
HRESULT sys_DWriteCreateFactory(int factory_type, REFIID riid, IUnknown** factory);
HRESULT sys_DwmSetWindowAttribute(HWND hwnd, DWORD attribute, const void* value, DWORD size);
