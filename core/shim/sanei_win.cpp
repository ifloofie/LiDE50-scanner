// Minimal Windows replacements for the sanei_* helpers used by the genesys
// backend: USB access (on top of libusb-1.0 / WinUSB), configuration and
// debug output. Only what the Canon LiDE 50 path needs is implemented.
//
// All activity is reported to the application's log through a sink
// (lide50_set_log_sink). A built-in fake GL841 chip (LIDE50_FAKE_USB=1) lets
// the real-hardware code paths of the driver be exercised without a scanner.
//
// Part of LiDE50 Scan. Licensed GPL-2.0-or-later (same as SANE backends).

#include "../include/sane/config.h"
#include "../include/sane/sane.h"
#include "../include/sane/sanei.h"
#include "../include/sane/sanei_usb.h"
#include "../include/sane/sanei_config.h"

#include <libusb.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
typedef void (*lide50_log_sink_fn)(const char* category, const char* text);
}

namespace {

struct UsbDev {
    std::string name;
    libusb_device* dev = nullptr;
    libusb_device_handle* handle = nullptr;
    int vendor = 0;
    int product = 0;
    int bcd_device = 0;
    int interface_nr = 0;
    int alt_setting = 0;
    unsigned char bulk_in_ep = 0;
    unsigned char bulk_out_ep = 0;
    unsigned char int_in_ep = 0;
    bool open = false;
    bool fake = false;
};

libusb_context* g_ctx = nullptr;
int g_init_count = 0;
int g_timeout_ms = 30000;
std::vector<UsbDev> g_devices;
lide50_log_sink_fn g_sink = nullptr;
std::atomic<bool> g_trace{false};
bool g_fake = false;

// transfer statistics, reported when the device is closed
unsigned long long g_ctrl_count = 0, g_bulk_in_count = 0, g_bulk_out_count = 0;
unsigned long long g_bulk_in_bytes = 0, g_bulk_out_bytes = 0, g_error_count = 0;

void usb_log(const char* category, const char* fmt, ...)
{
    if (!g_sink) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_sink(category, buf);
}

std::string hex_bytes(const unsigned char* d, int len, int max = 16)
{
    std::string s;
    char b[4];
    for (int i = 0; i < len && i < max; ++i) {
        std::snprintf(b, sizeof(b), "%02x ", d[i]);
        s += b;
    }
    if (len > max) s += "...";
    return s;
}

bool valid(SANE_Int dn) { return dn >= 0 && dn < static_cast<SANE_Int>(g_devices.size()); }

void free_device_list()
{
    for (auto& d : g_devices) {
        if (d.handle) { libusb_close(d.handle); d.handle = nullptr; }
        if (d.dev) { libusb_unref_device(d.dev); d.dev = nullptr; }
    }
    g_devices.clear();
}

// ------------------------------------------------------------ fake GL841

struct FakeGl841 {
    unsigned char regs[256] = {};
    unsigned char cur = 0;
    size_t pending_in = 0;
    unsigned seed = 12345;

    unsigned char read(unsigned char a)
    {
        switch (a) {
            case 0x41: {
                // power, feed+scan finished, at home, buffer not empty.
                // LIDE50_FAKE_AWAY=<n>: head reports "not at home, motor running" for the first n reads
                static long away = [] {
                    const char* e = std::getenv("LIDE50_FAKE_AWAY");
                    return e ? std::atol(e) : 0L;
                }();
                if (away > 0) { --away; return 0x80 | 0x01; }
                return 0x80 | 0x20 | 0x10 | 0x08;
            }
            case 0x42: return 0x0f;                      // valid words (plenty)
            case 0x43: return 0xff;
            case 0x44: return 0xff;
            case 0x48: return 0x0f;                      // feed steps (head reached position)
            case 0x49: case 0x4a: return 0xff;
            case 0x4b: return 0x0f;                      // scan counter
            case 0x4c: case 0x4d: return 0xff;
            case 0x6d: return static_cast<unsigned char>(regs[0x6d] | 0x0f); // buttons released
            default: return regs[a];
        }
    }

    void control(int rtype, int req, int value, int len, unsigned char* data)
    {
        const bool in = (rtype & 0x80) != 0;
        if (!in && req == 0x0c && value == 0x83 && len >= 1) cur = data[0];
        else if (!in && req == 0x0c && value == 0x85 && len >= 1) regs[cur] = data[0];
        else if (in && req == 0x0c && value == 0x84 && len >= 1) data[0] = read(cur);
        else if (!in && req == 0x04 && value == 0x83) {
            for (int i = 0; i + 1 < len; i += 2) regs[data[i]] = data[i + 1];
        } else if (!in && req == 0x04 && value == 0x82 && len >= 8) {
            size_t size = data[4] | (data[5] << 8) | (data[6] << 16) | (static_cast<size_t>(data[7]) << 24);
            if (data[0] == 0x00) pending_in = size; // BULK_IN
        } else if (in) {
            std::memset(data, 0, static_cast<size_t>(len));
        }
    }

    void bulk_read(unsigned char* buf, size_t& size)
    {
        // plausible sensor data: bright paper with some noise
        // (LIDE50_FAKE_DATA=<0..255> forces a constant value for robustness tests)
        static int forced = [] {
            const char* e = std::getenv("LIDE50_FAKE_DATA");
            return e ? std::atoi(e) : -1;
        }();
        if (forced >= 0) {
            std::memset(buf, forced, size);
            pending_in = pending_in > size ? pending_in - size : 0;
            return;
        }
        for (size_t i = 0; i < size; ++i) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = static_cast<unsigned char>(0xb0 + ((seed >> 16) & 0x1f));
        }
        pending_in = pending_in > size ? pending_in - size : 0;
    }
};

FakeGl841 g_fake_chip;

} // namespace

extern "C" {

// --------------------------------------------------------- app interface

void lide50_set_log_sink(lide50_log_sink_fn sink) { g_sink = sink; }
void lide50_set_usb_trace(int on) { g_trace = on != 0; }

// ---------------------------------------------------------------- debug

void sanei_init_debug(const char* backend, int* debug_level_var)
{
    std::string var = "SANE_DEBUG_";
    for (const char* p = backend; *p; ++p) var += static_cast<char>(std::toupper(*p));
    const char* v = std::getenv(var.c_str());
    *debug_level_var = v ? std::atoi(v) : 0;
}

void sanei_debug_msg(int level, int max_level, const char* /*be*/, const char* fmt, va_list ap)
{
    if (max_level < level || !g_sink) return;
    char buf[2048];
    va_list copy;
    va_copy(copy, ap);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof(buf)) {
        g_sink("DRV", buf);
    } else {
        std::vector<char> big(static_cast<size_t>(n) + 1);
        std::vsnprintf(big.data(), big.size(), fmt, ap);
        g_sink("DRV", big.data());
    }
}

void sanei_debug_ndebug(int, const char*, ...) {}

// --------------------------------------------------------------- config

SANE_Status sanei_configure_attach(const char*, SANEI_Config* config,
                                   SANE_Status (*config_attach)(SANEI_Config*, const char*, void*),
                                   void* data)
{
    // Built-in configuration: this build knows exactly one scanner.
    return config_attach(config, "usb 0x04a9 0x2213", data);
}

// ------------------------------------------------------------------ usb

SANE_Bool sanei_usb_is_replay_mode_enabled() { return SANE_FALSE; }
void sanei_usb_testing_record_message(SANE_String_Const) {}
void sanei_usb_testing_record_clear() {}

void sanei_usb_init(void)
{
    if (g_init_count++ == 0) {
        const char* f = std::getenv("LIDE50_FAKE_USB");
        g_fake = f && *f && *f != '0';
        if (g_fake) {
            usb_log("USB", "USB layer started in FAKE-DEVICE test mode (no real scanner is used)");
        } else {
            const libusb_version* v = libusb_get_version();
            // send libusb's own warnings/errors to the application log
            libusb_set_log_cb(nullptr, [](libusb_context*, enum libusb_log_level lvl, const char* str) {
                if (!g_sink || !str) return;
                std::string t = str;
                while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
                g_sink(lvl <= LIBUSB_LOG_LEVEL_ERROR ? "ERROR" : (lvl == LIBUSB_LOG_LEVEL_WARNING ? "WARN" : "USB"),
                       ("libusb: " + t).c_str());
            }, LIBUSB_LOG_CB_GLOBAL);
            int r = libusb_init(&g_ctx);
            if (r == 0)
                libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL,
                                  g_trace ? LIBUSB_LOG_LEVEL_INFO : LIBUSB_LOG_LEVEL_WARNING);
            if (r != 0) {
                usb_log("ERROR", "libusb_init failed: %s", libusb_error_name(r));
                g_ctx = nullptr;
            } else {
                usb_log("USB", "USB layer started (libusb %u.%u.%u)", v->major, v->minor, v->micro);
            }
        }
    }
    sanei_usb_scan_devices();
}

void sanei_usb_exit(void)
{
    if (g_init_count <= 0) return;
    if (--g_init_count == 0) {
        free_device_list();
        if (g_ctx) libusb_exit(g_ctx);
        g_ctx = nullptr;
        usb_log("USB", "USB layer stopped");
    }
}

void sanei_usb_scan_devices(void)
{
    if (g_fake) {
        bool have = false;
        for (auto& d : g_devices) have = have || d.fake;
        if (!have) {
            UsbDev d;
            d.name = "fake:001";
            d.vendor = 0x04a9;
            d.product = 0x2213;
            d.bcd_device = 0x0100;
            d.bulk_in_ep = 0x81;
            d.bulk_out_ep = 0x02;
            d.int_in_ep = 0x83;
            d.fake = true;
            g_devices.push_back(d);
        }
        usb_log("USB", "scan for scanners: fake LiDE 50 present");
        return;
    }
    if (!g_ctx) return;
    // keep already-open devices, refresh the rest
    for (auto& d : g_devices) {
        if (!d.open && d.dev) { libusb_unref_device(d.dev); d.dev = nullptr; }
    }
    libusb_device** list = nullptr;
    ssize_t n = libusb_get_device_list(g_ctx, &list);
    if (n < 0) {
        usb_log("ERROR", "libusb_get_device_list failed: %s", libusb_error_name(static_cast<int>(n)));
        return;
    }
    int found = 0;
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor != 0x04a9 || desc.idProduct != 0x2213) continue; // LiDE 50 only
        ++found;
        char name[64];
        std::snprintf(name, sizeof(name), "libusb:%03u:%03u",
                      libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]));
        UsbDev* slot = nullptr;
        for (auto& d : g_devices) if (d.name == name) slot = &d;
        if (!slot) { g_devices.emplace_back(); slot = &g_devices.back(); slot->name = name; }
        if (slot->open) continue;
        slot->dev = libusb_ref_device(list[i]);
        slot->vendor = desc.idVendor;
        slot->product = desc.idProduct;
        slot->bcd_device = desc.bcdDevice;
        usb_log("USB", "found Canon LiDE 50 at %s (USB %x.%02x, device release %x.%02x, port speed %d)", name,
                desc.bcdUSB >> 8, desc.bcdUSB & 0xff, desc.bcdDevice >> 8, desc.bcdDevice & 0xff,
                libusb_get_device_speed(list[i]));
    }
    libusb_free_device_list(list, 1);
    if (!found) usb_log("USB", "scan for scanners: no Canon LiDE 50 (04A9:2213) on USB (%d USB devices checked)",
                        static_cast<int>(n));
    // drop entries that disappeared
    std::vector<UsbDev> keep;
    for (auto& d : g_devices) if (d.dev || d.open) keep.push_back(d);
    g_devices.swap(keep);
}

SANE_Status sanei_usb_get_vendor_product_byname(SANE_String_Const devname, SANE_Word* vendor,
                                                SANE_Word* product)
{
    for (auto& d : g_devices) {
        if (d.name == devname) {
            if (vendor) *vendor = d.vendor;
            if (product) *product = d.product;
            return SANE_STATUS_GOOD;
        }
    }
    return SANE_STATUS_INVAL;
}

SANE_Status sanei_usb_get_vendor_product(SANE_Int dn, SANE_Word* vendor, SANE_Word* product)
{
    if (!valid(dn)) return SANE_STATUS_INVAL;
    if (vendor) *vendor = g_devices[dn].vendor;
    if (product) *product = g_devices[dn].product;
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_find_devices(SANE_Int vendor, SANE_Int product,
                                   SANE_Status (*attach)(SANE_String_Const devname))
{
    std::vector<std::string> names;
    for (auto& d : g_devices)
        if (d.vendor == vendor && d.product == product && (d.dev || d.fake)) names.push_back(d.name);
    for (auto& n : names) if (attach) attach(n.c_str());
    return SANE_STATUS_GOOD;
}

void sanei_usb_attach_matching_devices(const char* name,
                                       SANE_Status (*attach)(const char* dev))
{
    unsigned vendor = 0, product = 0;
    if (std::sscanf(name, "usb %x %x", &vendor, &product) == 2) {
        sanei_usb_find_devices(static_cast<SANE_Int>(vendor), static_cast<SANE_Int>(product), attach);
    } else {
        attach(name);
    }
}

SANE_Status sanei_usb_open(SANE_String_Const devname, SANE_Int* dn)
{
    for (size_t i = 0; i < g_devices.size(); ++i) {
        UsbDev& d = g_devices[i];
        if (d.name != devname) continue;
        if (d.open) {
            usb_log("ERROR", "open %s: already open", devname);
            return SANE_STATUS_DEVICE_BUSY;
        }
        if (d.fake) {
            d.open = true;
            *dn = static_cast<SANE_Int>(i);
            usb_log("USB", "opened %s (fake device)", devname);
            return SANE_STATUS_GOOD;
        }
        if (!d.dev) return SANE_STATUS_INVAL;

        int r = libusb_open(d.dev, &d.handle);
        if (r != 0) {
            usb_log("ERROR", "libusb_open(%s) failed: %s%s", devname, libusb_error_name(r),
                    r == LIBUSB_ERROR_NOT_SUPPORTED ? " (no WinUSB driver assigned to the scanner yet)" : "");
            d.handle = nullptr;
            if (r == LIBUSB_ERROR_ACCESS) return SANE_STATUS_ACCESS_DENIED;
            if (r == LIBUSB_ERROR_NOT_SUPPORTED) return SANE_STATUS_ACCESS_DENIED;
            return SANE_STATUS_IO_ERROR;
        }

        libusb_config_descriptor* cfg = nullptr;
        r = libusb_get_active_config_descriptor(d.dev, &cfg);
        if (r != 0) r = libusb_get_config_descriptor(d.dev, 0, &cfg);
        if (r != 0 || !cfg) {
            usb_log("ERROR", "could not read the USB configuration of %s: %s", devname, libusb_error_name(r));
            libusb_close(d.handle); d.handle = nullptr;
            return SANE_STATUS_IO_ERROR;
        }
        d.bulk_in_ep = d.bulk_out_ep = d.int_in_ep = 0;
        if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0) {
            const libusb_interface_descriptor& id = cfg->interface[0].altsetting[0];
            d.interface_nr = id.bInterfaceNumber;
            d.alt_setting = id.bAlternateSetting;
            for (int e = 0; e < id.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = id.endpoint[e];
                int type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                bool in = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
                usb_log("USB", "  endpoint 0x%02x type %d max packet %u", ep.bEndpointAddress, type,
                        ep.wMaxPacketSize);
                if (type == LIBUSB_TRANSFER_TYPE_BULK && in && !d.bulk_in_ep) d.bulk_in_ep = ep.bEndpointAddress;
                if (type == LIBUSB_TRANSFER_TYPE_BULK && !in && !d.bulk_out_ep) d.bulk_out_ep = ep.bEndpointAddress;
                if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in && !d.int_in_ep) d.int_in_ep = ep.bEndpointAddress;
            }
        }
        libusb_free_config_descriptor(cfg);

        r = libusb_claim_interface(d.handle, d.interface_nr);
        if (r != 0) {
            usb_log("ERROR", "libusb_claim_interface(%d) failed: %s", d.interface_nr, libusb_error_name(r));
            libusb_close(d.handle); d.handle = nullptr;
            return r == LIBUSB_ERROR_BUSY ? SANE_STATUS_DEVICE_BUSY : SANE_STATUS_IO_ERROR;
        }
        d.open = true;
        *dn = static_cast<SANE_Int>(i);
        g_ctrl_count = g_bulk_in_count = g_bulk_out_count = g_bulk_in_bytes = g_bulk_out_bytes = g_error_count = 0;
        usb_log("USB", "opened %s: interface %d, bulk-in 0x%02x, bulk-out 0x%02x, interrupt 0x%02x", devname,
                d.interface_nr, d.bulk_in_ep, d.bulk_out_ep, d.int_in_ep);
        return SANE_STATUS_GOOD;
    }
    usb_log("ERROR", "open %s: no such device", devname);
    return SANE_STATUS_INVAL;
}

void sanei_usb_close(SANE_Int dn)
{
    if (!valid(dn)) return;
    UsbDev& d = g_devices[dn];
    if (!d.open) return;
    if (!d.fake) {
        libusb_release_interface(d.handle, d.interface_nr);
        libusb_close(d.handle);
    }
    d.handle = nullptr;
    d.open = false;
    usb_log("USB", "closed %s (control transfers %llu, bulk in %llu = %llu bytes, bulk out %llu = %llu bytes, "
                   "errors %llu)",
            d.name.c_str(), g_ctrl_count, g_bulk_in_count, g_bulk_in_bytes, g_bulk_out_count, g_bulk_out_bytes,
            g_error_count);
}

void sanei_usb_set_timeout(SANE_Int timeout)
{
    if (timeout != g_timeout_ms) usb_log("USB", "USB timeout set to %d ms", timeout);
    g_timeout_ms = timeout;
}

void sanei_usb_set_endpoint(SANE_Int dn, SANE_Int ep_type, SANE_Int ep)
{
    if (!valid(dn)) return;
    if (ep_type == (USB_DIR_IN | USB_ENDPOINT_TYPE_BULK)) g_devices[dn].bulk_in_ep = static_cast<unsigned char>(ep);
    if (ep_type == (USB_DIR_OUT | USB_ENDPOINT_TYPE_BULK)) g_devices[dn].bulk_out_ep = static_cast<unsigned char>(ep);
}

SANE_Int sanei_usb_get_endpoint(SANE_Int dn, SANE_Int ep_type)
{
    if (!valid(dn)) return 0;
    if (ep_type == (USB_DIR_IN | USB_ENDPOINT_TYPE_BULK)) return g_devices[dn].bulk_in_ep;
    if (ep_type == (USB_DIR_OUT | USB_ENDPOINT_TYPE_BULK)) return g_devices[dn].bulk_out_ep;
    return 0;
}

SANE_Status sanei_usb_clear_halt(SANE_Int dn)
{
    if (!valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    UsbDev& d = g_devices[dn];
    usb_log("USB", "clear halt on bulk endpoints");
    if (d.fake) return SANE_STATUS_GOOD;
    int r1 = d.bulk_in_ep ? libusb_clear_halt(d.handle, d.bulk_in_ep) : 0;
    int r2 = d.bulk_out_ep ? libusb_clear_halt(d.handle, d.bulk_out_ep) : 0;
    if (r1 || r2) {
        usb_log("WARN", "clear halt failed: in=%s out=%s", libusb_error_name(r1), libusb_error_name(r2));
        return SANE_STATUS_INVAL;
    }
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_reset(SANE_Int dn)
{
    if (!valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    usb_log("USB", "reset device");
    if (g_devices[dn].fake) return SANE_STATUS_GOOD;
    int r = libusb_reset_device(g_devices[dn].handle);
    if (r != 0) usb_log("WARN", "reset failed: %s", libusb_error_name(r));
    return r == 0 ? SANE_STATUS_GOOD : SANE_STATUS_INVAL;
}

SANE_Status sanei_usb_read_bulk(SANE_Int dn, SANE_Byte* buffer, size_t* size)
{
    if (!size || !buffer || !valid(dn) || !g_devices[dn].open) {
        usb_log("ERROR", "bulk read called on a closed device or with no buffer");
        return SANE_STATUS_INVAL;
    }
    UsbDev& d = g_devices[dn];
    ++g_bulk_in_count;
    if (d.fake) {
        // LIDE50_FAKE_FAIL_AT=<n>: make the n-th bulk read fail (error-path testing)
        static long fail_at = [] {
            const char* e = std::getenv("LIDE50_FAKE_FAIL_AT");
            return e ? std::atol(e) : -1L;
        }();
        if (fail_at > 0 && static_cast<long>(g_bulk_in_count) == fail_at) {
            ++g_error_count;
            usb_log("ERROR", "bulk read of %zu bytes failed: LIBUSB_ERROR_IO (simulated)", *size);
            *size = 0;
            return SANE_STATUS_IO_ERROR;
        }
        g_fake_chip.bulk_read(buffer, *size);
        g_bulk_in_bytes += *size;
        if (g_trace) usb_log("USB", "bulk in  %zu bytes (fake)", *size);
        return SANE_STATUS_GOOD;
    }
    if (!d.bulk_in_ep) return SANE_STATUS_INVAL;
    if (*size > 0x7fffffff) *size = 0x7fffffff;
    int got = 0;
    int r = libusb_bulk_transfer(d.handle, d.bulk_in_ep, buffer, static_cast<int>(*size), &got,
                                 static_cast<unsigned>(g_timeout_ms));
    if (r < 0) {
        ++g_error_count;
        usb_log("ERROR", "bulk read of %zu bytes failed: %s (received %d bytes)", *size, libusb_error_name(r), got);
        libusb_clear_halt(d.handle, d.bulk_in_ep);
        *size = 0;
        return SANE_STATUS_IO_ERROR;
    }
    g_bulk_in_bytes += static_cast<unsigned>(got);
    if (g_trace || static_cast<size_t>(got) != *size)
        usb_log(static_cast<size_t>(got) != *size ? "WARN" : "USB", "bulk in  requested %zu, received %d bytes",
                *size, got);
    if (got == 0) { *size = 0; return SANE_STATUS_EOF; }
    *size = static_cast<size_t>(got);
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_write_bulk(SANE_Int dn, const SANE_Byte* buffer, size_t* size)
{
    if (!size || !buffer || !valid(dn) || !g_devices[dn].open) {
        usb_log("ERROR", "bulk write called on a closed device or with no buffer");
        return SANE_STATUS_INVAL;
    }
    UsbDev& d = g_devices[dn];
    ++g_bulk_out_count;
    if (d.fake) {
        g_bulk_out_bytes += *size;
        if (g_trace) usb_log("USB", "bulk out %zu bytes (fake)", *size);
        return SANE_STATUS_GOOD;
    }
    if (!d.bulk_out_ep) return SANE_STATUS_INVAL;
    int sent = 0;
    int r = libusb_bulk_transfer(d.handle, d.bulk_out_ep, const_cast<SANE_Byte*>(buffer),
                                 static_cast<int>(*size), &sent, static_cast<unsigned>(g_timeout_ms));
    if (r < 0) {
        ++g_error_count;
        usb_log("ERROR", "bulk write of %zu bytes failed: %s (sent %d bytes)", *size, libusb_error_name(r), sent);
        libusb_clear_halt(d.handle, d.bulk_out_ep);
        *size = 0;
        return SANE_STATUS_IO_ERROR;
    }
    g_bulk_out_bytes += static_cast<unsigned>(sent);
    if (g_trace) usb_log("USB", "bulk out %zu bytes, sent %d", *size, sent);
    *size = static_cast<size_t>(sent);
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_control_msg(SANE_Int dn, SANE_Int rtype, SANE_Int req, SANE_Int value,
                                  SANE_Int index, SANE_Int len, SANE_Byte* data)
{
    if (!valid(dn) || !g_devices[dn].open) {
        usb_log("ERROR", "control transfer on a closed device");
        return SANE_STATUS_INVAL;
    }
    if (len > 0 && !data) {
        usb_log("ERROR", "control transfer of %d bytes with no buffer", len);
        return SANE_STATUS_INVAL;
    }
    ++g_ctrl_count;
    UsbDev& d = g_devices[dn];
    if (d.fake) {
        g_fake_chip.control(rtype, req, value, len, data);
        if (g_trace)
            usb_log("USB", "ctrl %s req=%02x val=%04x idx=%04x len=%d [%s] (fake)", (rtype & 0x80) ? "in " : "out",
                    req, value, index, len, hex_bytes(data, len).c_str());
        return SANE_STATUS_GOOD;
    }
    int r = libusb_control_transfer(d.handle, static_cast<uint8_t>(rtype & 0xff),
                                    static_cast<uint8_t>(req & 0xff),
                                    static_cast<uint16_t>(value & 0xffff),
                                    static_cast<uint16_t>(index & 0xffff), data,
                                    static_cast<uint16_t>(len & 0xffff),
                                    static_cast<unsigned>(g_timeout_ms));
    if (r < 0) {
        ++g_error_count;
        usb_log("ERROR", "control transfer rtype=%02x req=%02x val=%04x idx=%04x len=%d failed: %s",
                rtype, req, value, index, len, libusb_error_name(r));
        return SANE_STATUS_IO_ERROR;
    }
    if (g_trace)
        usb_log("USB", "ctrl %s req=%02x val=%04x idx=%04x len=%d [%s]", (rtype & 0x80) ? "in " : "out", req,
                value, index, len, hex_bytes(data, len).c_str());
    return SANE_STATUS_GOOD;
}

SANE_Status sanei_usb_read_int(SANE_Int dn, SANE_Byte* buffer, size_t* size)
{
    if (!size || !valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    UsbDev& d = g_devices[dn];
    if (d.fake) { *size = 0; return SANE_STATUS_EOF; }
    if (!d.int_in_ep) return SANE_STATUS_INVAL;
    int got = 0;
    int r = libusb_interrupt_transfer(d.handle, d.int_in_ep, buffer, static_cast<int>(*size), &got,
                                      static_cast<unsigned>(g_timeout_ms));
    if (r < 0) { *size = 0; return SANE_STATUS_IO_ERROR; }
    *size = static_cast<size_t>(got);
    return got ? SANE_STATUS_GOOD : SANE_STATUS_EOF;
}

SANE_Status sanei_usb_set_configuration(SANE_Int dn, SANE_Int configuration)
{
    if (!valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    if (g_devices[dn].fake) return SANE_STATUS_GOOD;
    return libusb_set_configuration(g_devices[dn].handle, configuration) == 0 ? SANE_STATUS_GOOD
                                                                              : SANE_STATUS_IO_ERROR;
}

SANE_Status sanei_usb_claim_interface(SANE_Int dn, SANE_Int interface_number)
{
    if (!valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    if (g_devices[dn].fake) return SANE_STATUS_GOOD;
    return libusb_claim_interface(g_devices[dn].handle, interface_number) == 0 ? SANE_STATUS_GOOD
                                                                               : SANE_STATUS_IO_ERROR;
}

SANE_Status sanei_usb_release_interface(SANE_Int dn, SANE_Int interface_number)
{
    if (!valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    if (g_devices[dn].fake) return SANE_STATUS_GOOD;
    return libusb_release_interface(g_devices[dn].handle, interface_number) == 0 ? SANE_STATUS_GOOD
                                                                                 : SANE_STATUS_IO_ERROR;
}

SANE_Status sanei_usb_set_altinterface(SANE_Int dn, SANE_Int alternate)
{
    if (!valid(dn) || !g_devices[dn].open) return SANE_STATUS_INVAL;
    UsbDev& d = g_devices[dn];
    d.alt_setting = alternate;
    if (d.fake) return SANE_STATUS_GOOD;
    return libusb_set_interface_alt_setting(d.handle, d.interface_nr, alternate) == 0
               ? SANE_STATUS_GOOD : SANE_STATUS_IO_ERROR;
}

SANE_Status sanei_usb_get_descriptor(SANE_Int dn, struct sanei_usb_dev_descriptor* desc)
{
    if (!valid(dn)) return SANE_STATUS_INVAL;
    UsbDev& d = g_devices[dn];
    if (d.fake) {
        desc->desc_type = 1;
        desc->bcd_usb = 0x0200;
        desc->bcd_dev = static_cast<unsigned>(d.bcd_device);
        desc->dev_class = 0xff;
        desc->dev_sub_class = 0;
        desc->dev_protocol = 0;
        desc->max_packet_size = 64;
        return SANE_STATUS_GOOD;
    }
    if (!d.dev) return SANE_STATUS_INVAL;
    libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(d.dev, &dd) != 0) return SANE_STATUS_INVAL;
    desc->desc_type = dd.bDescriptorType;
    desc->bcd_usb = dd.bcdUSB;
    desc->bcd_dev = dd.bcdDevice;
    desc->dev_class = dd.bDeviceClass;
    desc->dev_sub_class = dd.bDeviceSubClass;
    desc->dev_protocol = dd.bDeviceProtocol;
    desc->max_packet_size = dd.bMaxPacketSize0;
    return SANE_STATUS_GOOD;
}

} // extern "C"
