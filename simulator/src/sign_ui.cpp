#include "sign_ui.h"

#include "serial.h"
#include "dbgrecord.h"

#include "imgui.h"
#include "ui_style.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace sim {

SignUI::SignUI(std::string repo_root, bool remote) : repo_(std::move(repo_root)) {
    std::string img = repo_ + "/bootloader/build-dump/bootflash-fw.bin";
    snprintf(image_, sizeof image_, "%s", img.c_str());
    ports_ = host::serial_ports();
    // the remote-control socket (see sign_ui.h)
    sock_path_ = repo_ + "/build/sim/sign.sock";
    if (!remote) {
        remote_note_ = "off (headless)";
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(repo_ + "/build/sim", ec);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (sock_path_.size() < sizeof a.sun_path) {
        memcpy(a.sun_path, sock_path_.c_str(), sock_path_.size() + 1);
        int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);       // another umami-marquee-sim answering: leave it be
        bool live = probe >= 0 && ::connect(probe, (sockaddr*)&a, sizeof a) == 0;
        if (probe >= 0)
            ::close(probe);
        if (live) {
            remote_note_ = "another umami-marquee-sim has it";
            return;
        }
        ::unlink(sock_path_.c_str());                        // a stale one, left by a crash
        memcpy(a.sun_path, sock_path_.c_str(), sock_path_.size() + 1);
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ >= 0 && (::bind(listen_fd_, (sockaddr*)&a, sizeof a) != 0 || ::listen(listen_fd_, 4) != 0)) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }
    if (listen_fd_ >= 0)
        server_ = std::thread([this] { serve(listen_fd_); });

    // Optional TCP control, UMAMI_SIGN_TCP="PORT" or "ADDR:PORT" (default ADDR 0.0.0.0). Same command protocol as the
    // Unix socket. For a Studio in a container to reach this host sign window (host.docker.internal); bound 0.0.0.0 is
    // reachable from the Docker VM (127.0.0.1 is not). Exposes sign control to the host's network: dev use only.
    if (remote) {
        if (const char* e = getenv("UMAMI_SIGN_TCP")) {
            std::string spec = e, addr = "0.0.0.0";
            auto colon = spec.rfind(':');
            if (colon != std::string::npos) { addr = spec.substr(0, colon); spec = spec.substr(colon + 1); }
            int port = atoi(spec.c_str());
            sockaddr_in ta{};
            ta.sin_family = AF_INET;
            ta.sin_port = htons((uint16_t)port);
            ta.sin_addr.s_addr = inet_addr(addr.c_str());
            listen_tcp_ = ::socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            if (listen_tcp_ >= 0)
                ::setsockopt(listen_tcp_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            if (listen_tcp_ >= 0 && (::bind(listen_tcp_, (sockaddr*)&ta, sizeof ta) != 0 || ::listen(listen_tcp_, 4) != 0)) {
                ::close(listen_tcp_);
                listen_tcp_ = -1;
            }
            if (listen_tcp_ >= 0)
                server_tcp_ = std::thread([this] { serve(listen_tcp_); });
            remote_note_ += listen_tcp_ >= 0 ? " + tcp " + addr + ":" + std::to_string(port)
                                             : " (tcp " + std::string(e) + " failed)";
        }
    }
}

SignUI::~SignUI() {
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (listen_tcp_ >= 0) {
        ::shutdown(listen_tcp_, SHUT_RDWR);
        ::close(listen_tcp_);
        listen_tcp_ = -1;
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& r : remotes_)
            r->done = true, r->answer = "the simulator is closing";
        if (active_)
            active_->done = true, active_->answer = "the simulator is closing";
    }
    remote_cv_.notify_all();
    if (server_.joinable()) {
        server_.join();
        ::unlink(sock_path_.c_str());                    // only the instance that listened removes it
    }
    if (server_tcp_.joinable())
        server_tcp_.join();
    stop_worker();
}

bool SignUI::wants_focus() {
    bool f = focus_;
    focus_ = false;
    return f;
}

void SignUI::serve(int listen_fd) {
    for (;;) {
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0)
            return;                                      // closed: the simulator is going away
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   // a client gone: an error, not SIGPIPE
        std::string line;
        char c;
        while (line.size() < 4096 && ::read(fd, &c, 1) == 1 && c != '\n')
            if (c != '\r')
                line += c;
        if (line.find_first_not_of(' ') == std::string::npos) {   // another instance checking that we listen
            ::close(fd);
            continue;
        }
        auto r = std::make_shared<Remote>();
        r->cmd = line;
        {
            std::unique_lock<std::mutex> g(mu_);
            remotes_.push_back(r);
            remote_cv_.wait(g, [&] { return r->done; });
        }
        std::string out = r->answer;
        if (out.empty() || out.back() != '\n')
            out += '\n';
        for (size_t o = 0; o < out.size();) {
            ssize_t n = ::write(fd, out.data() + o, out.size() - o);
            if (n <= 0)
                break;
            o += (size_t)n;
        }
        ::close(fd);
    }
}

std::string SignUI::remote_start(Remote& r) {
    std::istringstream in(r.cmd);
    std::string verb, arg;
    in >> verb;
    std::getline(in, arg);
    arg.erase(0, arg.find_first_not_of(' '));
    auto busy = [&] { return busy_ && !console_on_; };
    if (verb == "status")
        return "port " + (port_.empty() ? std::string("(none)") : port_) + "\nimage " + image_ + "\nreset " +
               (reset_first_ ? "on" : "off") + "\n" + (console_on_ ? "console connected" : busy_ ? "busy" : "idle");
    if (verb == "port") {
        port_ = arg;
        return "port " + port_;
    }
    if (verb == "image") {
        snprintf(image_, sizeof image_, "%s", arg.c_str());
        return std::string("image ") + image_;
    }
    if (verb == "reset") {
        reset_first_ = arg != "off";
        return std::string("reset ") + (reset_first_ ? "on" : "off");
    }
    if (verb == "rs485") {                               // one RS-485 frame (the studio's "Send to sign"), through this host
        std::istringstream a2(arg);
        std::string rport, hex;
        int baud = 19200;
        a2 >> rport >> baud >> hex;
        host::Bytes data;
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
            data.push_back((uint8_t)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        if (rport.empty() || data.empty())
            return "error: rs485 PORT BAUD HEX";
        try {
            host::serial_write(rport, baud, data);
        } catch (const std::exception& e) {
            return std::string("error: ") + e.what();
        }
        std::lock_guard<std::mutex> g(mu_);
        log_.insert(log_.begin(), "[Studio] RS-485: " + std::to_string(data.size()) + " bytes on " + rport);
        return "sent " + std::to_string(data.size()) + " bytes on " + rport;
    }
    if (verb == "ports") {                               // ports a Studio (e.g. in a container) can pick; this host opens them
        std::string out;
        std::error_code ec;
        for (const char* n : {"rs485", "db9"}) {          // the emulated sign's ports, if this sim is running them
            std::string pp = repo_ + "/build/sim/" + n;
            if (std::filesystem::exists(pp, ec))
                out += pp + "\n";
        }
        for (auto& pp : host::serial_ports())             // real USB serial adapters on this host
            out += pp + "\n";
        return out.empty() ? std::string("(none)") : out;
    }
    // never a port chosen by itself: a remote command acts only on the port someone set (the combo box or "port")
    if (port_.empty() && verb != "upload" && verb != "feed")
        return "error: no DB9 port set (use: port PATH)";
    if (verb == "disconnect") {
        console_on_ = false;
        return "console disconnected";
    }
    if (verb == "console" || verb.rfind("send", 0) == 0) {
        double wait = 8;
        if (verb.size() > 5 && verb[4] == ':')
            wait = std::stod(verb.substr(5));
        else if (verb != "send" && verb != "console")
            return "error: unknown command " + verb;
        if (busy())
            return "error: busy with a job";
        bool fresh = !console_on_;
        if (fresh)
            start_console();
        select_tab_ = TabConsole;
        std::lock_guard<std::mutex> g(mu_);
        r.mark = console_.size();
        if (verb != "console") {
            to_send_.push_back(arg);
            console_ += "";                               // (the echo shows what was sent)
        }
        r.state = Remote::Console;
        r.until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(wait * 1000));
        r.last_rx = std::chrono::steady_clock::now();
        return "";
    }
    if (verb == "upload" || verb == "feed") {
        if (busy())
            return "error: busy with a job";
        // the folder may be named by a container (paths.REPO=/app); resolve its basename under this host's uploads dir
        std::error_code ec;
        if (!std::filesystem::exists(arg + "/upload.txt", ec)) {
            std::string alt = repo_ + "/build/sim/uploads/" + std::filesystem::path(arg).filename().string();
            if (std::filesystem::exists(alt + "/upload.txt", ec))
                arg = alt;
        }
        std::ifstream list(arg + "/upload.txt");
        if (!list)
            return "error: no " + arg + "/upload.txt";
        for (std::string line; std::getline(list, line);)
            if (line.rfind("port ", 0) == 0)
                port_ = line.substr(5);
        snprintf(content_dir_, sizeof content_dir_, "%s", arg.c_str());
        start_run(Job::Content);
        {
            std::lock_guard<std::mutex> g(mu_);
            log_.insert(log_.begin(), "[Studio] " + verb + " " + arg);
        }
        if (verb == "upload")
            select_tab_ = TabContent;
        r.state = Remote::Job;
        return "";
    }
    static const std::pair<const char*, Job> jobs[] = {
        {"info", Job::Info}, {"boots", Job::Boots}, {"run", Job::Run}, {"install", Job::Install},
        {"uninstall", Job::Uninstall}, {"retry-slot", Job::RetrySlot}, {"clear-crash", Job::ClearCrash},
        {"slot-stage", Job::SlotStage}, {"slot-direct", Job::SlotDirect}};
    for (auto& [name, job] : jobs)
        if (verb == name) {
            if (busy())
                return "error: busy with a job";
            if ((job == Job::Run || job == Job::Install) && !arg.empty())
                snprintf(image_, sizeof image_, "%s", arg.c_str());
            start_run(job);
            select_tab_ = job == Job::Run || job == Job::Install || job == Job::Uninstall ? TabFirmware : TabDebug;
            {
                std::lock_guard<std::mutex> g(mu_);
                log_.insert(log_.begin(), "[Claude] " + r.cmd);
            }
            r.state = Remote::Job;
            return "";
        }
    return "error: unknown command " + verb + " (status port image reset info boots run install uninstall retry-slot "
           "clear-crash slot-stage slot-direct console send send:SECONDS disconnect upload feed ports rs485)";
}

void SignUI::remote_step() {
    std::shared_ptr<Remote> finished;
    if (active_) {
        auto now = std::chrono::steady_clock::now();
        if (active_->state == Remote::Job && !busy_) {
            std::lock_guard<std::mutex> g(mu_);
            std::string out;
            for (auto& l : log_)
                out += l + "\n";
            active_->answer = out;
            finished = active_;
        } else if (active_->state == Remote::Console) {
            std::lock_guard<std::mutex> g(mu_);
            size_t mark = std::min(active_->mark, console_.size());
            std::string got = console_.substr(mark);
            static size_t seen = 0;
            if (got.size() != seen)
                seen = got.size(), active_->last_rx = now;
            bool prompt = got.size() >= 4 && got.compare(got.size() - 4, 4, "mq> ") == 0;
            bool quiet = now - active_->last_rx > std::chrono::milliseconds(300);
            if ((prompt && quiet) || now > active_->until || !console_on_) {
                active_->answer = got.empty() ? (console_on_ ? "(no reply)" : "(console closed)") : got;
                finished = active_;
                seen = 0;
            }
        }
        if (finished)
            active_.reset();
    } else {
        std::shared_ptr<Remote> r;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!remotes_.empty()) {
                r = remotes_.front();
                remotes_.pop_front();
            }
        }
        if (r) {
            focus_ = r->cmd.rfind("feed ", 0) != 0;     // a feed refresh shows in the log, without jumping up
            last_remote_ = r->cmd;
            std::string a = remote_start(*r);
            if (a.empty())
                active_ = r;
            else {
                r->answer = a;
                finished = r;
            }
        }
    }
    if (finished) {
        {
            std::lock_guard<std::mutex> g(mu_);
            finished->done = true;
        }
        remote_cv_.notify_all();
    }
}

void SignUI::stop_worker() {
    cancel_ = true;
    console_on_ = false;
    if (worker_.joinable())
        worker_.join();
    cancel_ = false;
}

void SignUI::log(const std::string& s) {
    std::lock_guard<std::mutex> g(mu_);
    log_.push_back(s);
    if (log_.size() > 200)
        log_.erase(log_.begin());
}

static const char* job_label(int j) {
    static const char* names[] = {"run from RAM", "install", "uninstall", "boot history", "board info", "slot: stage",
                                  "slot: direct", "retry slot", "clear crash", "test fault", "install stage 1",
                                  "remove stage 1", "retry stage 1", "write chip", "content"};
    return j >= 0 && j < (int)(sizeof names / sizeof *names) ? names[j] : "working";
}

void SignUI::start_run(Job job) {
    stop_worker();
    job_name_ = job_label((int)job);
    {
        std::lock_guard<std::mutex> g(mu_);
        log_.clear();
        progress_ = -1;
    }
    busy_ = true;
    std::string port = port_, path = image_;
    bool reset_first = reset_first_;
    std::string cdir = content_dir_;
    worker_ = std::thread([this, port, path, reset_first, job, cdir]() mutable {
        if (job == Job::Content) {
            try {
                content_job(port, cdir);
            } catch (const std::exception& e) {
                log(std::string("Stopped: ") + e.what());
            }
            {
                std::lock_guard<std::mutex> g(mu_);
                progress_ = -1;
            }
            busy_ = false;
            return;
        }
        try {
            bool debug = job != Job::Run && job != Job::Install && job != Job::Uninstall && job != Job::InstallLoader
                         && job != Job::WriteChip;
            bool recovery = job == Job::UninstallLoader || job == Job::RetryLoader;   // the chip's own window
            if (job == Job::InstallLoader)
                path = repo_ + "/bootloader/build-dump-s1/stage1.img";
            std::ifstream f(path, std::ios::binary);
            if (!f && !debug)
                throw host::SignError("cannot read " + path);
            host::Bytes8 image;
            if (!debug)
                image.assign(std::istreambuf_iterator<char>(f), {});
            host::SerialPort p(port);
            if (job == Job::Install && reset_first && firmware_update(p, image))
                throw host::SignError("done");               // our running firmware took it, on the panel
            host::SignWindow w(p);
            if (reset_first && host::console_reset(p))
                log("The sign is restarting (its console's reset); catching the boot loader's window...");
            else
                log(std::string(reset_first ? "No answer from the sign's console. " : "") +
                    "Waiting for the sign: power it on now (up to 2 minutes).");
            log(w.wait_window(120, &cancel_, recovery));
            if (job == Job::WriteChip) {
                std::string info = w.hello();
                if (info.find(" s=1") == std::string::npos)
                    throw host::SignError("the sign is running its own boot loader (stage 0), which cannot write "
                                            "the chip it runs from. Install and boot a stage 1 first.");
                log("Writing the chip (stage 0) from " + path + ". Do NOT power the sign off until this finishes; "
                    "an interrupted write needs the programmer to recover.");
                w.install_bootflash(image);
                log("WROTE the chip: it now holds this boot loader. A power cycle starts it.");
                w.boot();
                throw host::SignError("done");
            }
            if (job == Job::InstallLoader) {
                w.hello();
                log("Installing the stage-1 boot loader (" + path + ") in the main flash's loader sector...");
                w.install_loader(image);
                log("INSTALLED: the sign's boot loader runs it from the next start on (back to the chip's own "
                    "after 2 failed tries; Recovery reaches the chip's own window at any time).");
                w.boot();
                throw host::SignError("done");
            }
            if (job == Job::Info) {                          // the boot loader's board info, then boot
                std::string info = w.hello();
                for (size_t a = 0, b; a < info.size() && (b = info.find('\n', a)) != std::string::npos; a = b + 1)
                    log("  " + info.substr(a, b - a));
                w.boot();
                throw host::SignError("done");
            }
            if (debug) {                                     // the boot loader's debugging switches (OPTION)
                w.hello();
                using W = host::SignWindow;
                auto rec = [&] { return host::dbg_decode(w.read(W::R_NVRAM, host::DBG_OFFSET, host::DBG_SIZE)); };
                if (job == Job::SlotStage || job == Job::SlotDirect) {
                    uint8_t o = rec().options;
                    o = job == Job::SlotDirect ? (o | host::DBG_OPT_SLOT_DIRECT) : (o & ~host::DBG_OPT_SLOT_DIRECT);
                    w.option(W::OPT_SET_OPTIONS, o);
                    log(job == Job::SlotDirect ? "Slot boot: direct, in place (for debugging)."
                                               : "Slot boot: through the RAM stage (normal).");
                } else if (job == Job::RetrySlot) {
                    w.option(W::OPT_SET_TRIES, 0);
                    log("Try counter reset: the next boot tries the installed image again.");
                } else if (job == Job::UninstallLoader) {
                    w.uninstall_loader();
                    log("Stage 1 removed: the chip's boot loader does everything from the next start on.");
                } else if (job == Job::RetryLoader) {
                    w.option(W::OPT_SET_S1_TRIES, 0);
                    log("Stage-1 try counter reset: the next start runs stage 1 again.");
                } else if (job == Job::ClearCrash) {
                    w.option(W::OPT_CLEAR_CRASH);
                    log("Crash record cleared.");
                } else if (job == Job::TestFault) {
                    log("Making the boot loader fault on purpose; the sign resets itself in about 3 s. Then use "
                        "Boot history: it should show a boot loader exception, vector 0x700.");
                    try {
                        w.option(W::OPT_TEST_FAULT);
                    } catch (const host::SignError&) {
                    }
                    throw host::SignError("done");
                }
                std::string text = host::dbg_report(rec(), [this](uint32_t a) { return symbolize(a); });
                for (size_t a = 0, b; a < text.size() && (b = text.find('\n', a)) != std::string::npos; a = b + 1)
                    log(text.substr(a, b - a));
                w.boot();
                throw host::SignError("done");
            }
            if (job == Job::Uninstall) {
                w.hello();
                w.uninstall();
                log("UNINSTALLED: the sign boots the boot flash's firmware from now on.");
                w.boot();
                throw host::SignError("done");
            }
            std::string info = w.hello();
            for (size_t a = 0, b; a < info.size() && (b = info.find('\n', a)) != std::string::npos; a = b + 1)
                if (info.compare(a, 6, "sysclk") == 0 || info.compare(a, 2, "id") == 0)
                    log("  " + info.substr(a, b - a));
            log("Staging the image in the sign's RAM...");
            auto t0 = std::chrono::steady_clock::now();
            uint32_t n = w.stage(image, [this](uint32_t done, uint32_t total) {
                std::lock_guard<std::mutex> g(mu_);
                progress_ = (float)done / (float)total;
            }, &cancel_);
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            log("Staged " + std::to_string(n) + " bytes in " + std::to_string((int)s) + " s; every block checked.");
            if (job == Job::Install) {
                log("Writing it into the sign's update slot (main flash: erase, program, verify)...");
                w.install(image, n);
                log("INSTALLED: the sign boots this firmware from now on. If it ever fails to start, the sign falls "
                    "back to the boot flash's firmware by itself.");
                w.boot();
            } else {
                w.run(image, n);
                log("RUN: the sign is booting the image from RAM. A power cycle brings back the installed firmware.");
            }
        } catch (const std::exception& e) {
            if (std::string(e.what()) != "done")
                log(std::string("Stopped: ") + e.what());
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            progress_ = -1;
        }
        busy_ = false;
    });
}

void SignUI::content_job(const std::string& port, const std::string& dir) {
    std::ifstream list(dir + "/upload.txt");
    if (!list)
        throw host::SignError("no " + dir + "/upload.txt");
    std::vector<std::pair<std::string, host::Bytes8>> files;
    std::vector<std::string> cmds;
    size_t total = 0;
    for (std::string line; std::getline(list, line);) {
        if (line.rfind("file ", 0) == 0) {
            std::string name = line.substr(5);
            std::ifstream f(dir + "/files/" + name, std::ios::binary);
            if (!f)
                throw host::SignError("cannot read " + name);
            host::Bytes8 b(std::istreambuf_iterator<char>(f), {});
            total += b.size();
            files.emplace_back(name, std::move(b));
        } else if (line.rfind("cmd ", 0) == 0)
            cmds.push_back(line.substr(4));
    }
    auto echo = [this](const std::string& sent, const std::string& reply) {   // the Console tab shows it too
        std::lock_guard<std::mutex> g(mu_);
        console_ += sent + "\n";
        for (auto& l : host::console_lines(reply, sent))
            console_ += l + "\n";
    };
    auto last = [](const std::string& reply, const std::string& sent) {
        auto ls = host::console_lines(reply, sent);
        return ls.empty() ? std::string() : ls.back();
    };
    host::SerialPort p(port);
    log("Opening the sign's console on " + port + "...");
    std::string hello = host::console_open(p, 5);
    if (hello.find("mq> ") == std::string::npos)
        throw host::SignError("no console answered: is our firmware running on the sign?");
    if (!files.empty()) {
        char b[96];
        snprintf(b, sizeof b, "New content store: %zu files, %zu bytes.", files.size(), total);
        log(b);
        echo("sbegin", host::console_command(p, "sbegin", 10));
        size_t done = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (auto& [name, data] : files) {
            snprintf(b, sizeof b, "sfile %s %zx", name.c_str(), data.size());
            std::string r = host::console_command(p, b, 10);
            echo(b, r);
            if (last(r, b) != "ok")
                throw host::SignError("the sign refused " + name + ": " + last(r, b));
            for (size_t k = 0; k < data.size(); k += 256) {          // pieces the sign acknowledges
                host::Bytes8 piece(data.begin() + k, data.begin() + std::min(data.size(), k + 256));
                std::string pr = host::console_raw(p, piece, 10);
                if (last(pr, "") != "ok")
                    throw host::SignError("the sign refused a piece of " + name + " at " + std::to_string(k) + ": " +
                                          last(pr, ""));
                done += piece.size();
                std::lock_guard<std::mutex> g(mu_);
                progress_ = (float)done / (float)total;
                if (cancel_)
                    throw host::SignError("cancelled (the store is unchanged)");
            }
            snprintf(b, sizeof b, "  %s: %zu bytes", name.c_str(), data.size());
            log(b);
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        log("Sent in " + std::to_string((int)s) + " s; writing the store into the main flash...");
        std::string r = host::console_command(p, "scommit", 120);
        echo("scommit", r);
        if (last(r, "scommit") != "committed")
            throw host::SignError("the store was not written: " + last(r, "scommit"));
        log("COMMITTED: the sign uses the new content store.");
    }
    for (auto& c : cmds) {
        std::string r = host::console_command(p, c, 10);
        echo(c, r);
        auto ls = host::console_lines(r, c);
        if (c.rfind("adata", 0) == 0 && (ls.empty() || ls.back() != "ok"))
            throw host::SignError("the sign refused " + c.substr(0, 40) + ": " + (ls.empty() ? "" : ls.back()));
        log("> " + c);
        for (auto& l : ls)
            log("  | " + l);
    }
    p.write({'e', 'x', 'i', 't', '\r'});                // give the port back to the download server
}

// Install through our running firmware (firmware/ext/update.c): the sign shows the update on its panel while the
// image goes in, writes its update slot as the boot loader's INSTALL does, and restarts into it. False when no
// console answered (no firmware of ours running): the caller then uses the boot loader's window.
bool SignUI::firmware_update(host::SerialPort& p, const host::Bytes8& image) {
    std::string hello = host::console_open(p, 3);
    if (hello.find("mq> ") == std::string::npos)
        return false;
    auto last = [](const std::string& reply, const std::string& sent) {
        auto ls = host::console_lines(reply, sent);
        return ls.empty() ? std::string() : ls.back();
    };
    char b[64];
    snprintf(b, sizeof b, "ubegin %zx %x", image.size(), host::crc32_bytes(image.data(), image.size()));
    std::string r = host::console_command(p, b, 10);
    if (last(r, b) != "ok") {
        if (last(r, b).find("unknown command") != std::string::npos) {
            log("The sign's firmware cannot update itself yet: installing through the boot loader's window.");
            return false;
        }
        throw host::SignError("the sign refused the update: " + last(r, b));
    }
    log("The sign shows the update on its panel. Sending the image to its firmware...");
    auto t0 = std::chrono::steady_clock::now();
    for (size_t k = 0; k < image.size(); k += 256) {
        host::Bytes8 piece(image.begin() + k, image.begin() + std::min(image.size(), k + 256));
        std::string pr = host::console_raw(p, piece, 10, "uraw");
        if (last(pr, "") != "ok") {
            host::console_command(p, "uabort", 5);
            throw host::SignError("the sign refused a piece at " + std::to_string(k) + ": " + last(pr, ""));
        }
        std::lock_guard<std::mutex> g(mu_);
        progress_ = (float)(k + piece.size()) / (float)image.size();
        if (cancel_) {
            host::console_command(p, "uabort", 5);
            throw host::SignError("cancelled (nothing written)");
        }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    log("Sent " + std::to_string(image.size()) + " bytes in " + std::to_string((int)s) +
        " s. The sign checks it, writes its update slot and reads it back...");
    {
        std::lock_guard<std::mutex> g(mu_);
        progress_ = -1;
    }
    r = host::console_command(p, "ucommit", 240);
    std::string res = last(r, "ucommit");
    if (r.find("updated") == std::string::npos)
        throw host::SignError("the update was not written: " + res);
    log("INSTALLED: the sign wrote its update slot and is restarting into the new firmware. If it ever fails to "
        "start, the sign falls back to the boot flash's firmware by itself.");
    return true;
}

void SignUI::start_console() {
    stop_worker();
    console_on_ = true;
    busy_ = true;
    std::string port = port_;
    worker_ = std::thread([this, port] {
        try {
            host::SerialPort p(port);
            p.write({'M', 'Q', '?', 'M', 'O', 'N', '\r'});
            while (console_on_) {
                std::vector<std::string> lines;
                {
                    std::lock_guard<std::mutex> g(mu_);
                    lines.swap(to_send_);
                }
                for (auto& l : lines) {
                    host::Bytes8 b(l.begin(), l.end());
                    b.push_back('\r');
                    p.write(b);
                }
                host::Bytes8 in = p.read(50);
                if (!in.empty()) {
                    std::lock_guard<std::mutex> g(mu_);
                    for (uint8_t c : in)
                        if (c >= 0x20 || c == '\n' || c == '\t')   // no NULs or other control bytes: they would
                            console_ += (char)c;                   // end or garble the shown text
                    decode_crashes();
                    if (console_.size() > 200000) {
                        console_.erase(0, console_.size() - 150000);
                        crash_scan_ = 0;
                    }
                }
            }
            p.write({'e', 'x', 'i', 't', '\r'});         // give the port back to the download server
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> g(mu_);
            console_ += std::string("\n[") + e.what() + "]\n";
        }
        console_on_ = false;
        busy_ = false;
    });
}

std::string SignUI::symbolize(uint32_t addr) {
    if (syms_.empty()) {
        std::ifstream f(repo_ + "/analysis/symbols.tsv");
        std::string line;
        while (std::getline(f, line)) {
            size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            size_t c = b == std::string::npos ? b : line.find('\t', b + 1);
            if (c == std::string::npos || line.compare(a + 1, b - a - 1, "func") != 0)
                continue;
            syms_.emplace_back((uint32_t)std::stoul(line.substr(0, a), nullptr, 16), line.substr(b + 1, c - b - 1));
        }
        std::sort(syms_.begin(), syms_.end());
    }
    auto it = std::upper_bound(syms_.begin(), syms_.end(), std::make_pair(addr, std::string("\x7f")));
    if (it == syms_.begin())
        return "?";
    --it;
    uint32_t off = addr - it->first;
    if (off > 0x10000)
        return "?";
    char b[32];
    snprintf(b, sizeof b, "+0x%x", off);
    return it->second + b;
}

void SignUI::decode_crashes() {
    // called with mu_ held: every complete MQCRASH block after crash_scan_ gets a decoded list appended
    for (;;) {
        size_t k = console_.find("MQCRASH 1", crash_scan_);
        size_t e = k == std::string::npos ? k : console_.find("MQCRASH END", k);
        if (e == std::string::npos)
            return;
        std::string block = console_.substr(k, e - k), dec = "\n-- crash decoded (analysis/symbols.tsv) --\n";
        for (const char* key : {" pc ", " lr ", "\nsp "}) {
            for (size_t p = block.find(key); p != std::string::npos; p = block.find(key, p + 1)) {
                size_t v = p + strlen(key);
                if (key[1] == 's') {                     // a stack line: its lr
                    v = block.find(" lr ", p);
                    if (v == std::string::npos)
                        continue;
                    v += 4;
                }
                if (v + 8 > block.size())
                    continue;
                uint32_t a = (uint32_t)std::stoul(block.substr(v, 8), nullptr, 16);
                std::string label = key[1] == 'p' ? "pc" : key[1] == 'l' ? "lr" : "  from";
                char buf[32];
                snprintf(buf, sizeof buf, "%08x", a);
                dec += label + " " + buf + " = " + symbolize(a) + "\n";
            }
        }
        size_t at = e + 11;
        console_.insert(at, dec);
        crash_scan_ = at + dec.size();
    }
}

std::string SignUI::port_label() const {
    if (port_.empty())
        return "none";
    size_t k = port_.rfind('/');
    std::string n = k == std::string::npos ? port_ : port_.substr(k + 1);
    if (n.rfind("cu.", 0) == 0)
        n = n.substr(3);
    return n;
}

std::string SignUI::activity() {
    if (console_on_)
        return "console";
    if (!busy_)
        return "idle";
    std::lock_guard<std::mutex> g(mu_);
    std::string s = *job_name_ ? job_name_ : "working";
    if (progress_ >= 0) {
        char b[16];
        snprintf(b, sizeof b, " %d%%", (int)(progress_ * 100));
        s += b;
    }
    return s;
}

std::string SignUI::last_remote() {
    return last_remote_;
}

static void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SignUI::draw_log() {
    std::lock_guard<std::mutex> g(mu_);
    if (progress_ >= 0)
        ImGui::ProgressBar(progress_, ImVec2(-1, 0));
    ImGui::BeginChild("log", ImVec2(-1, -1), ImGuiChildFlags_Borders);
    ui_mono();                                             // serial text: fixed width, so its columns line up
    for (auto& l : log_)
        ImGui::TextWrapped("%s", l.c_str());
    ui_pop();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void SignUI::draw() {
    remote_step();
    if (!ImGui::Begin(NAME)) {
        ImGui::End();
        return;
    }
    // the connection, always on top
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("DB9 port (?)").x - ImGui::GetStyle().ItemSpacing.x * 3);
    if (ImGui::BeginCombo("DB9 port", port_.empty() ? "Choose the USB serial adapter" : port_.c_str())) {
        ports_ = host::serial_ports();
        for (auto& p : ports_)
            if (ImGui::Selectable(p.c_str(), p == port_))
                port_ = p;
        ImGui::EndCombo();
    }
    help_marker("The sign's DB9 (RS-232) through a USB serial adapter, 115200 8N1. Commands from tools/signctl.py "
                "(build/sim/sign.sock) run here as if their button had been pressed.");
    bool can = !port_.empty() && !busy_;

    auto tab = [&](const char* label, int id) {
        ImGuiTabItemFlags f = select_tab_ == id ? ImGuiTabItemFlags_SetSelected : 0;
        return ImGui::BeginTabItem(label, nullptr, f);
    };
    if (ImGui::BeginTabBar("signtabs")) {
        if (tab("Firmware", TabFirmware)) {
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Image (?)").x - ImGui::GetStyle().ItemSpacing.x * 3);
            ImGui::InputText("Image", image_, sizeof image_);
            help_marker("A whole boot-flash image (make -C bootloader DUMP=1 fwimage). Run from RAM tries it with "
                        "nothing written (a power cycle goes back). Install keeps it: the main flash's update slot, "
                        "with an automatic fallback to the chip's firmware if it fails to start. Needs our boot "
                        "loader on the chip.");
            ImGui::Checkbox("Restart the sign from its console first", &reset_first_);
            help_marker("No power cycle needed while our firmware runs: its console's reset opens the boot "
                        "loader's window.");
            ImGui::BeginDisabled(!can);
            if (ImGui::Button("Run from RAM"))
                start_run(Job::Run);
            ImGui::SameLine();
            if (ImGui::Button("Install on the sign"))
                ImGui::OpenPopup("install?");
            ImGui::SameLine();
            if (ImGui::Button("Uninstall"))
                start_run(Job::Uninstall);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!busy_ || console_on_);
            if (ImGui::Button("Cancel"))
                cancel_ = true;
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("install?")) {
                ImGui::TextUnformatted("Write this image into the sign's update slot? The sign boots it from then on;\n"
                                       "if it fails to start, the sign falls back to its boot flash by itself.");
                if (ImGui::Button("Install")) {
                    start_run(Job::Install);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##inst"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            draw_log();
            ImGui::EndTabItem();
        }
        if (tab("Content", TabContent)) {
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Folder (?)").x - ImGui::GetStyle().ItemSpacing.x * 3);
            ImGui::InputTextWithHint("Folder", "an upload folder (upload.txt, files/)", content_dir_, sizeof content_dir_);
            help_marker("The sign's content store (apps, imported emoji) and the apps' data, over the console. The "
                        "Studio's Upload apps and its feeds hand their folder to this tab (build/sim/uploads/...), so "
                        "every upload shows here. Upload again resends the folder above.");
            ImGui::BeginDisabled(!can || !content_dir_[0]);
            if (ImGui::Button("Upload again"))
                start_run(Job::Content);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!busy_ || console_on_);
            if (ImGui::Button("Cancel##content"))
                cancel_ = true;
            ImGui::EndDisabled();
            draw_log();
            ImGui::EndTabItem();
        }
        if (tab("Debug", TabDebug)) {
            ImGui::BeginDisabled(!can);
            if (ImGui::Button("Board info"))
                start_run(Job::Info);
            ImGui::SameLine();
            if (ImGui::Button("Boot history"))
                start_run(Job::Boots);
            ImGui::SameLine();
            if (ImGui::Button("Retry the installed image"))
                start_run(Job::RetrySlot);
            ImGui::SameLine();
            if (ImGui::Button("Clear the crash"))
                start_run(Job::ClearCrash);
            if (ImGui::Button("Slot boot: RAM stage"))
                start_run(Job::SlotStage);
            ImGui::SameLine();
            if (ImGui::Button("Slot boot: direct (debug)"))
                start_run(Job::SlotDirect);
            ImGui::SameLine();
            if (ImGui::Button("Test fault"))
                start_run(Job::TestFault);
            ImGui::EndDisabled();
            help_marker("From the boot loader's window, before the firmware runs: the NVRAM debug record (the last 8 "
                        "boots' reset causes and steps, the last crash with its registers) and the boot switches.");
            draw_log();
            ImGui::EndTabItem();
        }
        if (tab("Stage 1", TabLoader)) {
            ImGui::BeginDisabled(!can);
            if (ImGui::Button("Install stage 1"))
                start_run(Job::InstallLoader);
            ImGui::SameLine();
            if (ImGui::Button("Retry stage 1"))
                start_run(Job::RetryLoader);
            ImGui::SameLine();
            if (ImGui::Button("Remove stage 1"))
                start_run(Job::UninstallLoader);
            ImGui::EndDisabled();
            help_marker("The boot loader itself, updated over the DB9 (bootloader/build-dump-s1/stage1.img): the chip "
                        "keeps a fixed first stage and runs this one from the main flash, going back to its own if it "
                        "fails twice. Retry and Remove reach the chip's own window (recovery), whatever stage 1 does.");
            ImGui::BeginDisabled(!can);
            if (ImGui::Button("Write the chip (stage 0)"))
                ImGui::OpenPopup("writechip?");
            ImGui::EndDisabled();
            help_marker("Writes the Firmware tab's image into the boot-flash chip itself, over serial. Only works while "
                        "a stage 1 is running. A power loss during this write needs the programmer to recover.");
            if (ImGui::BeginPopup("writechip?")) {
                ImGui::TextUnformatted("Write the image into the boot-flash chip itself (stage 0), over serial.\n"
                                       "Only works while a stage 1 is running. This is the one write a power loss cannot\n"
                                       "recover from without the programmer. Continue?");
                if (ImGui::Button("Write the chip")) {
                    start_run(Job::WriteChip);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel##wc"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            draw_log();
            ImGui::EndTabItem();
        }
        if (tab("Console", TabConsole)) {
            if (!console_on_) {
                ImGui::BeginDisabled(!can);
                if (ImGui::Button("Connect"))
                    start_console();
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Disconnect"))
                    console_on_ = false;
                ImGui::SameLine();
                if (ImGui::Button("Restart the sign")) {
                    std::lock_guard<std::mutex> g(mu_);
                    to_send_.push_back("reset");
                }
            }
            help_marker("The firmware's console (MQ?MON): help lists its commands. Crash reports get function names.");
            ImGui::BeginChild("console", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
            {
                std::lock_guard<std::mutex> g(mu_);
                ui_mono();
                ImGui::TextUnformatted(console_.data(), console_.data() + console_.size());
                ui_pop();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::BeginDisabled(!console_on_);
            ImGui::SetNextItemWidth(-1);
            ui_mono();
            bool entered = ImGui::InputTextWithHint("##cmd", "console command", input_, sizeof input_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
            ui_pop();
            if (entered) {
                std::lock_guard<std::mutex> g(mu_);
                to_send_.push_back(input_);
                input_[0] = 0;
                ImGui::SetKeyboardFocusHere(-1);
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    select_tab_ = TabNone;
    ImGui::End();
}

}  // namespace sim
