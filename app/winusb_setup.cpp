// LiDE50 Scan - WinUSB binding for the Canon LiDE 50.
#include "winusb_setup.h"
#include "log.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <newdev.h>
#include <initguid.h>

#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace {

// {88BAE032-5A81-49f0-BC3D-A4FF138216D6} - "Universal Serial Bus devices" class used by winusb.inf
const GUID kUsbDeviceClass = {0x88bae032, 0x5a81, 0x49f0, {0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6}};
const wchar_t kHwid[] = L"VID_04A9&PID_2213";

std::wstring upper(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
    return s;
}

std::wstring last_error_text(DWORD err)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    wchar_t code[32];
    swprintf(code, 32, L" (0x%08lX)", static_cast<unsigned long>(err));
    return s + code;
}

bool get_string_prop(HDEVINFO devs, SP_DEVINFO_DATA* did, DWORD prop, std::wstring& out)
{
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(devs, did, prop, &type, nullptr, 0, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size + 4, 0);
    if (!SetupDiGetDeviceRegistryPropertyW(devs, did, prop, &type, buf.data(), size, &size)) return false;
    if (type == REG_MULTI_SZ) {
        // flatten for searching
        out.clear();
        const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
        while (*p) { out += p; out += L'|'; p += wcslen(p) + 1; }
    } else {
        out = reinterpret_cast<const wchar_t*>(buf.data());
    }
    return true;
}

// Finds the LiDE 50 device node. On success `devs` must be destroyed by caller.
bool find_device(HDEVINFO& devs, SP_DEVINFO_DATA& did)
{
    devs = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return false;
    for (DWORD i = 0;; ++i) {
        did = SP_DEVINFO_DATA{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInfo(devs, i, &did)) break;
        std::wstring hw;
        if (!get_string_prop(devs, &did, SPDRP_HARDWAREID, hw)) continue;
        if (upper(hw).find(kHwid) != std::wstring::npos) return true;
    }
    SetupDiDestroyDeviceInfoList(devs);
    devs = INVALID_HANDLE_VALUE;
    return false;
}

using DiInstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);

bool install_from_list(HDEVINFO devs, SP_DEVINFO_DATA& did, DWORD list_type, std::wstring& why)
{
    SP_DEVINSTALL_PARAMS_W ip{};
    ip.cbSize = sizeof(ip);
    if (!SetupDiGetDeviceInstallParamsW(devs, &did, &ip)) {
        why = L"SetupDiGetDeviceInstallParams: " + last_error_text(GetLastError());
        return false;
    }
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::wstring inf = std::wstring(windir) + L"\\INF\\winusb.inf";
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        why = L"Windows' built-in WinUSB driver (winusb.inf) was not found.";
        return false;
    }
    wcsncpy(ip.DriverPath, inf.c_str(), MAX_PATH - 1);
    ip.Flags |= DI_ENUMSINGLEINF;
    ip.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    if (!SetupDiSetDeviceInstallParamsW(devs, &did, &ip)) {
        why = L"SetupDiSetDeviceInstallParams: " + last_error_text(GetLastError());
        return false;
    }
    if (!SetupDiBuildDriverInfoList(devs, &did, list_type)) {
        why = L"SetupDiBuildDriverInfoList: " + last_error_text(GetLastError());
        return false;
    }
    SP_DRVINFO_DATA_W drv{};
    drv.cbSize = sizeof(drv);
    bool found = false;
    for (DWORD i = 0; SetupDiEnumDriverInfoW(devs, &did, list_type, i, &drv); ++i) {
        found = true; // winusb.inf contains a single model: "WinUsb Device"
        break;
    }
    if (!found) {
        SetupDiDestroyDriverInfoList(devs, &did, list_type);
        why = L"winusb.inf offered no driver for this device.";
        return false;
    }
    if (!SetupDiSetSelectedDriverW(devs, &did, &drv)) {
        why = L"SetupDiSetSelectedDriver: " + last_error_text(GetLastError());
        SetupDiDestroyDriverInfoList(devs, &did, list_type);
        return false;
    }

    HMODULE newdev = LoadLibraryW(L"newdev.dll");
    auto install = newdev ? reinterpret_cast<DiInstallDeviceFn>(GetProcAddress(newdev, "DiInstallDevice")) : nullptr;
    if (!install) {
        why = L"DiInstallDevice is not available on this version of Windows.";
        SetupDiDestroyDriverInfoList(devs, &did, list_type);
        return false;
    }
    BOOL reboot = FALSE;
    BOOL ok = install(nullptr, devs, &did, &drv, 0, &reboot);
    DWORD err = GetLastError();
    SetupDiDestroyDriverInfoList(devs, &did, list_type);
    if (!ok) {
        why = L"DiInstallDevice: " + last_error_text(err);
        return false;
    }
    if (reboot) why = L"reboot";
    return true;
}

// Last resort: an INF that only references Windows' own WinUSB driver for this
// one hardware ID. It has no catalog, so Windows shows its "can't verify the
// publisher" prompt; no third-party driver binaries are involved.
bool install_with_inf(std::wstring& why)
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"lide50scan_inf";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring inf = dir + L"\\lide50_winusb.inf";
    const char* text =
        "[Version]\r\n"
        "Signature = \"$Windows NT$\"\r\n"
        "Class = USBDevice\r\n"
        "ClassGuid = {88BAE032-5A81-49F0-BC3D-A4FF138216D6}\r\n"
        "Provider = %Provider%\r\n"
        "DriverVer = 10/12/2026,1.0.0.0\r\n"
        "\r\n"
        "[Manufacturer]\r\n"
        "%Mfg% = Devices,NTamd64,NTarm64\r\n"
        "\r\n"
        "[Devices.NTamd64]\r\n"
        "%DeviceName% = USB_Install, USB\\VID_04A9&PID_2213\r\n"
        "\r\n"
        "[Devices.NTarm64]\r\n"
        "%DeviceName% = USB_Install, USB\\VID_04A9&PID_2213\r\n"
        "\r\n"
        "[USB_Install]\r\n"
        "Include = winusb.inf\r\n"
        "Needs = WINUSB.NT\r\n"
        "\r\n"
        "[USB_Install.Services]\r\n"
        "Include = winusb.inf\r\n"
        "Needs = WINUSB.NT.Services\r\n"
        "\r\n"
        "[USB_Install.HW]\r\n"
        "AddReg = Dev_AddReg\r\n"
        "\r\n"
        "[Dev_AddReg]\r\n"
        "HKR,,DeviceInterfaceGUIDs,0x10000,\"{6B1F8A51-5C4E-4D7A-9C21-0A5E1D50A050}\"\r\n"
        "\r\n"
        "[Strings]\r\n"
        "Provider = \"LiDE50 Scan\"\r\n"
        "Mfg = \"Canon\"\r\n"
        "DeviceName = \"CanoScan LiDE 50 (WinUSB)\"\r\n";
    HANDLE f = CreateFileW(inf.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        why = L"Could not write the setup file: " + last_error_text(GetLastError());
        return false;
    }
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(f);

    using UpdateFn = BOOL(WINAPI*)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);
    HMODULE newdev = LoadLibraryW(L"newdev.dll");
    auto update = newdev ? reinterpret_cast<UpdateFn>(GetProcAddress(newdev, "UpdateDriverForPlugAndPlayDevicesW")) : nullptr;
    if (!update) {
        why = L"UpdateDriverForPlugAndPlayDevices is not available.";
        return false;
    }
    BOOL reboot = FALSE;
    if (!update(GetForegroundWindow(), L"USB\\VID_04A9&PID_2213", inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        why = L"UpdateDriverForPlugAndPlayDevices: " + last_error_text(GetLastError());
        return false;
    }
    if (reboot) why = L"reboot";
    return true;
}

} // namespace

UsbAccessInfo usb_detect_lide50()
{
    UsbAccessInfo info;
    HDEVINFO devs;
    SP_DEVINFO_DATA did;
    if (!find_device(devs, did)) return info;

    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(did.DevInst, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) info.instance_id = id;
    get_string_prop(devs, &did, SPDRP_SERVICE, info.service);
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0) == CR_SUCCESS && (status & DN_HAS_PROBLEM))
        info.problem = problem;
    SetupDiDestroyDeviceInfoList(devs);

    info.state = (_wcsicmp(info.service.c_str(), L"WinUSB") == 0 && info.problem == 0)
                     ? UsbAccessState::Ready : UsbAccessState::NeedsSetup;
    return info;
}

bool usb_bind_winusb(std::wstring& message)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA did;
    if (!find_device(devs, did)) {
        message = L"The Canon LiDE 50 is not plugged in. Connect it with the USB cable and try again.";
        return false;
    }

    std::wstring why1, why2, why3;
    LOG_INFO("access setup: attempt 1 (Windows WinUSB driver, compatible list)");
    bool ok = install_from_list(devs, did, SPDIT_COMPATDRIVER, why1);
    logw(ok ? "INFO" : "WARN", L"access setup: attempt 1 " + std::wstring(ok ? L"succeeded " : L"failed: ") + why1);
    if (!ok) {
        LOG_INFO("access setup: attempt 2 (Windows WinUSB driver, class list)");
        ok = install_from_list(devs, did, SPDIT_CLASSDRIVER, why2);
        logw(ok ? "INFO" : "WARN", L"access setup: attempt 2 " + std::wstring(ok ? L"succeeded " : L"failed: ") + why2);
    }
    if (!ok) {
        // Move the device into the "Universal Serial Bus devices" class and retry.
        const wchar_t* cls = L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}";
        SetupDiSetDeviceRegistryPropertyW(devs, &did, SPDRP_CLASSGUID,
                                          reinterpret_cast<const BYTE*>(cls),
                                          static_cast<DWORD>((wcslen(cls) + 1) * sizeof(wchar_t)));
        SetupDiDestroyDeviceInfoList(devs);
        if (find_device(devs, did)) {
            did.ClassGuid = kUsbDeviceClass;
            LOG_INFO("access setup: attempt 3 (moved device to the USB device class)");
            ok = install_from_list(devs, did, SPDIT_CLASSDRIVER, why3);
            logw(ok ? "INFO" : "WARN", L"access setup: attempt 3 " + std::wstring(ok ? L"succeeded " : L"failed: ") + why3);
        }
    }
    if (devs != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(devs);

    std::wstring why4;
    if (!ok) {
        LOG_INFO("access setup: attempt 4 (setup file that points to Windows' WinUSB driver)");
        ok = install_with_inf(why4);
        logw(ok ? "INFO" : "WARN", L"access setup: attempt 4 " + std::wstring(ok ? L"succeeded " : L"failed: ") + why4);
    }

    if (!ok) {
        message = L"Automatic setup did not succeed.\n\nDetails:\n  " + why1 + L"\n  " + why2 +
                  (why3.empty() ? L"" : L"\n  " + why3) + L"\n  " + why4 + L"\n\n" +
                  usb_manual_setup_instructions();
        return false;
    }

    // give PnP a moment to start the device on WinUSB
    for (int i = 0; i < 20; ++i) {
        if (usb_detect_lide50().state == UsbAccessState::Ready) break;
        Sleep(250);
    }
    message = L"Scanner access is set up. Windows' built-in WinUSB driver is now assigned to the Canon LiDE 50.";
    if (why1 == L"reboot" || why2 == L"reboot" || why3 == L"reboot" || why4 == L"reboot")
        message += L"\nWindows asked for a restart before the change takes full effect.";
    return true;
}

bool usb_unbind_winusb(std::wstring& message)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA did;
    if (!find_device(devs, did)) {
        message = L"Plug in the Canon LiDE 50 first.";
        return false;
    }
    SP_REMOVEDEVICE_PARAMS rp{};
    rp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    rp.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    rp.Scope = DI_REMOVEDEVICE_GLOBAL;
    bool ok = SetupDiSetClassInstallParamsW(devs, &did, &rp.ClassInstallHeader, sizeof(rp)) &&
              SetupDiCallClassInstaller(DIF_REMOVE, devs, &did);
    DWORD err = GetLastError();
    SetupDiDestroyDeviceInfoList(devs);
    message = ok ? L"The scanner's WinUSB assignment was removed. Unplug and replug the scanner."
                 : L"Could not remove the device: " + last_error_text(err);
    return ok;
}

std::wstring usb_manual_setup_instructions()
{
    return L"Manual setup (one time, uses only drivers built into Windows):\n"
           L"1. Right-click Start and open Device Manager.\n"
           L"2. Under \"Other devices\" right-click \"CanoScan\" (or the unknown device) and choose Update driver.\n"
           L"3. Choose \"Browse my computer for drivers\", then \"Let me pick from a list of available drivers\".\n"
           L"4. Select \"Universal Serial Bus devices\", then manufacturer \"WinUsb Device\" and model \"WinUsb Device\".\n"
           L"5. Click Next and confirm. Then return to LiDE50 Scan.";
}
