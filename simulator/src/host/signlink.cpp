#include "signlink.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <thread>

namespace host {

static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- serial port ----
static speed_t baud_const(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 230400: return B230400;
    default: return B115200;
    }
}

SerialPort::SerialPort(const std::string& path, int baud) : path_(path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0)
        throw SignError(path + ": " + strerror(errno));
    termios t{};
    if (tcgetattr(fd_, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag = CS8 | CREAD | CLOCAL;
        cfsetispeed(&t, baud_const(baud));
        cfsetospeed(&t, baud_const(baud));
        tcsetattr(fd_, TCSANOW, &t);
    }
}

SerialPort::~SerialPort() {
    if (fd_ >= 0)
        ::close(fd_);
}

void SerialPort::write(const Bytes8& b) {
    size_t done = 0;
    while (done < b.size()) {
        ssize_t n = ::write(fd_, b.data() + done, b.size() - done);
        if (n < 0) {
            if (errno == EAGAIN) {
                pollfd pf{fd_, POLLOUT, 0};
                poll(&pf, 1, 100);
                continue;
            }
            throw SignError(path_ + ": " + strerror(errno));
        }
        done += (size_t)n;
    }
}

Bytes8 SerialPort::read(int timeout_ms) {
    Bytes8 out;
    pollfd pf{fd_, POLLIN, 0};
    if (poll(&pf, 1, timeout_ms) <= 0)
        return out;
    uint8_t b[4096];
    for (;;) {
        ssize_t n = ::read(fd_, b, sizeof b);
        if (n <= 0)
            break;
        out.insert(out.end(), b, b + n);
        if (poll(&pf, 1, 0) <= 0)
            break;
    }
    return out;
}

void SerialPort::flush_input() {
    tcflush(fd_, TCIFLUSH);
}

// ---- CRC-32 (zlib) ----
uint32_t crc32_bytes(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(Bytes8& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        b.push_back((uint8_t)(v >> s));
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static Bytes8 make_frame(uint8_t type, uint8_t tag, const Bytes8& payload) {
    Bytes8 f = {'M', 'Q', type, tag, (uint8_t)(payload.size() >> 8), (uint8_t)payload.size()};
    for (uint8_t c : payload)
        f.push_back(c ^ tag);
    put32(f, crc32_bytes(f.data() + 2, f.size() - 2));
    return f;
}

Bytes8 safe_frame(uint8_t type, uint8_t& tag, const Bytes8& payload) {
    for (int i = 0; i < 256; i++) {
        uint8_t t = (uint8_t)(tag + i);
        Bytes8 f = make_frame(type, t, payload);
        bool ok = true;
        for (uint8_t c : f)
            if (c == SignWindow::FORBIDDEN) {
                ok = false;
                break;
            }
        if (ok) {
            tag = t;
            return f;
        }
    }
    throw SignError("no tag keeps 0x5A out of the request");
}

// ---- the window ----
bool SignWindow::frame_in(double deadline, uint8_t& type, uint8_t& tag, Bytes8& payload) {
    for (;;) {
        // hunt for MQ, a header, the payload and the CRC in buf_
        size_t i = 0;
        while (i + 1 < buf_.size() && !(buf_[i] == 'M' && buf_[i + 1] == 'Q'))
            i++;
        if (i)
            buf_.erase(buf_.begin(), buf_.begin() + i);
        if (buf_.size() >= 6) {
            size_t n = (size_t)buf_[4] << 8 | buf_[5];
            if (buf_.size() >= 10 + n) {
                uint32_t want = get32(&buf_[6 + n]);
                if (crc32_bytes(&buf_[2], 4 + n) == want) {
                    type = buf_[2];
                    tag = buf_[3];
                    payload.assign(buf_.begin() + 6, buf_.begin() + 6 + n);
                    buf_.erase(buf_.begin(), buf_.begin() + 10 + n);
                    return true;
                }
                buf_.erase(buf_.begin());                   // a false sync: look further
                continue;
            }
        }
        double left = deadline - now_s();
        if (left <= 0)
            return false;
        Bytes8 in = p_.read((int)std::min(50.0, left * 1000) + 1);
        recent_.append(in.begin(), in.end());
        if (recent_.size() > 256)
            recent_.erase(0, recent_.size() - 256);
        buf_.insert(buf_.end(), in.begin(), in.end());
    }
}

Bytes8 SignWindow::request(uint8_t type, const Bytes8& payload, double timeout, int retries) {
    for (int attempt = 0; attempt <= retries; attempt++) {
        tag_++;
        Bytes8 f = safe_frame(type, tag_, payload);
        p_.write(f);
        double deadline = now_s() + timeout * timeout_scale;
        uint8_t rt, rtag;
        Bytes8 rp;
        while (frame_in(deadline, rt, rtag, rp)) {
            if (rtag != tag_)
                continue;                                    // a late reply to an earlier try
            if (rt == ERROR) {
                static const char* why[] = {"?", "unknown request", "bad length", "no such region", "out of range",
                                            "EEPROM did not answer", "speed not reachable",
                                            "the staged image failed its check", "writes go to the stage only",
                                            "the main flash did not erase, program or verify"};
                int code = rp.empty() ? 0 : rp[0];
                std::string msg = std::string("sign says: ") + (code < 10 ? why[code] : "error");
                if (code == 9 && rp.size() >= 12) {             // where the flash failed (serdump.c)
                    static const char* step[] = {"?", "no main flash or image too big", "erase", "program",
                                                 "blank check", "verify"};
                    uint32_t a = uint32_t(rp[4]) << 24 | uint32_t(rp[5]) << 16 | uint32_t(rp[6]) << 8 | rp[7];
                    char d[96];
                    snprintf(d, sizeof d, ": %s at 0x%08x, read 0x%04x, expected 0x%04x", rp[2] < 6 ? step[rp[2]] : "?",
                             a, rp[8] << 8 | rp[9], rp[10] << 8 | rp[11]);
                    msg += d;
                }
                throw SignError(msg);
            }
            if (rt == (type | 0x80))
                return rp;
        }
        if (recent_.find("MQDUMP CLOSED") != std::string::npos)
            throw SignError("the window closed and the sign went on booting");
    }
    throw SignError("no reply from the sign");
}

std::string SignWindow::wait_window(double seconds, const std::atomic<bool>* cancel, bool recovery) {
    static const Bytes8 magic = {'M', 'Q', '?', 'D', 'U', 'M', 'P', '\r'};
    static const Bytes8 magic0 = {'M', 'Q', '?', 'D', 'U', 'M', 'P', '0'};   // stage 0's recovery window (300 ms)
    double end = now_s() + seconds;
    std::string seen;
    unsigned sent = 0;
    p_.flush_input();
    while (now_s() < end) {
        if (cancel && *cancel)
            throw SignError("cancelled");
        // the window listens ~2 s after reset; recovery: both magics (without a stage 1 there is no recovery listen)
        p_.write(recovery && sent++ % 2 == 0 ? magic0 : magic);
        Bytes8 in = p_.read(recovery ? 40 : 100);
        seen.append(in.begin(), in.end());
        size_t k = seen.find("MQDUMP READY");
        if (k != std::string::npos) {
            size_t e = seen.find("\r\n", k);
            for (double t = now_s() + 0.5; e == std::string::npos && now_s() < t;) {
                Bytes8 more = p_.read(50);
                seen.append(more.begin(), more.end());
                e = seen.find("\r\n", k);
            }
            std::string line = seen.substr(k, e == std::string::npos ? std::string::npos : e - k);
            buf_.assign(seen.begin() + (e == std::string::npos ? seen.size() : e + 2), seen.end());
            p_.read(200);                                    // magics sent meanwhile are ignored by the window
            return line;
        }
        if (seen.size() > 4096)
            seen.erase(0, seen.size() - 64);
    }
    throw SignError("no answer: power the sign on (with our boot loader) while the Studio waits");
}

std::string SignWindow::hello() {
    Bytes8 r = request(HELLO, {}, 2.0);
    return std::string(r.begin(), r.end());
}

uint32_t SignWindow::stage(const Bytes8& image, const std::function<void(uint32_t, uint32_t)>& progress,
                           const std::atomic<bool>* cancel) {
    uint32_t n = (uint32_t)image.size();
    while (n && image[n - 1] == 0xFF)
        n--;
    n = std::min<uint32_t>((n + 3) & ~3u, (uint32_t)image.size());
    if (n > STAGE_SIZE)
        throw SignError("the image is larger than the sign's stage (512 KB)");
    uint32_t off = 0;
    while (off < n) {
        if (cancel && *cancel)
            throw SignError("cancelled");
        uint32_t size = std::min(WRITE_MAX, n - off);
        Bytes8 pl;
        for (;;) {                                           // the largest chunk some tag keeps 0x5A out of
            pl = {R_STAGE, 0, 0, 0};
            put32(pl, off);
            pl.insert(pl.end(), image.begin() + off, image.begin() + off + size);
            uint8_t t = tag_;
            try {
                (void)safe_frame(WRITE, t, pl);
                break;
            } catch (const SignError&) {
                if (size <= 32)
                    throw;
                size /= 2;
            }
        }
        Bytes8 want = {R_STAGE, 0, 0, 0};
        put32(want, off);
        put32(want, size);
        put32(want, crc32_bytes(image.data() + off, size));
        bool ok = false;
        for (int k = 0; k < 4 && !ok; k++)
            ok = request(WRITE, pl, 1.0 + (size + 64) * 10.0 / 115200 * 1.5) == want;
        if (!ok)
            throw SignError("a block was not stored correctly");
        off += size;
        if (progress)
            progress(off, n);
    }
    return n;
}

void SignWindow::install_loader(const Bytes8& image) {
    if (image.size() < 32 || image.size() > 0x10000 || std::string(image.begin(), image.begin() + 4) != "MQS1")
        throw SignError("not a stage-1 image (bootloader/build-dump-s1/stage1.img)");
    uint32_t n = stage(image);
    Bytes8 pl;
    put32(pl, n);
    put32(pl, crc32_bytes(image.data(), n));
    request(INSTALL_S1, pl, 30.0, 0);
}

void SignWindow::uninstall_loader() {
    request(UNINSTALL_S1, {}, 30.0, 0);
}

void SignWindow::install_bootflash(const Bytes8& image) {
    uint32_t n = stage(image);
    Bytes8 pl;
    put32(pl, n);
    put32(pl, crc32_bytes(image.data(), n));
    request(INSTALL_BOOT, pl, 120.0, 0);
}

void SignWindow::run(const Bytes8& image, uint32_t n) {
    Bytes8 pl;
    put32(pl, n);
    put32(pl, crc32_bytes(image.data(), n));
    try {
        request(RUN, pl, 3.0, 3);
    } catch (const SignError& e) {
        if (std::string(e.what()).find("window closed") == std::string::npos)
            throw;
    }
}

void SignWindow::install(const Bytes8& image, uint32_t n) {
    Bytes8 pl;
    put32(pl, n);
    put32(pl, crc32_bytes(image.data(), n));
    request(INSTALL, pl, 180.0, 0);                     // erase, program and verify: up to a minute or two
}

void SignWindow::uninstall() {
    request(UNINSTALL, {}, 60.0, 0);
}

Bytes8 SignWindow::read(uint8_t region, uint32_t off, uint32_t len) {
    Bytes8 q = {region, 0, 0, 0, (uint8_t)(off >> 24), (uint8_t)(off >> 16), (uint8_t)(off >> 8), (uint8_t)off,
                (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    Bytes8 r = request(READ, q, 2.0 + len * 10.0 / 115200 * 1.5, 3);
    if (r.size() != 12 + len || !std::equal(q.begin(), q.end(), r.begin()))
        throw SignError("bad READ reply");
    return Bytes8(r.begin() + 12, r.end());
}

std::array<uint8_t, 3> SignWindow::option(uint8_t op, uint8_t value) {
    Bytes8 r = request(OPTION, {op, value}, 2.0, op == OPT_TEST_FAULT ? 0 : 1);
    if (r.size() < 3)
        throw SignError("bad OPTION reply");
    return {r[0], r[1], r[2]};
}

void SignWindow::boot() {
    try {
        request(BOOT, {}, 1.0, 3);
    } catch (const SignError&) {
    }
}

// ---- the app's console ----
static std::string read_until(SerialPort& port, const std::string& end, double seconds) {
    std::string s;
    double t = now_s() + seconds;
    while (now_s() < t && s.find(end) == std::string::npos) {
        Bytes8 in = port.read(50);
        s.append(in.begin(), in.end());
    }
    return s;
}

std::string console_open(SerialPort& port, double seconds) {
    port.write({'M', 'Q', '?', 'M', 'O', 'N', '\r'});
    return read_until(port, "mq> ", seconds);
}

std::string console_command(SerialPort& port, const std::string& line, double seconds) {
    Bytes8 b(line.begin(), line.end());
    b.push_back('\r');
    port.write(b);
    return read_until(port, "mq> ", seconds);
}

std::string console_raw(SerialPort& port, const Bytes8& data, double seconds, const char* cmd) {
    std::string l = std::string(cmd) + " " + [&] { char b[16]; snprintf(b, sizeof b, "%zx", data.size()); return std::string(b); }() + "\r";
    // the line and its bytes back to back: the console reads its input in order, so the bytes after the line are
    // taken raw (pieces stay small enough for the UART's receive buffer)
    Bytes8 all(l.begin(), l.end());
    all.insert(all.end(), data.begin(), data.end());
    port.write(all);
    return read_until(port, "mq> ", seconds);
}

std::vector<std::string> console_lines(const std::string& reply, const std::string& sent) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
        std::string t = a == std::string::npos ? "" : cur.substr(a, b - a + 1);
        if (t.rfind("mq> ", 0) == 0 || t == "mq>")
            t = t.size() > 4 ? t.substr(4) : "";
        if (!t.empty() && !(out.empty() && t == sent))
            out.push_back(t);
        cur.clear();
    };
    for (char c : reply)
        if (c == '\n')
            flush();
        else if (c != '\r')
            cur += c;
    flush();
    return out;
}

bool console_reset(SerialPort& port, double seconds) {
    if (console_open(port, seconds).find("mq> ") == std::string::npos)
        return false;
    // a restarting sign prints no prompt again: wait for its "resetting" only, then return at once, so the caller's
    // magic reaches the boot loader's window (open about 2 s, from about 0.5 s after the restart)
    port.write({'r', 'e', 's', 'e', 't', '\r'});
    return read_until(port, "resetting", 1.0).find("resetting") != std::string::npos;
}

}  // namespace host
