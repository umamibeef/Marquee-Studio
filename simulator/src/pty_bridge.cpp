#include "pty_bridge.h"

#include "machine.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace sim {

PtyBridge::PtyBridge(Machine& m, int minor, std::string link) : m_(m), minor_(minor), link_(std::move(link)) {
    master_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_ < 0 || grantpt(master_) || unlockpt(master_)) {
        error_ = std::string("pseudo-terminal: ") + strerror(errno);
        return;
    }
    const char* name = ptsname(master_);
    if (!name) {
        error_ = "pseudo-terminal: no name";
        return;
    }
    device_ = name;
    // Hold the device open ourselves: without any open device side, reading the controlling side fails (EIO) until
    // a client comes. Raw, so nothing is echoed or translated (a client sets its own mode; the baud rate is moot).
    slave_ = open(device_.c_str(), O_RDWR | O_NOCTTY);
    if (slave_ >= 0) {
        termios t{};
        if (tcgetattr(slave_, &t) == 0) {
            cfmakeraw(&t);
            tcsetattr(slave_, TCSANOW, &t);
        }
    }
    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
    if (!link_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(link_).parent_path(), ec);
        std::filesystem::remove(link_, ec);
        std::filesystem::create_symlink(device_, link_, ec);
        if (ec)
            error_ = "link " + link_ + ": " + ec.message();
    }
    thread_ = std::thread([this] { run(); });
}

PtyBridge::~PtyBridge() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    if (!link_.empty()) {
        std::error_code ec;
        if (std::filesystem::is_symlink(link_, ec) && std::filesystem::read_symlink(link_, ec) == device_)
            std::filesystem::remove(link_, ec);
    }
    if (slave_ >= 0)
        close(slave_);
    if (master_ >= 0)
        close(master_);
}

void PtyBridge::run() {
    // What the firmware has sent on this channel is the whole log (Machine::View::serial); pass on what is new. Start
    // from what is there now: a client gets what is sent after the device appeared.
    size_t sent = m_.view().serial[minor_].size();
    std::vector<uint8_t> buf(4096);
    while (!stop_) {
        pollfd p{master_, POLLIN, 0};
        if (poll(&p, 1, 5) > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(master_, buf.data(), buf.size());
            if (n > 0)
                m_.serial_send(minor_, std::vector<uint8_t>(buf.begin(), buf.begin() + n));
        }
        auto v = m_.view();
        const auto& out = v.serial[minor_];
        if (out.size() < sent)                           // the board was reset: its log starts again
            sent = 0;
        while (sent < out.size()) {
            ssize_t n = write(master_, out.data() + sent, out.size() - sent);
            if (n <= 0)
                break;                                   // nobody reading and the buffer is full: try again later
            sent += (size_t)n;
        }
    }
}

}  // namespace sim
