// LiDE50 Scan - one-time binding of Windows' built-in WinUSB driver to the
// Canon LiDE 50 (USB 04A9:2213), so the app can talk to the scanner directly.
// No third-party driver files are installed: winusb.inf ships with Windows
// and is signed by Microsoft.
#pragma once
#include <string>

enum class UsbAccessState {
    NotConnected,   // no LiDE 50 plugged in
    NeedsSetup,     // plugged in, but no WinUSB binding yet
    Ready,          // WinUSB bound, app can open it
};

struct UsbAccessInfo {
    UsbAccessState state = UsbAccessState::NotConnected;
    std::wstring instance_id;
    std::wstring service;      // current function driver service name
    unsigned problem = 0;      // CM_PROB_* code (0 = none)
};

UsbAccessInfo usb_detect_lide50();

// Must run elevated. Returns true on success; `message` describes the result.
bool usb_bind_winusb(std::wstring& message);

// Must run elevated. Removes the device node so Windows forgets the binding.
bool usb_unbind_winusb(std::wstring& message);

// Text shown if automatic setup is not possible.
std::wstring usb_manual_setup_instructions();
