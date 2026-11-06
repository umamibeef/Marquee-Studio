// The Studio's Sign window: the real sign over its DB9 (a USB serial adapter).
//  - Run from RAM: power the sign on, the Studio catches our boot loader's window, stages a whole boot-flash image
//    in the sign's RAM and boots it. Nothing is written to flash or NVRAM; a power cycle brings back the chip's.
//  - Console: the firmware's console (MQ?MON) in a terminal pane.
//  - Remote control: build/sim/sign.sock (a Unix socket) takes one command per connection, runs it here as if its
//    button were pressed (so it shows in this window) and answers with what the window showed for it.
//    tools/signctl.py is the client. Commands: port PATH | image PATH | reset on|off | info | boots | run [IMAGE] |
//    install [IMAGE] | uninstall | retry-slot | clear-crash | slot-stage | slot-direct | console |
//    send LINE [WAIT_S] | disconnect | upload DIR | feed DIR.
//    upload / feed: DIR/upload.txt lists "port PATH", "file NAME" (DIR/files/NAME) and "cmd LINE" lines; the files
//    replace the sign's content store (sbegin, sfile, sraw pieces, scommit), then the lines run in its console.
//    The Studio's Upload apps and its feeds come this way (studio/marquee_studio/sign_window.py); feed does not
//    bring the window to the front. Never the chip write (stage 0): that one stays a button.
#pragma once

#include "signlink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sim {

class SignUI {
public:
    static constexpr const char* NAME = "\xef\x8b\x9b Sign###Sign";      // ICON_FA_MICROCHIP
    // remote: listen on build/sim/sign.sock (not for headless screenshots, and never over another live instance's)
    explicit SignUI(std::string repo_root, bool remote = true);
    ~SignUI();
    void draw();
    bool wants_focus();                                  // a remote command just started: bring this window up
    // for the status bar
    std::string port_label() const;                      // the DB9 adapter's short name, or "none"
    std::string activity();                              // idle, console, the job (with its progress)
    std::string last_remote();                           // the last command from tools/signctl.py, or ""
    bool remote_listening() const { return listen_fd_ >= 0; }
    std::string remote_note() const { return remote_note_; }   // why there is no remote control, if there is none
    void remote_step();                                  // start / finish the queued remote command (UI thread; also
                                                         // called while the window is minimized)

private:
    enum class Job { Run, Install, Uninstall, Boots, Info, SlotStage, SlotDirect, RetrySlot, ClearCrash, TestFault, InstallLoader, UninstallLoader, RetryLoader, WriteChip, Content };
    void content_job(const std::string& port, const std::string& dir);   // the worker of Job::Content
    bool firmware_update(host::SerialPort& p, const host::Bytes8& image);  // Install through our firmware
    struct Remote {                                      // one command from the socket, answered from draw()
        std::string cmd;
        std::string answer;
        bool done = false;
        enum { Queued, Job, Console } state = Queued;
        size_t mark = 0;                                 // console_ size when a line was sent
        std::chrono::steady_clock::time_point until, last_rx;
    };
    void serve(int listen_fd);                           // an accept loop for a listening socket (its own thread)
    std::string remote_start(Remote& r);                 // "" when it is now running, else the answer
    void start_run(Job job = Job::Run);
    void start_console();
    void stop_worker();
    void log(const std::string& s);
    std::string symbolize(uint32_t addr);                // name+0xoff from analysis/symbols.tsv
    void decode_crashes();                               // a crash report in the console gets function names

    std::string repo_;
    std::vector<std::string> ports_;
    std::string port_;
    char image_[1024] = {};
    char input_[256] = {};
    bool reset_first_ = true;                            // restart the sign from its console, no power cycle

    std::thread worker_;
    std::atomic<bool> busy_{false}, cancel_{false}, console_on_{false};
    std::mutex mu_;
    std::vector<std::string> log_;                       // the run's messages
    std::string console_;                                // console output
    std::vector<std::string> to_send_;                   // console lines typed, for the worker
    float progress_ = -1;                                // 0..1 while staging
    std::vector<std::pair<uint32_t, std::string>> syms_; // functions, sorted by address
    size_t crash_scan_ = 0;                              // console_ decoded up to here

    std::thread server_;
    std::thread server_tcp_;
    int listen_fd_ = -1;
    int listen_tcp_ = -1;      // optional TCP control (UMAMI_SIGN_TCP), e.g. a Studio in a container via host.docker.internal
    std::string sock_path_;
    std::string remote_note_;
    std::deque<std::shared_ptr<Remote>> remotes_;        // guarded by mu_
    std::shared_ptr<Remote> active_;                     // draw() thread only
    std::condition_variable remote_cv_;
    bool focus_ = false;
    enum Tab { TabNone = -1, TabFirmware, TabContent, TabDebug, TabLoader, TabConsole };
    char content_dir_[1024] = {};                        // the last upload folder (Content tab)
    int select_tab_ = TabNone;                           // a remote command shows its tab
    const char* job_name_ = "";                          // the running job, for the status bar
    std::string last_remote_;
    void draw_log();                                     // the job's progress and messages
};

}  // namespace sim
