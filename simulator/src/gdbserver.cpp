#include "gdbserver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace sim {

static const int NREGS = 38;

static std::string target_xml() {
    std::string x = "<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target version=\"1.0\">\n"
                    "  <architecture>powerpc:MPC8XX</architecture>\n  <feature name=\"org.gnu.gdb.power.core\">\n";
    for (int i = 0; i < 32; i++)
        x += "    <reg name=\"r" + std::to_string(i) + "\" bitsize=\"32\" type=\"" + (i == 1 ? "data_ptr" : "uint32") + "\"/>\n";
    x += "    <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>\n    <reg name=\"msr\" bitsize=\"32\" type=\"uint32\"/>\n"
         "    <reg name=\"cr\" bitsize=\"32\" type=\"uint32\"/>\n    <reg name=\"lr\" bitsize=\"32\" type=\"code_ptr\"/>\n"
         "    <reg name=\"ctr\" bitsize=\"32\" type=\"uint32\"/>\n    <reg name=\"xer\" bitsize=\"32\" type=\"uint32\"/>\n"
         "  </feature>\n</target>\n";
    return x;
}

static const char* MONITOR_HELP =
    "monitor commands:\n"
    "  status          where the simulator is, simulated time, panel frames, device events\n"
    "  reset           boot again from the flash image (breakpoints are kept)\n"
    "  tasks           the RTEMS tasks (id, name, priority, state, where)\n"
    "  frame           the latest panel frame as text\n"
    "  ir KEYS         queue IR remote keys (! is Menu/Enter, ] next, [ up, } in, { back, - exit)\n"
    "  db9 TEXT        send TEXT to the DB9 port (/dev/rs232); \\xNN escapes allowed\n"
    "  rs485 TEXT      send TEXT to the RS-485 port (/dev/rs485)\n"
    "  serial          what the sign has sent on each serial port\n";

static std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; i++)
        o += d[p[i] >> 4], o += d[p[i] & 15];
    return o;
}

static std::string hex(const std::string& s) { return hex((const uint8_t*)s.data(), s.size()); }

static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        v.push_back((uint8_t)std::stoul(s.substr(i, 2), nullptr, 16));
    return v;
}

static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::vector<uint8_t> unescape(const std::string& t) {
    std::vector<uint8_t> o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\\' && i + 1 < t.size()) {
            char c = t[i + 1];
            if (c == 'x' && i + 3 < t.size()) {
                o.push_back((uint8_t)std::stoul(t.substr(i + 2, 2), nullptr, 16));
                i += 3;
                continue;
            }
            o.push_back(c == 'r' ? '\r' : c == 'n' ? '\n' : c == 't' ? '\t' : c == '0' ? 0 : (uint8_t)c);
            i++;
            continue;
        }
        o.push_back((uint8_t)t[i]);
    }
    return o;
}

std::string frame_text(const std::vector<uint8_t>& f) {
    size_t n = f.size() / 2, w = 80;
    for (size_t x : {80, 120, 160, 200, 320})
        if (n % x == 0 && (n / x) % 16 == 0 && n / x <= 96) {
            w = x;
            break;
        }
    std::string o;
    for (size_t y = 0; y < n / w; y++) {
        for (size_t x = 0; x < w; x++)
            o += ((f[2 * (y * w + x)] << 8 | f[2 * (y * w + x) + 1]) & 0x7FFF) ? '#' : '.';
        o += '\n';
    }
    return o;
}

GdbServer::GdbServer(Machine& m, int port) : m_(m), port_(port) {
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_, (sockaddr*)&a, sizeof a) || listen(listen_, 1)) {
        error = fmt("gdb server: cannot listen on port %d (%s)", port, strerror(errno));
        close(listen_);
        listen_ = -1;
        return;
    }
    socklen_t l = sizeof a;
    getsockname(listen_, (sockaddr*)&a, &l);
    port_ = ntohs(a.sin_port);
    m_.on_stop = [this](uint32_t pc, int sig, uint32_t tid) {   // on the emulation thread: no calls into m_
        last_sig_ = sig;
        if (waiting_.exchange(false) && conn_ >= 0) {
            thread_sel_ = 0;
            auto v = m_.view();
            std::string extra;
            if (sig == 5 && v.stop_reason == "breakpoint")
                extra = "swbreak:;";
            else if (sig == 5 && v.stop_reason == "watchpoint")
                extra = fmt("%s:%x;", v.watch_kind == 1 ? "watch" : v.watch_kind == 2 ? "rwatch" : "awatch", v.watch_addr);
            send(fmt("T%02x20:%08x;thread:%x;", sig, pc, tid) + extra);
        }
    };
    thread_ = std::thread([this] { serve(); });
}

GdbServer::~GdbServer() {
    quit_ = true;
    m_.on_stop = nullptr;
    if (listen_ >= 0)
        shutdown(listen_, SHUT_RDWR), close(listen_);
    int c = conn_.exchange(-1);
    if (c >= 0)
        shutdown(c, SHUT_RDWR), close(c);
    if (thread_.joinable())
        thread_.join();
}

void GdbServer::serve() {
    while (!quit_) {
        pollfd p{listen_, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0)
            continue;
        int fd = accept(listen_, nullptr, nullptr);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        m_.halt();                                       // gdb expects a stopped target on attach
        for (int i = 0; i < 400 && m_.running(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        conn_ = fd;
        ack_ = true;
        thread_sel_ = 0;
        session(fd);
        int c = conn_.exchange(-1);
        if (c >= 0)
            close(c);
    }
}

void GdbServer::send(const std::string& payload) {
    unsigned sum = 0;
    for (unsigned char c : payload)
        sum += c;
    std::string pkt = "$" + payload + fmt("#%02x", sum & 0xFF);
    std::lock_guard<std::mutex> g(send_mu_);
    int fd = conn_;
    if (fd >= 0)
        ::send(fd, pkt.data(), pkt.size(), 0);
}

void GdbServer::session(int fd) {
    std::string buf;
    char chunk[65536];
    bool closed = false;
    while (!closed && !quit_) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0)
            continue;
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0)
            break;
        buf.append(chunk, (size_t)n);
        while (!buf.empty() && !closed) {
            char c = buf[0];
            if (c == '+' || c == '-') {
                buf.erase(0, 1);
            } else if (c == 0x03) {                      // Ctrl-C
                buf.erase(0, 1);
                m_.halt();
            } else if (c == '$') {
                size_t end = buf.find('#');
                if (end == std::string::npos || buf.size() < end + 3)
                    break;
                std::string payload = buf.substr(1, end - 1), raw;
                buf.erase(0, end + 3);
                if (ack_) {
                    std::lock_guard<std::mutex> g(send_mu_);
                    ::send(fd, "+", 1, 0);
                }
                for (size_t i = 0; i < payload.size(); i++)   // binary escapes
                    raw += payload[i] == 0x7D && i + 1 < payload.size() ? (char)(payload[++i] ^ 0x20) : payload[i];
                std::string reply = handle(raw, closed);
                if (reply != "\x01")
                    send(reply);
            } else
                buf.erase(0, 1);
        }
    }
    waiting_ = false;
}

uint32_t GdbServer::current_tid(const std::vector<Task>& tasks) {
    for (auto& t : tasks)
        if (t.current)
            return t.id;
    return 1;
}

std::vector<uint32_t> GdbServer::thread_regs(bool& current) {
    current = true;
    if (thread_sel_ > 0)
        for (auto& t : m_.tasks())
            if (t.id == (uint32_t)thread_sel_) {
                current = t.current;
                return std::vector<uint32_t>(t.regs, t.regs + NREGS);
            }
    return m_.regs();
}

std::string GdbServer::stop_reply(int sig) {
    auto tasks = m_.tasks();
    uint32_t pc = m_.regs()[32];
    thread_sel_ = 0;
    std::string extra = sig == 5 && m_.view().stop_reason == "breakpoint" ? "swbreak:;" : "";
    return fmt("T%02x20:%08x;thread:%x;", sig, pc, current_tid(tasks)) + extra;
}

std::string GdbServer::monitor(const std::string& cmd) {
    std::string word = cmd.substr(0, cmd.find(' ')), rest = cmd.find(' ') == std::string::npos ? "" : cmd.substr(cmd.find(' ') + 1);
    std::string out;
    if (word.empty() || word == "help")
        out = MONITOR_HELP;
    else if (word == "status") {
        auto v = m_.view();
        out = fmt("%s at 0x%x %s; %llu instructions, %.3f s simulated, %llu panel frames\n",
                  v.running ? "running" : ("halted (" + v.stop_reason + ")").c_str(), v.pc,
                  m_.repo.symbols.describe(v.pc).c_str(), (unsigned long long)v.icount,
                  v.now / (CPU_MIPS * 1e6), (unsigned long long)v.frames);
        out += "events:";
        for (auto& [k, n] : v.events)
            out += fmt(" %s %llu,", k.c_str(), (unsigned long long)n);
        out += "\n";
    } else if (word == "reset") {
        m_.reset();
        out = "booted again; halted at the app entry\n";
    } else if (word == "tasks") {
        for (auto& t : m_.tasks())
            out += fmt("%c 0x%08x %-9s prio %3u state 0x%04x pc 0x%x %s\n", t.current ? '*' : ' ', t.id, t.name.c_str(),
                       t.prio, t.state, t.regs[32], m_.repo.symbols.describe(t.regs[32]).c_str());
        if (out.empty())
            out = "no RTEMS tasks yet\n";
    } else if (word == "frame") {
        auto v = m_.view();
        out = v.frame.empty() ? "no panel frame yet\n" : frame_text(v.frame);
    } else if (word == "ir") {
        m_.ir_press(rest);
        out = fmt("queued %zu IR key(s)\n", rest.size());
    } else if (word == "db9" || word == "rs485") {
        auto d = unescape(rest);
        m_.serial_send(word == "db9" ? 4 : 1, d);
        out = fmt("queued %zu byte(s) on %s\n", d.size(), word.c_str());
    } else if (word == "serial") {
        auto v = m_.view();
        for (auto [name, minor] : {std::pair{"db9", 4}, std::pair{"rs485", 1}})
            out += std::string(name) + ": " + (v.serial[minor].empty() ? "(nothing)" : hex(v.serial[minor].data(), v.serial[minor].size())) + "\n";
    } else
        out = "unknown monitor command '" + word + "'\n" + MONITOR_HELP;
    send("O" + hex(out));
    return "OK";
}

std::string GdbServer::handle(const std::string& p, bool& closed) {
    char c = p.empty() ? 0 : p[0];
    auto starts = [&](const char* s) { return p.rfind(s, 0) == 0; };
    if (starts("qSupported"))
        return "PacketSize=4000;qXfer:features:read+;QStartNoAckMode+;swbreak+;hwbreak+;vContSupported+";
    if (p == "QStartNoAckMode") {
        send("OK");
        ack_ = false;
        return "\x01";
    }
    if (starts("qXfer:features:read:target.xml:")) {
        static const std::string xml = target_xml();
        auto args = p.substr(p.rfind(':') + 1);
        size_t off = std::stoul(args.substr(0, args.find(',')), nullptr, 16);
        size_t len = std::stoul(args.substr(args.find(',') + 1), nullptr, 16);
        std::string data = off < xml.size() ? xml.substr(off, len) : "";
        return (off + len >= xml.size() ? "l" : "m") + data;
    }
    if (p == "?")
        return stop_reply(last_sig_);
    if (p == "qAttached")
        return "1";
    if (p == "qC")
        return fmt("QC%x", current_tid(m_.tasks()));
    if (p == "qfThreadInfo") {
        auto tasks = m_.tasks();
        std::string o = "m";
        for (size_t i = 0; i < tasks.size(); i++)
            o += fmt(i ? ",%x" : "%x", tasks[i].id);
        return tasks.empty() ? "m1" : o;
    }
    if (p == "qsThreadInfo")
        return "l";
    if (starts("qThreadExtraInfo,")) {
        uint32_t tid = (uint32_t)std::stoul(p.substr(17), nullptr, 16);
        for (auto& t : m_.tasks())
            if (t.id == tid)
                return hex(fmt("%s prio %u state 0x%x%s", t.name.c_str(), t.prio, t.state, t.current ? " (running)" : ""));
        return hex(std::string("cpu"));
    }
    if (c == 'H') {
        std::string a = p.substr(2);
        int64_t tid = (a.empty() || a == "-1") ? -1 : (int64_t)std::stoul(a, nullptr, 16);
        if (p[1] == 'g')
            thread_sel_ = tid;
        return "OK";
    }
    if (c == 'T') {
        uint32_t tid = (uint32_t)std::stoul(p.substr(1), nullptr, 16);
        auto tasks = m_.tasks();
        if (tasks.empty())
            return tid == 1 ? "OK" : "E01";
        for (auto& t : tasks)
            if (t.id == tid)
                return "OK";
        return "E01";
    }
    if (p == "qOffsets")
        return "Text=0;Data=0;Bss=0";
    if (starts("qSymbol"))
        return "OK";
    if (starts("qRcmd,")) {
        auto b = unhex(p.substr(6));
        return monitor(std::string(b.begin(), b.end()));
    }
    if (c == 'g') {
        bool cur;
        auto r = thread_regs(cur);
        std::string o;
        for (int i = 0; i < NREGS; i++)
            o += fmt("%08x", r[i]);
        return o;
    }
    if (c == 'G') {
        bool cur;
        thread_regs(cur);
        if (!cur)
            return "E01";
        auto d = unhex(p.substr(1));
        for (int i = 0; i < NREGS && 4 * i + 3 < (int)d.size(); i++)
            m_.set_reg(i, (uint32_t)d[4 * i] << 24 | d[4 * i + 1] << 16 | d[4 * i + 2] << 8 | d[4 * i + 3]);
        return "OK";
    }
    if (c == 'p') {
        int n = (int)std::stoul(p.substr(1), nullptr, 16);
        if (n >= NREGS)
            return "E01";
        bool cur;
        return fmt("%08x", thread_regs(cur)[n]);
    }
    if (c == 'P') {
        int n = (int)std::stoul(p.substr(1, p.find('=') - 1), nullptr, 16);
        bool cur;
        thread_regs(cur);
        if (n >= NREGS || !cur)
            return "E01";
        m_.set_reg(n, (uint32_t)std::stoul(p.substr(p.find('=') + 1), nullptr, 16));
        return "OK";
    }
    if (c == 'm') {
        uint32_t a = (uint32_t)std::stoul(p.substr(1, p.find(',') - 1), nullptr, 16);
        size_t n = std::stoul(p.substr(p.find(',') + 1), nullptr, 16);
        std::vector<uint8_t> d(n);
        if (n && !m_.read_mem(a, d.data(), n))
            return "E14";
        return hex(d.data(), n);
    }
    if (c == 'M') {
        auto colon = p.find(':');
        uint32_t a = (uint32_t)std::stoul(p.substr(1, p.find(',') - 1), nullptr, 16);
        auto d = unhex(p.substr(colon + 1));
        return m_.write_mem(a, d.data(), d.size()) ? "OK" : "E14";
    }
    if (c == 'Z' || c == 'z') {
        std::stringstream ss(p.substr(1));
        std::string kind, as, ls;
        std::getline(ss, kind, ','), std::getline(ss, as, ','), std::getline(ss, ls, ';');
        uint32_t a = (uint32_t)std::stoul(as, nullptr, 16), len = (uint32_t)std::stoul(ls, nullptr, 16);
        bool on = c == 'Z';
        if (kind == "0" || kind == "1")
            m_.set_breakpoint(a, on);
        else if (kind == "2" || kind == "3" || kind == "4")
            m_.set_watchpoint({a, len ? len : 1, kind == "2" ? 1 : kind == "3" ? 2 : 3}, on);
        else
            return "";
        return "OK";
    }
    auto resume = [&]() -> std::string {
        thread_sel_ = 0;
        waiting_ = true;
        m_.run();
        return "\x01";
    };
    auto step = [&]() -> std::string {
        thread_sel_ = 0;
        waiting_ = true;
        m_.step(1);
        return "\x01";
    };
    if (p == "vCont?")
        return "vCont;c;C;s;S;t";
    if (starts("vCont;")) {
        if (p.find(";s") != std::string::npos || p.find(";S") != std::string::npos)
            return step();
        if (p.find(";c") != std::string::npos || p.find(";C") != std::string::npos)
            return resume();
        if (p.find(";t") != std::string::npos) {
            m_.halt();
            return "OK";
        }
        return "E01";
    }
    if (c == 'c' || c == 's') {
        if (p.size() > 1)
            m_.set_reg(32, (uint32_t)std::stoul(p.substr(1), nullptr, 16));
        return c == 'c' ? resume() : step();
    }
    if (c == 'C' || c == 'S')
        return c == 'C' ? resume() : step();
    if (c == 'D') {
        send("OK");
        m_.run();
        closed = true;
        return "\x01";
    }
    if (c == 'k') {
        closed = true;
        return "\x01";
    }
    return "";
}

}  // namespace sim
