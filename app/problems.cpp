// LiDE50 Scan - every problem the app can run into, in plain language.
#include "problems.h"

#include "../core/include/lide50_diag.h"
#include "../core/include/sane/sane.h"

#include <windows.h>

#include <cstdio>

namespace {

std::wstring widen_ascii(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

std::wstring fmt(const wchar_t* f, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(buf, 1023, f, ap);
    va_end(ap);
    buf[1023] = 0;
    return buf;
}

std::wstring driver_detail(int status, const std::string& driver_message)
{
    std::wstring d = L"Driver status: " + widen_ascii(sane_status_name(status));
    if (!driver_message.empty()) d += L"\nDriver message: " + widen_ascii(driver_message);
    return d;
}

const wchar_t kReplugStep[] =
    L"Unplug the scanner's USB cable, wait five seconds and plug it back in. The app reconnects by itself.";
const wchar_t kPortStep[] =
    L"Plug the scanner straight into a USB port on the computer (on a desktop, one at the back), not into a hub, "
    L"keyboard or monitor. The LiDE 50 is powered over USB and needs a port that gives full power.";
const wchar_t kCableStep[] = L"If that doesn't help, try another USB cable and another port.";
const wchar_t kLidStep[] =
    L"Close the lid completely. The scanner calibrates against a white strip under the lid, and room light or an "
    L"open lid throws that off.";

std::wstring calib_numbers(const Lide50CalibReport& c)
{
    return fmt(L"LED calibration: %ls after %d tries; brightness R %u, G %u, B %u (target %u); exposure %u, %u, %u. "
               L"White strip levels R %u, G %u, B %u.",
               c.led_result == LIDE50_LED_OK ? L"settled" : c.led_result == LIDE50_LED_UNSETTLED ? L"did not settle"
                                                                                                    : L"not run",
               c.led_tries, c.led_average[0], c.led_average[1], c.led_average[2], c.led_target, c.led_exposure[0],
               c.led_exposure[1], c.led_exposure[2], c.white_level[0], c.white_level[1], c.white_level[2]);
}

} // namespace

const wchar_t* colour_name(int channel)
{
    switch (channel) {
        case 0: return L"red";
        case 1: return L"green";
        case 2: return L"blue";
        default: return L"lamp";
    }
}

std::string sane_status_name(int status)
{
    switch (status) {
        case SANE_STATUS_GOOD: return "GOOD";
        case SANE_STATUS_UNSUPPORTED: return "UNSUPPORTED";
        case SANE_STATUS_CANCELLED: return "CANCELLED";
        case SANE_STATUS_DEVICE_BUSY: return "DEVICE_BUSY";
        case SANE_STATUS_INVAL: return "INVAL";
        case SANE_STATUS_EOF: return "EOF";
        case SANE_STATUS_JAMMED: return "JAMMED";
        case SANE_STATUS_NO_DOCS: return "NO_DOCS";
        case SANE_STATUS_COVER_OPEN: return "COVER_OPEN";
        case SANE_STATUS_IO_ERROR: return "IO_ERROR";
        case SANE_STATUS_NO_MEM: return "NO_MEM";
        case SANE_STATUS_ACCESS_DENIED: return "ACCESS_DENIED";
        default: return "status " + std::to_string(status);
    }
}

std::wstring Problem::report_text() const
{
    std::wstring s = title + L" [" + widen_ascii(code) + L"]\n" + what + L"\n";
    for (size_t i = 0; i < steps.size(); ++i) s += std::to_wstring(i + 1) + L". " + steps[i] + L"\n";
    if (!details.empty()) s += L"\nDetails:\n" + details + L"\n";
    return s;
}

// ------------------------------------------------------------- connection

Problem problem_not_connected()
{
    Problem p;
    p.code = "USB-NOT-FOUND";
    p.title = L"The scanner isn't connected";
    p.what = L"Windows doesn't see a Canon LiDE 50 on any USB port.";
    p.steps = {kPortStep,
               L"Wait a few seconds after plugging in. Windows plays its device sound when it notices the scanner.",
               kCableStep,
               L"If Windows still doesn't react, try the scanner on another computer to rule out the cable or "
               L"scanner."};
    return p;
}

Problem problem_device_error(unsigned cm, const std::wstring& service)
{
    Problem p;
    p.details = fmt(L"Device Manager problem code %u, driver '%ls'.", cm, service.c_str());
    if (cm == 22) {  // CM_PROB_DISABLED
        p.code = "USB-DISABLED";
        p.title = L"The scanner is turned off in Device Manager";
        p.what = L"Windows sees the scanner, but the device has been disabled.";
        p.steps = {L"Open Device Manager (the button below), find the CanoScan or WinUsb Device entry, "
                   L"right-click it and choose Enable device.",
                   L"Return to LiDE50 Scan. It connects as soon as the device is enabled."};
        p.fix = FixAction::OpenDeviceManager;
        p.fix_label = L"Open Device Manager";
        return p;
    }
    if (cm == 28 || cm == 1 || cm == 18) {  // no driver / not configured / reinstall
        p.code = "USB-NEEDS-SETUP";
        p.title = L"The scanner needs the one-time setup";
        p.what = L"Windows sees the scanner but has no driver assigned to it yet.";
        p.steps = {L"Click Enable scanner access and approve the Windows prompt. This assigns Windows' own WinUSB "
                   L"driver; nothing is downloaded or installed."};
        p.fix = FixAction::EnableAccess;
        p.fix_label = L"Enable scanner access";
        return p;
    }
    p.code = "USB-DEVICE-ERROR";
    p.title = L"Windows reports a problem with the scanner";
    if (cm == 43)
        p.what = L"Windows stopped the scanner because it reported a USB error (Device Manager code 43). This is "
                 L"usually power or a loose cable.";
    else if (cm == 10)
        p.what = L"Windows couldn't start the scanner (Device Manager code 10).";
    else
        p.what = fmt(L"Windows couldn't use the scanner (Device Manager code %u).", cm);
    p.steps = {kReplugStep, kPortStep, kCableStep,
               L"Restart the computer if the problem stays. Windows sometimes keeps a USB port in an error state "
               L"until then."};
    p.fix = FixAction::OpenDeviceManager;
    p.fix_label = L"Open Device Manager";
    return p;
}

Problem problem_other_driver(const std::wstring& service)
{
    Problem p;
    p.code = "USB-OTHER-DRIVER";
    p.title = L"Another driver is using the scanner";
    p.what = L"The scanner is assigned to a different driver" +
             (service.empty() ? std::wstring() : L" (\"" + service + L"\")") +
             L", for example Canon's own scanner driver. LiDE50 Scan can't talk to it while that driver owns it.";
    p.steps = {L"Click Enable scanner access to switch the scanner to Windows' WinUSB driver, which LiDE50 Scan "
               L"uses.",
               L"Canon's software won't see the scanner after the switch. To go back later, use \"Remove scanner "
               L"access\" in the ⋯ menu and unplug and replug the scanner."};
    p.details = L"Current driver service: " + service;
    p.fix = FixAction::EnableAccess;
    p.fix_label = L"Switch to LiDE50 Scan";
    return p;
}

Problem problem_open_failed(int status, const std::string& msg)
{
    Problem p;
    p.details = driver_detail(status, msg);
    if (status == SANE_STATUS_ACCESS_DENIED || status == SANE_STATUS_DEVICE_BUSY) {
        p.code = "USB-IN-USE";
        p.title = L"The scanner is in use by another program";
        p.what = L"Windows wouldn't let LiDE50 Scan open the scanner. Usually another program has it open.";
        p.steps = {L"Close other scanning programs (another copy of LiDE50 Scan, VueScan, Windows Scan, NAPS2 or "
                   L"Canon software), then click Try again.",
                   kReplugStep,
                   L"If this keeps happening, run the one-time setup again from the ⋯ menu (Enable scanner "
                   L"access)."};
        p.fix = FixAction::TryAgain;
        p.fix_label = L"Try again";
        return p;
    }
    if (status == SANE_STATUS_IO_ERROR) {
        p.code = "USB-OPEN-IO";
        p.title = L"The scanner didn't answer";
        p.what = L"LiDE50 Scan found the scanner but couldn't talk to it over USB.";
        p.steps = {kReplugStep, kPortStep, kCableStep};
        p.fix = FixAction::TryAgain;
        p.fix_label = L"Try again";
        return p;
    }
    p.code = "USB-OPEN";
    p.title = L"The scanner couldn't be opened";
    p.what = L"Starting the built-in scanner driver didn't work.";
    p.steps = {kReplugStep, L"Close and reopen LiDE50 Scan.", kPortStep};
    p.fix = FixAction::TryAgain;
    p.fix_label = L"Try again";
    return p;
}

// ---------------------------------------------------------------- scanning

Problem problem_scan_start(int status, const std::string& msg, const Lide50CalibReport& cal)
{
    Problem p;
    p.details = driver_detail(status, msg);
    if (cal.ran) p.details += L"\n" + calib_numbers(cal);

    if (status == SANE_STATUS_JAMMED || cal.low_light_channel >= 0) {
        p.code = "CAL-NO-LIGHT";
        p.title = L"The scanner couldn't see its light";
        const int ch = cal.low_light_channel;
        const bool one_colour = ch >= 0 && ch <= 2 &&
                                (cal.white_level[(ch + 1) % 3] > 2000 || cal.white_level[(ch + 2) % 3] > 2000);
        if (one_colour)
            p.what = fmt(L"While calibrating, the sensor saw almost no %ls light, although the other colours were "
                         L"fine. The head isn't locked; the %ls light didn't reach the sensor.",
                         colour_name(ch), colour_name(ch));
        else
            p.what = L"While calibrating, the sensor saw almost no light. The scan head is very rarely locked; "
                     L"usually the light or the sensor connection had a hiccup.";
        p.steps = {kLidStep, kReplugStep,
                   L"Look at the underside of the scanner. If there is a lock switch, slide it to the unlocked "
                   L"position.",
                   L"Click Try again. Each scan starts a fresh calibration."};
        if (one_colour)
            p.steps.push_back(fmt(L"If it fails every time with the %ls light, that LED in the scan head may have "
                                  L"failed. Scanning in Grayscale may still work.",
                                  colour_name(ch)));
        p.fix = FixAction::TryAgain;
        p.fix_label = L"Try again";
        return p;
    }
    if (status == SANE_STATUS_IO_ERROR) {
        p.code = "USB-LOST";
        p.title = L"The connection to the scanner was lost";
        p.what = L"The scanner stopped answering while getting ready to scan.";
        p.steps = {kReplugStep, kPortStep,
                   L"On a laptop, plug in the charger. Some laptops cut USB power on battery.",
                   L"Then click Try again."};
        p.fix = FixAction::TryAgain;
        p.fix_label = L"Try again";
        return p;
    }
    if (status == SANE_STATUS_NO_MEM) {
        p = problem_not_enough_memory(0, 0, true);
        p.details = driver_detail(status, msg);
        return p;
    }
    if (status == SANE_STATUS_ACCESS_DENIED || status == SANE_STATUS_DEVICE_BUSY) {
        p = problem_open_failed(status, msg);
        return p;
    }
    if (status == SANE_STATUS_COVER_OPEN) {
        p.code = "COVER-OPEN";
        p.title = L"The lid is open";
        p.what = L"The scanner reported that its lid is open.";
        p.steps = {kLidStep, L"Click Try again."};
        p.fix = FixAction::TryAgain;
        p.fix_label = L"Try again";
        return p;
    }
    p.code = "SCAN-START";
    p.title = L"The scan couldn't start";
    p.what = L"The scanner driver stopped while getting ready to scan.";
    p.steps = {L"Click Try again.", kReplugStep,
               L"If it keeps failing, use Copy details and include the text when you report the problem."};
    p.fix = FixAction::TryAgain;
    p.fix_label = L"Try again";
    return p;
}

Problem problem_scan_read(int status, const std::string& msg, int lines, int total)
{
    Problem p;
    p.details = driver_detail(status, msg) + fmt(L"\nLines received: %d of %d.", lines, total);
    if (status == SANE_STATUS_NO_MEM) {
        p = problem_not_enough_memory(0, 0, false);
        p.details = driver_detail(status, msg);
        return p;
    }
    p.code = lines > 0 ? "SCAN-INTERRUPTED" : "SCAN-NO-DATA";
    p.title = lines > 0 ? L"The scan stopped partway" : L"The scanner sent no image";
    p.what = status == SANE_STATUS_IO_ERROR
                 ? L"The USB connection to the scanner dropped during the scan."
                 : L"The scanner driver reported an error while the image was coming in.";
    p.steps = {kReplugStep, kPortStep,
               L"Don't move the scanner or press on the lid while it scans.",
               L"On a laptop, plug in the charger.",
               L"Click Try again."};
    p.fix = FixAction::TryAgain;
    p.fix_label = L"Try again";
    return p;
}

Problem problem_scan_settings(const std::string& option, int status)
{
    Problem p;
    p.code = "SCAN-SETTINGS";
    p.title = L"The scan settings weren't accepted";
    p.what = L"The scanner driver refused one of the settings for this scan, so the scan was not started to "
             L"avoid a wrong result.";
    p.steps = {L"Choose a different resolution or a slightly smaller area and try again.", kReplugStep};
    p.details = L"Option '" + widen_ascii(option) + L"': " + widen_ascii(sane_status_name(status));
    return p;
}

Problem problem_bad_image_format(const std::string& details)
{
    Problem p;
    p.code = "SCAN-FORMAT";
    p.title = L"The scanner described an image the app can't use";
    p.what = L"The image size or format reported by the driver didn't make sense, so the scan was stopped.";
    p.steps = {L"Click Try again.", L"Pick a different resolution.", kReplugStep};
    p.details = widen_ascii(details);
    p.fix = FixAction::TryAgain;
    p.fix_label = L"Try again";
    return p;
}

Problem problem_not_enough_memory(double needed_mb, double available_mb, bool before_start)
{
    Problem p;
    p.code = "NO-MEMORY";
    p.title = L"Not enough memory for this scan";
    if (needed_mb > 0)
        p.what = fmt(L"This scan needs about %.0f MB of memory, but only about %.0f MB is free%ls.", needed_mb,
                     available_mb, before_start ? L", so it wasn't started" : L"");
    else
        p.what = L"The computer ran out of memory during the scan.";
    p.steps = {L"Select a smaller area on the preview, or choose a lower resolution. 300 dpi is enough for "
               L"documents and 600 dpi for photos.",
               L"Save the scans you want to keep, then use Clear all scans in the ⋯ menu.",
               L"Close other programs, especially web browsers with many tabs."};
    return p;
}

Problem problem_engine_stopped(const std::string& details)
{
    Problem p;
    p.code = "ENGINE-STOPPED";
    p.title = L"The scanner part of the app stopped";
    p.what = L"An internal error stopped the part of LiDE50 Scan that talks to the scanner. Your scans so far are "
             L"still here.";
    p.steps = {L"Save the scans you want to keep.", L"Close and reopen LiDE50 Scan.",
               L"Use Copy details and include the text when you report the problem."};
    p.details = widen_ascii(details);
    return p;
}

Problem problem_unexpected(const std::string& where, const std::string& details)
{
    Problem p;
    p.code = "UNEXPECTED";
    p.title = L"Something went wrong";
    p.what = L"LiDE50 Scan ran into an unexpected error " + widen_ascii(where) +
             L". The app is still running and your scans are safe.";
    p.steps = {L"Try the same thing again.", L"If it happens again, close and reopen LiDE50 Scan.",
               L"Use Copy details and include the text when you report the problem."};
    p.details = widen_ascii(details);
    return p;
}

// ---------------------------------------------------------------- warnings

Problem warning_led_unsettled(const Lide50CalibReport& cal)
{
    Problem p;
    p.warning = true;
    p.details = calib_numbers(cal);
    if (cal.led_saturated) {
        p.code = "CAL-STRAY-LIGHT";
        p.title = L"Too much light reached the sensor";
        p.what = L"During calibration the sensor was overexposed even with the shortest lamp setting, so the "
                 L"colours and brightness of this scan may be off.";
        p.steps = {kLidStep,
                   L"For a thick book that keeps the lid open, cover the scanner with a dark cloth while it "
                   L"scans.",
                   L"Move the scanner out of direct sunlight or away from a bright desk lamp.",
                   L"Scan again."};
        return p;
    }
    int ch = cal.led_dim_channel >= 0 ? cal.led_dim_channel : cal.led_bright_channel;
    p.code = "CAL-COLOUR";
    p.title = L"Colours may be off in this scan";
    if (cal.led_dim_channel >= 0)
        p.what = fmt(L"The %ls light stayed too weak during calibration, even at its strongest setting. The scan "
                     L"was made, but it may have a colour cast.",
                     colour_name(ch));
    else
        p.what = fmt(L"The %ls light couldn't be adjusted to the right level during calibration. The scan was "
                     L"made, but it may have a colour cast.",
                     colour_name(ch));
    p.steps = {kLidStep,
               L"Clean the glass and the white strip on the inside of the lid, at the end where the head rests.",
               L"Scan again. Calibration starts fresh for every scan and nothing wrong is remembered.",
               fmt(L"If the %ls light is weak every time, that LED is likely wearing out. Grayscale scans are "
                   L"less affected.",
                   colour_name(ch))};
    return p;
}

Problem warning_one_colour(int strong, const double m[3])
{
    Problem p;
    p.warning = true;
    p.code = "SCAN-ONE-COLOUR";
    p.title = fmt(L"This scan is almost entirely %ls", colour_name(strong));
    p.what = L"Two of the three colours are nearly missing from the scan. That usually means a lamp colour "
             L"didn't light during the scan, not that the page is really this colour.";
    p.steps = {kLidStep, kReplugStep, L"Scan again.",
               L"If it keeps happening, use Copy details and include the text when you report the problem."};
    p.details = fmt(L"Average levels R %.0f, G %.0f, B %.0f (0-255).", m[0], m[1], m[2]);
    return p;
}

Problem warning_blank(bool all_dark, double mean)
{
    Problem p;
    p.warning = true;
    p.code = all_dark ? "SCAN-BLACK" : "SCAN-BLANK";
    p.title = all_dark ? L"The scan came out black" : L"The scan looks empty";
    p.what = all_dark ? L"The whole image is nearly black, so the lamp probably didn't light."
                      : L"The whole image is a single plain shade.";
    if (all_dark)
        p.steps = {kLidStep, kReplugStep, L"Scan again."};
    else
        p.steps = {L"Check that the page lies face down on the glass.",
                   L"Make a preview and check that the selected area covers the page.", L"Scan again."};
    p.details = fmt(L"Average level %.1f (0-255).", mean);
    return p;
}

Problem warning_head_return(int status, const std::string& msg)
{
    Problem p;
    p.warning = true;
    p.code = "HEAD-RETURN";
    p.title = L"The scan head may not have returned";
    p.what = L"Your scan is complete, but the scanner reported a problem while moving the head back.";
    p.steps = {L"If the next scan fails, unplug the scanner, wait five seconds and plug it back in. The head "
               L"returns by itself."};
    p.details = driver_detail(status, msg);
    return p;
}

// ------------------------------------------------------------------ saving

Problem problem_save(unsigned long e, const std::wstring& path, double needed_mb, double free_mb,
                     const std::wstring& detail)
{
    Problem p;
    std::wstring folder = path.substr(0, path.find_last_of(L"\\/"));
    p.details = fmt(L"Windows error %lu. ", e) + detail + L"\nFile: " + path;
    switch (e) {
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
            p.code = "SAVE-DENIED";
            p.title = L"Windows didn't allow saving there";
            p.what = L"LiDE50 Scan isn't allowed to write to " + folder + L".";
            p.steps = {L"Windows Security's ransomware protection (Controlled folder access) blocks unknown apps "
                       L"from Documents, Pictures and the Desktop. Either save to another folder, such as a "
                       L"new C:\\Scans folder, or allow LiDE50Scan.exe under Windows Security > Virus & threat "
                       L"protection > Ransomware protection > Allow an app.",
                       L"If the folder is on a USB stick or SD card, check that it isn't write-protected.",
                       L"If a file with this name already exists and is read-only, choose a different name."};
            p.fix = FixAction::OpenRansomwareSettings;
            p.fix_label = L"Open ransomware protection";
            return p;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
            p.code = "SAVE-IN-USE";
            p.title = L"The file is open in another program";
            p.what = L"Another program has " + path.substr(path.find_last_of(L"\\/") + 1) +
                     L" open, so it can't be replaced.";
            p.steps = {L"Close the file in the other program (for example Word, a PDF viewer or a photo viewer), "
                       L"then save again.",
                       L"Or save under a different name."};
            return p;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            p.code = "SAVE-DISK-FULL";
            p.title = L"The disk is full";
            p.what = needed_mb > 0 ? fmt(L"The file needs about %.0f MB, but the drive has only %.0f MB free.",
                                         needed_mb, free_mb)
                                   : std::wstring(L"The drive ran out of space while saving.");
            p.steps = {L"Save to another drive, or free up space and save again.",
                       L"JPEG and PDF files are much smaller than TIFF, PNG or BMP."};
            return p;
        case ERROR_PATH_NOT_FOUND:
        case ERROR_FILE_NOT_FOUND:
        case ERROR_BAD_NETPATH:
        case ERROR_NETNAME_DELETED:
        case ERROR_NOT_READY:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_INVALID_DRIVE:
            p.code = "SAVE-FOLDER-GONE";
            p.title = L"The folder isn't available";
            p.what = L"The folder " + folder + L" can't be reached. It may be on a drive or network share that "
                     L"was disconnected.";
            p.steps = {L"Reconnect the drive or network share, or choose another folder, then save again."};
            return p;
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_INVALID_NAME:
            p.code = "SAVE-NAME";
            p.title = L"That file name can't be used";
            p.what = L"The name or folder path is too long or contains characters Windows doesn't allow.";
            p.steps = {L"Choose a shorter name or a folder closer to the top of the drive, then save again."};
            return p;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return problem_save_memory(needed_mb);
        default:
            p.code = "SAVE-FAILED";
            p.title = L"The file wasn't saved";
            p.what = L"Windows reported an error while writing the file. Nothing was changed in the existing "
                     L"file, if there was one.";
            p.steps = {L"Save again, or choose another folder.",
                       L"If it keeps failing, use Copy details and include the text when you report the "
                       L"problem."};
            return p;
    }
}

Problem problem_save_memory(double needed_mb)
{
    Problem p;
    p.code = "SAVE-NO-MEMORY";
    p.title = L"Not enough memory to save";
    p.what = needed_mb > 0 ? fmt(L"Saving needs about %.0f MB of free memory.", needed_mb)
                           : std::wstring(L"The computer ran out of memory while preparing the file.");
    p.steps = {L"Save the scans one at a time instead of all together.",
               L"Close other programs, then save again.",
               L"JPEG or PDF needs less memory than TIFF or PNG."};
    return p;
}

// ------------------------------------------------------------------- setup

Problem problem_setup_cancelled()
{
    Problem p;
    p.code = "SETUP-CANCELLED";
    p.title = L"Setup was cancelled";
    p.what = L"The Windows permission prompt was closed, so nothing was changed.";
    p.steps = {L"Click Enable scanner access again and choose Yes in the Windows prompt.",
               L"If you can't approve it (no administrator rights), ask someone who can, or follow the manual "
               L"steps."};
    p.fix = FixAction::EnableAccess;
    p.fix_label = L"Enable scanner access";
    return p;
}

Problem problem_setup_failed(const std::wstring& details)
{
    Problem p;
    p.code = "SETUP-FAILED";
    p.title = L"Automatic setup didn't work";
    p.what = L"Windows wouldn't assign its WinUSB driver to the scanner automatically. You can do it by hand in "
             L"Device Manager in about a minute.";
    p.steps = {L"Unplug the scanner and plug it back in, then try Enable scanner access once more.",
               L"If it still fails, click Manual setup steps and follow them."};
    p.details = details;
    p.fix = FixAction::ManualSetup;
    p.fix_label = L"Manual setup steps";
    return p;
}

Problem problem_setup_timeout()
{
    Problem p;
    p.code = "SETUP-TIMEOUT";
    p.title = L"Setup is taking very long";
    p.what = L"Windows didn't finish assigning the driver within two minutes.";
    p.steps = {L"Wait another minute; the scanner may still appear as ready.", kReplugStep,
               L"If it doesn't, restart the computer and try Enable scanner access again."};
    return p;
}

Problem problem_setup_not_started(unsigned long e)
{
    Problem p;
    p.code = "SETUP-NOT-STARTED";
    p.title = L"Setup couldn't start";
    p.what = L"Windows didn't start the setup step.";
    p.steps = {L"Make sure LiDE50Scan.exe isn't inside a ZIP file. Extract the whole folder first.",
               L"Try again, or follow the manual setup steps."};
    p.details = fmt(L"Windows error %lu.", e);
    p.fix = FixAction::ManualSetup;
    p.fix_label = L"Manual setup steps";
    return p;
}

// --------------------------------------------------------------------- app

Problem problem_previous_crash(const std::wstring& when, const std::wstring& what)
{
    Problem p;
    p.code = "PREVIOUS-CRASH";
    p.title = L"LiDE50 Scan closed unexpectedly last time";
    p.what = L"The app stopped " + (when.empty() ? std::wstring(L"last time") : L"on " + when) +
             L". Scans that weren't saved then are lost, sorry.";
    p.steps = {L"Save scans soon after making them; a large scan at a high resolution needs a lot of memory.",
               L"If it happens again, use Copy details right after restarting, and include the text when you "
               L"report the problem. It contains the crash report."};
    p.details = what;
    return p;
}

Problem problem_display(const std::string& details)
{
    Problem p;
    p.code = "DISPLAY";
    p.title = L"The screen couldn't be drawn";
    p.what = L"Windows' graphics system reported an error. LiDE50 Scan will try again.";
    p.steps = {L"If the window stays blank, close and reopen the app.",
               L"Updating the graphics driver can help."};
    p.details = widen_ascii(details);
    return p;
}

// ---------------------------------------------------------- troubleshooting

Problem help_not_found()
{
    Problem p = problem_not_connected();
    p.code = "HELP-NOT-FOUND";
    p.title = L"Help: the scanner isn't found";
    p.steps.push_back(L"If the sidebar says \"Needs permission\", click Enable scanner access once.");
    p.steps.push_back(L"If you used Canon's own driver before, the one-time setup switches the scanner over.");
    return p;
}

Problem help_colours()
{
    Problem p;
    p.code = "HELP-COLOURS";
    p.title = L"Help: colours look wrong or green";
    p.what = L"The scanner calibrates its red, green and blue lights before every scan. Light from outside the "
             L"scanner, a dirty calibration strip or an ageing LED can throw the colours off.";
    p.steps = {kLidStep,
               L"Clean the glass and the white strip inside the lid, at the end where the scan head rests.",
               L"Scan again. LiDE50 Scan checks every calibration and warns when a colour didn't settle.",
               L"If one colour is weak every time, that LED is probably wearing out. Grayscale scans are less "
               L"affected."};
    return p;
}

Problem help_noise_stretched()
{
    Problem p;
    p.code = "HELP-MOTOR";
    p.title = L"Help: grinding noise or stretched scans";
    p.what = L"A rough noise during a scan means the motor is slipping. The image then comes out stretched or "
             L"wavy from top to bottom. At 75 to 200 dpi, and in previews, the motor runs at full speed, where "
             L"an older mechanism slips most easily.";
    p.steps = {L"Put the scanner on a flat, firm surface and don't press on the lid while it scans.",
               kPortStep,
               L"Try 300 dpi or higher: the motor runs more slowly and smoothly there. You can reduce the size "
               L"when saving.",
               L"If it happens at every resolution, use Copy details and include which resolution you used "
               L"when you report the problem."};
    return p;
}

Problem help_crashes()
{
    Problem p;
    p.code = "HELP-CRASH";
    p.title = L"Help: the app closes or crashes";
    p.what = L"Every crash is written to LiDE50Scan.log next to the app, with the details needed to fix it.";
    p.steps = {L"Restart LiDE50 Scan. It shows what happened last time.",
               L"Use Copy details (or open the log from the ⋯ menu) and include the text when you report the "
               L"problem.",
               L"Very large scans need a lot of memory: select a smaller area or use a lower resolution."};
    return p;
}
