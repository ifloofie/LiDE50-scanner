// LiDE50 Scan - scanner engine. Owns all access to the bundled Canon LiDE 50
// driver (SANE genesys/GL841 code running in user space over WinUSB).
// All driver calls happen on one worker thread.
#pragma once

#include "image.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

enum class ColorMode { Color, Gray, BlackWhite };

struct ScanRequest {
    int dpi = 300;
    ColorMode mode = ColorMode::Color;
    double tl_x = 0, tl_y = 0, br_x = 216, br_y = 297; // millimetres
    bool preview = false;
};

// Live state of the scan in progress. Read by the UI under `mutex`.
struct ScanProgress {
    std::mutex mutex;
    Image image;            // grows/fills while scanning
    int lines_done = 0;
    int lines_total = 0;
    bool active = false;
    ScanRequest request;
};

enum class EngineEvent {
    Connected,        // text = model name
    ConnectFailed,    // text = reason
    ScanStarted,
    ScanFinished,     // image ready in take_result()
    ScanFailed,       // text = reason
    ScanCancelled,
    ButtonPressed,    // text = button name (scan/copy/file/email)
    Status,           // text = status message
    Parking,          // image delivered; scan head is returning to the start
    Idle,             // scanner finished all work (head parked); ready for the next scan
};

class ScannerEngine {
public:
    using Callback = std::function<void(EngineEvent, const std::string&)>;

    ScannerEngine();
    ~ScannerEngine();

    // `data_dir` holds the calibration cache. `simulate` runs the full driver
    // code path against a virtual LiDE 50 (for testing without hardware).
    // `fake_usb` runs the real-hardware driver path against a built-in fake
    // GL841 chip (testing only). `driver_log_level` sets the driver trace detail.
    void start(const std::wstring& data_dir, bool simulate, bool fake_usb, int driver_log_level, Callback cb);
    void stop();

    void connect();                        // (re)open the scanner
    void disconnect();                     // scanner was unplugged
    void scan(const ScanRequest& req);     // async; progress in progress()
    void cancel() { cancel_requested_ = true; }
    void set_button_polling(bool on) { poll_buttons_ = on; }

    bool connected() const { return connected_; }
    bool busy() const { return busy_; }
    bool parking() const { return parking_; }
    double bed_width_mm() const { return bed_w_; }
    double bed_height_mm() const { return bed_h_; }
    bool simulated() const { return simulate_; }

    ScanProgress& progress() { return progress_; }
    Image take_result();

private:
    enum class Cmd { Connect, Disconnect, Scan, Quit };
    struct Job { Cmd cmd; ScanRequest req; };

    void thread_entry();
    void thread_main();
    template <class F> bool guarded(const char* what, F&& fn);
    void log_options();
    void do_connect();
    void do_close();
    void do_scan(const ScanRequest& req);
    void poll_buttons();
    bool set_option(const char* name, const void* value);
    int find_option(const char* name);

    Callback cb_;
    void* thread_ = nullptr;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Job> queue_;

    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> parking_{false};
    std::atomic<bool> poll_buttons_{true};
    bool simulate_ = false;
    bool fake_usb_ = false;
    bool sane_inited_ = false;
    void* handle_ = nullptr;
    double bed_w_ = 216.0, bed_h_ = 297.0;
    int button_fail_count_ = 0;

    ScanProgress progress_;
    std::mutex result_mutex_;
    Image result_;
};
