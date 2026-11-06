// The simulated sign's serial ports as devices: a pseudo-terminal per emulated port, so host tools (studio/server.py,
// tools/serdump.py, a terminal program) open the simulator exactly as they open the real sign's USB serial adapter.
// Bytes a client writes go into the emulated port (Machine::serial_send); what the firmware sends on that port comes
// out of the device. A stable link (build/sim/db9, build/sim/rs485) points at the current device.
#pragma once
#include <atomic>
#include <string>
#include <thread>

namespace sim {

class Machine;

class PtyBridge {
public:
    // minor: the emulated channel (the DB9 is 4 on the default board, the RS-485 1); link: the stable path to create
    PtyBridge(Machine& m, int minor, std::string link);
    ~PtyBridge();
    const std::string& device() const { return device_; }   // e.g. /dev/ttys012; empty when it could not be made
    const std::string& link() const { return link_; }
    const std::string& error() const { return error_; }

private:
    void run();
    Machine& m_;
    int minor_;
    std::string link_, device_, error_;
    int master_ = -1, slave_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace sim
