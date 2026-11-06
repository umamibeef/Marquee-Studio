#include "serial.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace host {

std::vector<std::string> serial_ports() {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator("/dev", ec)) {
        std::string n = e.path().filename().string();
        if (n.rfind("cu.", 0) == 0 || n.rfind("ttyUSB", 0) == 0 || n.rfind("ttyACM", 0) == 0)
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static speed_t speed(int baud) {
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return B19200;
    }
}

void serial_write(const std::string& port, int baud, const Bytes& data) {
    int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        throw std::runtime_error(port + ": " + strerror(errno));
    termios t{};
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag = CS8 | CREAD | CLOCAL;
        cfsetispeed(&t, speed(baud));
        cfsetospeed(&t, speed(baud));
        tcsetattr(fd, TCSANOW, &t);
    }
    fcntl(fd, F_SETFL, 0);                               // blocking writes from here
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            std::string e = strerror(errno);
            close(fd);
            throw std::runtime_error(port + ": " + e);
        }
        done += (size_t)n;
    }
    tcdrain(fd);
    close(fd);
}

}  // namespace host
