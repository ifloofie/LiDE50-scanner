// LiDE50 Scan - loading Windows' own DLLs from the system folder only.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#include "sysdll.h"

#include <string>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

void sysdll_restrict_search()
{
    // remove the current folder from the search path, then allow System32 only
    SetDllDirectoryW(L"");
    using Fn = BOOL(WINAPI*)(DWORD);
    if (auto fn = reinterpret_cast<Fn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories"))))
        fn(LOAD_LIBRARY_SEARCH_SYSTEM32);
}

HMODULE sysdll_load(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    std::wstring full = std::wstring(dir) + L"\\" + name;
    // a full path, so nothing is searched for the DLL itself; the flag makes the DLL's own
    // dependencies come from System32 as well
    return LoadLibraryExW(full.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

namespace {
template <class Fn>
Fn sys_proc(const wchar_t* dll, const char* name)
{
    HMODULE m = sysdll_load(dll);
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}
} // namespace

HRESULT sys_D2D1CreateFactory(int factory_type, REFIID riid, const void* options, void** factory)
{
    using Fn = HRESULT(WINAPI*)(int, REFIID, const void*, void**);
    static Fn fn = sys_proc<Fn>(L"d2d1.dll", "D2D1CreateFactory");
    return fn ? fn(factory_type, riid, options, factory) : E_NOTIMPL;
}

HRESULT sys_DWriteCreateFactory(int factory_type, REFIID riid, IUnknown** factory)
{
    using Fn = HRESULT(WINAPI*)(int, REFIID, IUnknown**);
    static Fn fn = sys_proc<Fn>(L"dwrite.dll", "DWriteCreateFactory");
    return fn ? fn(factory_type, riid, factory) : E_NOTIMPL;
}

HRESULT sys_DwmSetWindowAttribute(HWND hwnd, DWORD attribute, const void* value, DWORD size)
{
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static Fn fn = sys_proc<Fn>(L"dwmapi.dll", "DwmSetWindowAttribute");
    return fn ? fn(hwnd, attribute, value, size) : E_NOTIMPL;
}
