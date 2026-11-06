// A GDB remote serial protocol server for the simulated sign: gdb, VS Code, Ghidra or any gdb front end attaches over
// TCP (localhost).
//
// The target is a 32-bit big-endian PowerPC (MPC862), described in target.xml as powerpc:MPC8XX with the
// org.gnu.gdb.power.core registers (r0..r31, pc, msr, cr, lr, ctr, xer; the app is soft-float); msr is the shadow
// MSR. Packets: ?, g/G, p/P, m/M, c/C, s/S, vCont (c, s, t), Ctrl-C, Z0/Z1 breakpoints, Z2/Z3/Z4 watchpoints,
// qXfer:features:read, QStartNoAckMode, threads (qC, qfThreadInfo, qThreadExtraInfo, Hg, T) and qRcmd (monitor).
// Threads are the RTEMS tasks (thread id = the RTEMS object id); another task's registers are the context it saved.
#pragma once

#include "machine.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace sim {

class GdbServer {
public:
    GdbServer(Machine& m, int port);
    ~GdbServer();
    int port() const { return port_; }
    bool connected() const { return conn_ >= 0; }
    std::string error;

private:
    Machine& m_;
    int port_, listen_ = -1;
    std::atomic<int> conn_{-1};
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::mutex send_mu_;
    bool ack_ = true;
    std::atomic<bool> waiting_{false};
    int64_t thread_sel_ = 0;
    std::string last_stop_ = "halted";
    int last_sig_ = 5;

    void serve();
    void session(int fd);
    void send(const std::string& payload);
    std::string handle(const std::string& p, bool& closed);
    std::string stop_reply(int sig);
    std::string monitor(const std::string& cmd);
    uint32_t current_tid(const std::vector<Task>& tasks);
    std::vector<uint32_t> thread_regs(bool& current);
};

std::string frame_text(const std::vector<uint8_t>& f);
std::vector<uint8_t> unescape(const std::string& text);

}  // namespace sim
