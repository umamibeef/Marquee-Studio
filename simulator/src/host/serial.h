// A serial port for the studio's Send (POSIX termios): an RS-485 adapter at the sign's baud rate, 8N1.
#pragma once

#include "text.h"

#include <string>

namespace host {

// /dev/cu.* (macOS) or /dev/ttyUSB* and /dev/ttyACM* (Linux), sorted.
std::vector<std::string> serial_ports();
// Opens the port, sends the bytes and closes it; throws std::runtime_error with the reason on failure.
void serial_write(const std::string& port, int baud, const Bytes& data);

}  // namespace host
