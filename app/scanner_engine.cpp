// LiDE50 Scan - scanner engine implementation.
#include "scanner_engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "../core/include/sane/sane.h"
#include "../core/include/sane/saneopts.h"

#include "log.h"
#include <process.h>


extern "C" {
void lide50_enable_simulation(void);
SANE_Status sane_genesys_init(SANE_Int* version_code, SANE_Auth_Callback authorize);
void sane_genesys_exit(void);
SANE_Status sane_genesys_get_devices(const SANE_Device*** device_list, SANE_Bool local_only);
SANE_Status sane_genesys_open(SANE_String_Const devicename, SANE_Handle* handle);
void sane_genesys_close(SANE_Handle handle);
const SANE_Option_Descriptor* sane_genesys_get_option_descriptor(SANE_Handle handle, SANE_Int option);
SANE_Status sane_genesys_control_option(SANE_Handle handle, SANE_Int option, SANE_Action action,
                                        void* value, SANE_Int* info);
SANE_Status sane_genesys_get_parameters(SANE_Handle handle, SANE_Parameters* params);
SANE_Status sane_genesys_start(SANE_Handle handle);
SANE_Status sane_genesys_read(SANE_Handle handle, SANE_Byte* data, SANE_Int max_length, SANE_Int* length);
void sane_genesys_cancel(SANE_Handle handle);
}

namespace {

std::string status_text(SANE_Status s)
{
    switch (s) {
        case SANE_STATUS_GOOD: return "OK";
        case SANE_STATUS_UNSUPPORTED: return "operation not supported";
        case SANE_STATUS_CANCELLED: return "cancelled";
        case SANE_STATUS_DEVICE_BUSY: return "the scanner is busy or in use by another program";
        case SANE_STATUS_INVAL: return "invalid argument";
        case SANE_STATUS_EOF: return "end of data";
        case SANE_STATUS_JAMMED:
            return "the scan head did not move or the sensor saw no light. If your scanner has a lock "
                   "switch underneath, slide it to UNLOCK, close the lid, then unplug and reconnect the scanner";
        case SANE_STATUS_NO_DOCS: return "no document";
        case SANE_STATUS_COVER_OPEN: return "scanner cover is open";
        case SANE_STATUS_IO_ERROR: return "USB communication error (try unplugging and reconnecting the scanner)";
        case SANE_STATUS_NO_MEM: return "out of memory";
        case SANE_STATUS_ACCESS_DENIED:
            return "Windows has not given this app access to the scanner yet. "
                   "Use \"Enable scanner access\" (one-time setup).";
        default: return "unknown error";
    }
}

inline double to_mm(SANE_Word w) { return SANE_UNFIX(w); }

// Synthetic "document" used only in simulation mode, so the UI can be
// exercised without hardware. Coordinates are in millimetres on the bed.
void sim_pixel(double x, double y, std::uint8_t* rgb)
{
    int r = 248, g = 246, b = 240; // paper
    if (x < 6 || x > 210 || y < 6 || y > 291) { r = 236; g = 236; b = 233; } // white lid
    else if (y > 20 && y < 34 && x > 20 && x < 196) {                       // title bar
        r = 30; g = 60; b = 140;
    } else if (y > 44 && y < 104 && x > 20 && x < 196) {                   // color bars
        int bar = static_cast<int>((x - 20) / 22);
        static const int c[8][3] = {{230,40,40},{240,140,30},{240,220,40},{60,170,70},
                                    {40,170,200},{40,70,200},{140,60,180},{30,30,30}};
        bar = std::min(bar, 7);
        r = c[bar][0]; g = c[bar][1]; b = c[bar][2];
    } else if (y > 112 && y < 128 && x > 20 && x < 196) {                  // gray ramp
        int v = static_cast<int>((x - 20) / 176.0 * 255.0);
        r = g = b = v;
    } else if (y > 140 && y < 260 && x > 20 && x < 196) {
        double cx = 150, cy = 200;
        double d = std::hypot(x - cx, y - cy);
        if (d < 34) {                                                       // photo-like disc
            double t = d / 34.0;
            r = static_cast<int>(250 - 120 * t); g = static_cast<int>(180 - 60 * t);
            b = static_cast<int>(90 + 100 * t);
        } else if (x < 112) {                                               // "text" lines
            int line = static_cast<int>((y - 140) / 6.0);
            double ly = std::fmod(y - 140, 6.0);
            double len = 70 + 18 * std::sin(line * 1.7);
            if (ly > 1.5 && ly < 4.0 && x < 24 + len) {
                int word = static_cast<int>((x - 20) * 1.3) % 13;
                if (word < 10) { r = g = b = 25; }
            }
        }
    }
    rgb[0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
    rgb[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
    rgb[2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
}

} // namespace

ScannerEngine::ScannerEngine() = default;

ScannerEngine::~ScannerEngine() { stop(); }

void ScannerEngine::start(const std::wstring& data_dir, bool simulate, bool fake_usb, int driver_log_level,
                          Callback cb)
{
    cb_ = std::move(cb);
    simulate_ = simulate;
    fake_usb_ = fake_usb;

    // The driver keeps its calibration cache in %HOME%\.sane
    std::wstring sane_dir = data_dir + L"\\.sane";
    CreateDirectoryW(data_dir.c_str(), nullptr);
    CreateDirectoryW(sane_dir.c_str(), nullptr);
    wchar_t shortp[MAX_PATH] = {};
    std::wstring use = data_dir;
    if (GetShortPathNameW(data_dir.c_str(), shortp, MAX_PATH) > 0) use = shortp;
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_ACP, 0, use.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
    std::string env = std::string("HOME=") + narrow;
    _putenv(env.c_str());
    LOG_INFO("engine: calibration cache folder %s\\.sane", narrow);

    // driver trace level: 4 = info, 5 = every driver function, 6 = every register access,
    // 7 = every data block
    std::string lvl = "SANE_DEBUG_GENESYS=" + std::to_string(driver_log_level);
    _putenv(lvl.c_str());
    _putenv(fake_usb ? "LIDE50_FAKE_USB=1" : "LIDE50_FAKE_USB=");
    LOG_INFO("engine: driver log level %d%s%s", driver_log_level, simulate ? ", SIMULATION mode" : "",
             fake_usb ? ", FAKE USB DEVICE test mode" : "");

    // A dedicated thread with a large stack runs all driver code.
    thread_ = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, 64u * 1024u * 1024u,
        [](void* self) -> unsigned {
            static_cast<ScannerEngine*>(self)->thread_entry();
            return 0;
        },
        this, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (!thread_) LOG_ERROR("engine: could not start the scanner thread (errno %d)", errno);
}

void ScannerEngine::thread_entry()
{
    prepare_thread_for_crash_logging("scanner engine");
    try {
        thread_main();
    } catch (const std::exception& e) {
        LOG_ERROR("engine: unexpected error ended the scanner thread: %s", e.what());
        log_stack_trace("engine thread stack:");
    } catch (...) {
        LOG_ERROR("engine: unexpected unknown error ended the scanner thread");
    }
    LOG_INFO("engine: scanner thread finished");
}

void ScannerEngine::stop()
{
    if (!thread_) return;
    LOG_INFO("engine: stopping");
    cancel_requested_ = true;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
        queue_.push_back({Cmd::Quit, {}});
    }
    queue_cv_.notify_all();
    if (WaitForSingleObject(thread_, 15000) == WAIT_TIMEOUT)
        LOG_WARN("engine: scanner thread did not stop within 15 s");
    CloseHandle(thread_);
    thread_ = nullptr;
}

void ScannerEngine::connect()
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back({Cmd::Connect, {}});
    queue_cv_.notify_all();
}

void ScannerEngine::disconnect()
{
    connected_ = false; // reflect immediately in the UI
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back({Cmd::Disconnect, {}});
    queue_cv_.notify_all();
}

void ScannerEngine::scan(const ScanRequest& req)
{
    cancel_requested_ = false;
    busy_ = true;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back({Cmd::Scan, req});
    queue_cv_.notify_all();
}

Image ScannerEngine::take_result()
{
    std::lock_guard<std::mutex> lock(result_mutex_);
    Image out = std::move(result_);
    result_ = Image();
    return out;
}

void ScannerEngine::thread_main()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            bool got = queue_cv_.wait_for(lock, std::chrono::milliseconds(250),
                                          [this] { return !queue_.empty(); });
            if (!got) {
                lock.unlock();
                if (connected_ && poll_buttons_ && !simulate_) guarded("button check", [this] { poll_buttons(); });
                continue;
            }
            job = queue_.front();
            queue_.pop_front();
        }
        if (job.cmd == Cmd::Quit) break;
        if (job.cmd == Cmd::Connect) {
            LOG_INFO("engine: connect requested");
            if (!guarded("connect", [this] { do_connect(); }))
                cb_(EngineEvent::ConnectFailed, "Unexpected error while connecting (see LiDE50Scan.log).");
        }
        if (job.cmd == Cmd::Disconnect) {
            LOG_INFO("engine: disconnect requested (scanner unplugged)");
            guarded("disconnect", [this] {
                do_close();
                if (sane_inited_) { sane_genesys_exit(); sane_inited_ = false; }
            });
        }
        if (job.cmd == Cmd::Scan) {
            const ScanRequest& r = job.req;
            LOG_INFO("engine: %s requested: %d dpi, %s, area %.1f,%.1f - %.1f,%.1f mm",
                     r.preview ? "PREVIEW" : "SCAN", r.dpi,
                     r.mode == ColorMode::Color ? "color" : (r.mode == ColorMode::Gray ? "grayscale" : "black & white"),
                     r.tl_x, r.tl_y, r.br_x, r.br_y);
            bool ok = guarded("scan", [this, &job] { do_scan(job.req); });
            if (!ok) {
                {
                    std::lock_guard<std::mutex> lock(progress_.mutex);
                    progress_.active = false;
                }
                if (handle_) guarded("cancel after error", [this] { sane_genesys_cancel(static_cast<SANE_Handle>(handle_)); });
                bool was_parking = parking_.exchange(false);
                busy_ = false;
                if (was_parking) {
                    // the image was already delivered; only the head return failed
                    cb_(EngineEvent::Idle, "");
                } else {
                    cb_(EngineEvent::ScanFailed,
                        "The scan stopped because of an unexpected error. Details are in LiDE50Scan.log.");
                }
            }
            parking_ = false;
            busy_ = false;
        }
    }
    LOG_INFO("engine: shutting down driver");
    guarded("shutdown", [this] {
        do_close();
        if (sane_inited_) { sane_genesys_exit(); sane_inited_ = false; }
    });
}

template <class F>
bool ScannerEngine::guarded(const char* what, F&& fn)
{
    try {
        fn();
        return true;
    } catch (const std::bad_alloc&) {
        LOG_ERROR("engine: out of memory during %s", what);
    } catch (const std::exception& e) {
        LOG_ERROR("engine: error during %s: %s", what, e.what());
    } catch (...) {
        LOG_ERROR("engine: unknown error during %s", what);
    }
    log_stack_trace("stack at the time of the error:");
    return false;
}

void ScannerEngine::do_close()
{
    if (handle_) {
        LOG_INFO("engine: closing scanner (lamp off, head parked, calibration saved)");
        sane_genesys_close(static_cast<SANE_Handle>(handle_));
        handle_ = nullptr;
        LOG_INFO("engine: scanner closed");
    }
    connected_ = false;
}

void ScannerEngine::do_connect()
{
    do_close();
    if (sane_inited_) { sane_genesys_exit(); sane_inited_ = false; }

    if (simulate_) {
        lide50_enable_simulation();
    }

    SANE_Int version = 0;
    LOG_INFO("engine: starting driver (sane_init)");
    SANE_Status st = sane_genesys_init(&version, nullptr);
    LOG_INFO("engine: sane_init -> %s", status_text(st).c_str());
    if (st != SANE_STATUS_GOOD) {
        cb_(EngineEvent::ConnectFailed, "Driver initialisation failed: " + status_text(st));
        return;
    }
    sane_inited_ = true;

    const SANE_Device** list = nullptr;
    st = sane_genesys_get_devices(&list, SANE_TRUE);
    LOG_INFO("engine: sane_get_devices -> %s, %s", status_text(st).c_str(),
             (list && list[0]) ? list[0]->name : "no scanner found");
    if (st != SANE_STATUS_GOOD || !list || !list[0]) {
        cb_(EngineEvent::ConnectFailed, "No Canon LiDE 50 found on USB.");
        return;
    }
    std::string name = list[0]->name;
    SANE_Handle h = nullptr;
    LOG_INFO("engine: opening %s", name.c_str());
    st = sane_genesys_open(name.c_str(), &h);
    LOG_INFO("engine: sane_open -> %s", status_text(st).c_str());
    if (st != SANE_STATUS_GOOD) {
        cb_(EngineEvent::ConnectFailed, "Could not open the scanner: " + status_text(st));
        return;
    }
    handle_ = h;
    connected_ = true;
    button_fail_count_ = 0;
    poll_buttons_ = true;

    // bed size from the geometry ranges
    int brx = find_option(SANE_NAME_SCAN_BR_X);
    int bry = find_option(SANE_NAME_SCAN_BR_Y);
    if (brx > 0) {
        auto* d = sane_genesys_get_option_descriptor(h, brx);
        if (d && d->constraint_type == SANE_CONSTRAINT_RANGE) bed_w_ = to_mm(d->constraint.range->max);
    }
    if (bry > 0) {
        auto* d = sane_genesys_get_option_descriptor(h, bry);
        if (d && d->constraint_type == SANE_CONSTRAINT_RANGE) bed_h_ = to_mm(d->constraint.range->max);
    }
    std::string label = std::string(list[0]->vendor) + " " + list[0]->model;
    if (label.find("LiDE") != std::string::npos) label = "Canon CanoScan LiDE 50";
    if (simulate_) label += " (simulated)";
    if (fake_usb_) label += " (fake USB test device)";
    LOG_INFO("engine: connected to %s, glass %.1f x %.1f mm", label.c_str(), bed_w_, bed_h_);
    log_options();
    cb_(EngineEvent::Connected, label);
}

int ScannerEngine::find_option(const char* name)
{
    if (!handle_) return -1;
    auto h = static_cast<SANE_Handle>(handle_);
    SANE_Int count = 0;
    if (sane_genesys_control_option(h, 0, SANE_ACTION_GET_VALUE, &count, nullptr) != SANE_STATUS_GOOD)
        return -1;
    for (int i = 1; i < count; ++i) {
        auto* d = sane_genesys_get_option_descriptor(h, i);
        if (d && d->name && std::strcmp(d->name, name) == 0) return i;
    }
    return -1;
}

bool ScannerEngine::set_option(const char* name, const void* value)
{
    int idx = find_option(name);
    if (idx < 0) {
        LOG_WARN("engine: option '%s' does not exist", name);
        return false;
    }
    auto h = static_cast<SANE_Handle>(handle_);
    auto* d = sane_genesys_get_option_descriptor(h, idx);
    if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || !SANE_OPTION_IS_SETTABLE(d->cap)) {
        LOG_WARN("engine: option '%s' is not settable right now", name);
        return false;
    }
    std::vector<char> buf(std::max<size_t>(static_cast<size_t>(d->size), 64));
    std::memcpy(buf.data(), value, d->type == SANE_TYPE_STRING
                                       ? std::strlen(static_cast<const char*>(value)) + 1
                                       : sizeof(SANE_Word));
    std::string shown;
    if (d->type == SANE_TYPE_STRING) shown = static_cast<const char*>(value);
    else if (d->type == SANE_TYPE_FIXED) shown = std::to_string(SANE_UNFIX(*static_cast<const SANE_Word*>(value)));
    else shown = std::to_string(*static_cast<const SANE_Word*>(value));
    SANE_Int info = 0;
    SANE_Status st = sane_genesys_control_option(h, idx, SANE_ACTION_SET_VALUE, buf.data(), &info);
    std::string now;
    if (d->type == SANE_TYPE_STRING) now = buf.data();
    else if (d->type == SANE_TYPE_FIXED) now = std::to_string(SANE_UNFIX(*reinterpret_cast<SANE_Word*>(buf.data())));
    else now = std::to_string(*reinterpret_cast<SANE_Word*>(buf.data()));
    if (st == SANE_STATUS_GOOD)
        LOG_INFO("engine: set %s = %s%s", name, shown.c_str(),
                 (info & SANE_INFO_INEXACT) ? (" (scanner adjusted it to " + now + ")").c_str() : "");
    else
        LOG_WARN("engine: setting %s = %s failed: %s", name, shown.c_str(), status_text(st).c_str());
    return st == SANE_STATUS_GOOD;
}

void ScannerEngine::log_options()
{
    auto h = static_cast<SANE_Handle>(handle_);
    SANE_Int count = 0;
    if (sane_genesys_control_option(h, 0, SANE_ACTION_GET_VALUE, &count, nullptr) != SANE_STATUS_GOOD) return;
    LOG_INFO("engine: driver offers %d options", count - 1);
    for (int i = 1; i < count; ++i) {
        auto* d = sane_genesys_get_option_descriptor(h, i);
        if (!d || !d->name || d->type == SANE_TYPE_GROUP) continue;
        std::string c;
        if (d->constraint_type == SANE_CONSTRAINT_WORD_LIST && d->constraint.word_list) {
            for (int k = 1; k <= d->constraint.word_list[0]; ++k)
                c += (k > 1 ? "," : "") + std::to_string(d->constraint.word_list[k]);
        } else if (d->constraint_type == SANE_CONSTRAINT_RANGE && d->constraint.range) {
            char b[96];
            if (d->type == SANE_TYPE_FIXED)
                std::snprintf(b, sizeof(b), "%.2f..%.2f", SANE_UNFIX(d->constraint.range->min), SANE_UNFIX(d->constraint.range->max));
            else
                std::snprintf(b, sizeof(b), "%d..%d", d->constraint.range->min, d->constraint.range->max);
            c = b;
        } else if (d->constraint_type == SANE_CONSTRAINT_STRING_LIST && d->constraint.string_list) {
            for (int k = 0; d->constraint.string_list[k]; ++k) c += (k ? "," : "") + std::string(d->constraint.string_list[k]);
        }
        LOG_INFO("engine:   option %-22s %s%s", d->name, c.c_str(), SANE_OPTION_IS_ACTIVE(d->cap) ? "" : " (inactive)");
    }
}

extern "C" int sanei_debug_genesys; // driver trace level

void ScannerEngine::poll_buttons()
{
    // Button checks run 4 times a second; keep them out of the log unless
    // something goes wrong (errors are still logged).
    struct QuietDriver {
        int saved = sanei_debug_genesys;
        QuietDriver() { if (sanei_debug_genesys > 3) sanei_debug_genesys = 3; }
        ~QuietDriver() { sanei_debug_genesys = saved; }
    } quiet;
    static const char* names[] = {"scan", "copy", "file", "email"};
    auto h = static_cast<SANE_Handle>(handle_);
    for (const char* n : names) {
        int idx = find_option(n);
        if (idx < 0) continue;
        SANE_Bool v = SANE_FALSE;
        SANE_Status st = sane_genesys_control_option(h, idx, SANE_ACTION_GET_VALUE, &v, nullptr);
        if (st != SANE_STATUS_GOOD) {
            LOG_WARN("engine: reading scanner button '%s' failed: %s", n, status_text(st).c_str());
            if (++button_fail_count_ > 3) {
                poll_buttons_ = false;
                LOG_WARN("engine: scanner buttons disabled after repeated read errors");
            }
            return;
        }
        if (v) {
            LOG_INFO("engine: scanner button '%s' pressed", n);
            cb_(EngineEvent::ButtonPressed, n);
            return;
        }
    }
}

void ScannerEngine::do_scan(const ScanRequest& req)
{
    if (!connected_) {
        do_connect();
        if (!connected_) {
            cb_(EngineEvent::ScanFailed, "The scanner is not connected.");
            return;
        }
    }
    auto h = static_cast<SANE_Handle>(handle_);

    // test hook for the crash reporter: LIDE50_CRASH_TEST=1
    if (const char* ct = std::getenv("LIDE50_CRASH_TEST")) {
        if (*ct == '1') {
            LOG_WARN("engine: LIDE50_CRASH_TEST set - crashing on purpose");
            *static_cast<volatile int*>(nullptr) = 1;
        }
    }

    // The LiDE 50 has no hardware black & white mode worth using; scan gray
    // and threshold on the PC so the threshold can be adjusted afterwards.
    const char* mode = req.mode == ColorMode::Color ? SANE_VALUE_SCAN_MODE_COLOR
                                                    : SANE_VALUE_SCAN_MODE_GRAY;
    SANE_Word depth = 8;
    SANE_Word dpi = req.dpi;
    SANE_Word tlx = SANE_FIX(std::clamp(req.tl_x, 0.0, bed_w_));
    SANE_Word tly = SANE_FIX(std::clamp(req.tl_y, 0.0, bed_h_));
    SANE_Word brx = SANE_FIX(std::clamp(req.br_x, req.tl_x + 1.0, bed_w_));
    SANE_Word bry = SANE_FIX(std::clamp(req.br_y, req.tl_y + 1.0, bed_h_));
    SANE_Bool preview = SANE_FALSE; // full-quality preview; resolution is already low

    set_option(SANE_NAME_SCAN_MODE, mode);
    set_option(SANE_NAME_BIT_DEPTH, &depth);
    set_option(SANE_NAME_SCAN_RESOLUTION, &dpi);
    set_option(SANE_NAME_PREVIEW, &preview);
    // order matters: set bottom-right first so top-left is always valid
    set_option(SANE_NAME_SCAN_BR_X, &brx);
    set_option(SANE_NAME_SCAN_BR_Y, &bry);
    set_option(SANE_NAME_SCAN_TL_X, &tlx);
    set_option(SANE_NAME_SCAN_TL_Y, &tly);

    cb_(EngineEvent::Status, req.preview ? "Warming up lamp and calibrating for preview..."
                                         : "Warming up lamp and calibrating...");
    LOG_INFO("engine: starting scan (sane_start: lamp warm-up, calibration, head positioning)");
    auto t_start = std::chrono::steady_clock::now();
    SANE_Status st = sane_genesys_start(h);
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - t).count());
    };
    LOG_INFO("engine: sane_start -> %s after %lld ms", status_text(st).c_str(), ms_since(t_start));
    if (st != SANE_STATUS_GOOD) {
        sane_genesys_cancel(h);
        if (st == SANE_STATUS_IO_ERROR || st == SANE_STATUS_ACCESS_DENIED) do_close();
        busy_ = false;
        cb_(EngineEvent::ScanFailed, "Could not start the scan: " + status_text(st));
        return;
    }
    SANE_Parameters p{};
    SANE_Status pst = sane_genesys_get_parameters(h, &p);
    LOG_INFO("engine: parameters (%s): format %d (%s), %d pixels x %d lines, %d bytes/line, depth %d",
             status_text(pst).c_str(), p.format, p.format == SANE_FRAME_RGB ? "RGB" : (p.format == SANE_FRAME_GRAY ? "gray" : "other"),
             p.pixels_per_line, p.lines, p.bytes_per_line, p.depth);
    if (p.pixels_per_line <= 0 || p.bytes_per_line <= 0 ||
        p.bytes_per_line != p.pixels_per_line * (p.format == SANE_FRAME_RGB ? 3 : 1)) {
        sane_genesys_cancel(h);
        LOG_ERROR("engine: the scanner reported an invalid line size");
        cb_(EngineEvent::ScanFailed, "The scanner reported an invalid image size.");
        return;
    }
    if (p.depth != 8 || (p.format != SANE_FRAME_RGB && p.format != SANE_FRAME_GRAY)) {
        sane_genesys_cancel(h);
        cb_(EngineEvent::ScanFailed, "Unexpected image format from scanner.");
        return;
    }
    const int channels = p.format == SANE_FRAME_RGB ? 3 : 1;
    const int width = p.pixels_per_line;
    const int bpl = p.bytes_per_line;
    int expected = p.lines > 0 ? p.lines : 1000;

    {
        std::lock_guard<std::mutex> lock(progress_.mutex);
        progress_.image = Image();
        progress_.image.alloc(width, expected, channels);
        progress_.image.dpi = req.dpi;
        std::fill(progress_.image.data.begin(), progress_.image.data.end(), 255);
        progress_.lines_done = 0;
        progress_.lines_total = p.lines > 0 ? p.lines : 0;
        progress_.active = true;
        progress_.request = req;
    }
    cb_(EngineEvent::ScanStarted, "");
    cb_(EngineEvent::Status, req.preview ? "Previewing..." : "Scanning...");

    std::vector<std::uint8_t> line(static_cast<size_t>(bpl));
    std::vector<std::uint8_t> chunk(256 * 1024);
    size_t line_fill = 0;
    int lines = 0;
    bool cancelled = false;
    std::string error;
    const double mm_per_px = 25.4 / req.dpi;
    auto sim_last = std::chrono::steady_clock::now();
    int sim_lines_paced = 0;

    auto t_read = std::chrono::steady_clock::now();
    unsigned long long total_bytes = 0;
    int next_report = 10;
    bool delivered = false;   // image handed to the UI before the head finished parking

    // Hands the finished image to the UI.
    auto deliver = [&](const std::string& note) {
        Image out;
        {
            std::lock_guard<std::mutex> lock(progress_.mutex);
            out = progress_.image;
            progress_.active = false;
        }
        out.height = lines;
        out.data.resize(static_cast<size_t>(lines) * out.stride());
        out.dpi = req.dpi;
        const int ow = out.width, oh = out.height;
        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result_ = std::move(out);
        }
        LOG_INFO("engine: scan complete: %d x %d pixels", ow, oh);
        cb_(EngineEvent::ScanFinished, note);
    };

    LOG_INFO("engine: reading image data");
    for (;;) {
        if (cancel_requested_ && !delivered) {
            LOG_INFO("engine: cancel requested by user after %d lines", lines);
            cancelled = true;
            break;
        }
        SANE_Int got = 0;
        st = sane_genesys_read(h, chunk.data(), static_cast<SANE_Int>(chunk.size()), &got);
        if (st == SANE_STATUS_EOF) {
            LOG_INFO("engine: end of image data");
            break;
        }
        if (st != SANE_STATUS_GOOD) {
            error = status_text(st);
            LOG_ERROR("engine: sane_read failed after %d lines: %s", lines, error.c_str());
            break;
        }
        if (got < 0 || got > static_cast<SANE_Int>(chunk.size())) {
            error = "invalid data length from driver";
            LOG_ERROR("engine: driver returned an invalid length %d", got);
            break;
        }
        total_bytes += static_cast<unsigned long long>(got);
        if (p.lines > 0 && lines * 100 / p.lines >= next_report) {
            LOG_INFO("engine: %d%% (%d of %d lines, %.1f MB, %lld ms)", lines * 100 / p.lines, lines, p.lines,
                     total_bytes / 1048576.0, ms_since(t_read));
            while (next_report <= lines * 100 / p.lines) next_report += 10;
        }
        size_t off = 0;
        while (off < static_cast<size_t>(got)) {
            size_t n = std::min(static_cast<size_t>(got) - off, static_cast<size_t>(bpl) - line_fill);
            std::memcpy(line.data() + line_fill, chunk.data() + off, n);
            line_fill += n;
            off += n;
            if (line_fill == static_cast<size_t>(bpl)) {
                line_fill = 0;
                if (simulate_) {
                    // replace the virtual device's blank output with a test pattern
                    double y = req.tl_y + lines * mm_per_px;
                    for (int x = 0; x < width; ++x) {
                        std::uint8_t rgb[3];
                        sim_pixel(req.tl_x + x * mm_per_px, y, rgb);
                        if (channels == 3) std::memcpy(&line[x * 3], rgb, 3);
                        else line[x] = static_cast<std::uint8_t>((rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8);
                    }
                }
                std::lock_guard<std::mutex> lock(progress_.mutex);
                Image& img = progress_.image;
                if (lines >= img.height) {
                    int nh = img.height * 2;
                    img.data.resize(static_cast<size_t>(nh) * img.stride(), 255);
                    img.height = nh;
                }
                std::memcpy(img.row(lines), line.data(), img.stride());
                ++lines;
                progress_.lines_done = lines;
            }
        }
        if (!delivered && p.lines > 0 && lines >= p.lines) {
            // All image lines are in. The driver's next read returns the scan head to the
            // start of the glass (several seconds after a full-length scan) and then reports
            // the end of data, so show the image now and park the head afterwards.
            LOG_INFO("engine: all %d lines received in %lld ms; returning the scan head", lines, ms_since(t_start));
            delivered = true;
            parking_ = true;
            deliver("");
            cb_(EngineEvent::Parking, "Returning the scan head");
            if (simulate_ || fake_usb_) {
                // test hook: LIDE50_TEST_PARK_MS imitates the real head-return time
                if (const char* e = std::getenv("LIDE50_TEST_PARK_MS"))
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(e)));
            }
            continue;
        }
        if (simulate_) {
            // pace the virtual scan (~2 ms per line) so the live view can be tested
            auto target = sim_last + std::chrono::milliseconds(2 * std::max(0, lines - sim_lines_paced));
            std::this_thread::sleep_until(target);
            sim_lines_paced = lines;
            sim_last = std::chrono::steady_clock::now();
        }
    }
    LOG_INFO("engine: finishing scan (sane_cancel: parking head)");
    sane_genesys_cancel(h);
    LOG_INFO("engine: received %d lines, %llu bytes in %lld ms", lines, total_bytes, ms_since(t_start));

    if (delivered) {
        // The image already went to the UI; only the head return was left.
        parking_ = false;
        if (!error.empty()) {
            LOG_WARN("engine: returning the scan head reported an error (the image is complete): %s",
                     error.c_str());
            if (st == SANE_STATUS_IO_ERROR) do_close();
        } else {
            LOG_INFO("engine: scan head parked, ready");
        }
        busy_ = false;
        cb_(EngineEvent::Idle, "");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(progress_.mutex);
        progress_.active = false;
    }
    busy_ = false;
    if (cancelled) {
        cb_(EngineEvent::ScanCancelled, "");
        return;
    }
    if (!error.empty() && lines == 0) {
        if (st == SANE_STATUS_IO_ERROR) do_close();
        cb_(EngineEvent::ScanFailed, "Scan failed: " + error);
        return;
    }
    deliver(error.empty() ? "" : "Scan ended early: " + error);
    cb_(EngineEvent::Idle, "");
}
