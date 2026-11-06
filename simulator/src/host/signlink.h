// The sign's DB9, from the Studio: our boot loader's serial window (bootloader/include/serdump.h) and the app's
// console (firmware/ext/console.c). The reference implementation is tools/serdump.py; this follows it.
//
//  Run from RAM: wait for the window at power-on, stage a whole boot-flash image in the sign's RAM, RUN it. Nothing
//  is written to flash or NVRAM; a power cycle without the Studio brings back the chip's firmware.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace host {

using Bytes8 = std::vector<uint8_t>;

class SignError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A serial port (POSIX termios), 8N1, raw.
class SerialPort {
public:
    explicit SerialPort(const std::string& path, int baud = 115200);
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    void write(const Bytes8& b);
    Bytes8 read(int timeout_ms);                         // whatever arrives within timeout_ms (may be empty)
    void flush_input();

private:
    int fd_ = -1;
    std::string path_;
};

// The boot loader's window protocol.
class SignWindow {
public:
    static constexpr uint8_t HELLO = 0x01, READ = 0x02, CRC = 0x03, SPEED = 0x04, PING = 0x05, BOOT = 0x06,
                             WRITE = 0x07, RUN = 0x08, INSTALL = 0x09, UNINSTALL = 0x0A, ERROR = 0xFF;
    static constexpr uint8_t R_STAGE = 8;
    static constexpr uint32_t WRITE_MAX = 0x400, STAGE_SIZE = 0x80000;
    static constexpr uint8_t FORBIDDEN = 0x5A;          // sync byte of the app's DB9 download frames

    SignWindow(SerialPort& port, std::function<void(const std::string&)> log = {}) : p_(port), log_(std::move(log)) {}

    // Sends the magic until the window answers (the sign is powered on meanwhile). Returns the READY line.
    // recovery: the chip's own boot loader's window (MQ?DUMP0), skipping an installed stage 1 for this boot
    std::string wait_window(double seconds, const std::atomic<bool>* cancel = nullptr, bool recovery = false);
    std::string hello();
    // Stages image (trailing 0xFF left out); progress(done, total). Returns the staged length.
    uint32_t stage(const Bytes8& image, const std::function<void(uint32_t, uint32_t)>& progress = {},
                   const std::atomic<bool>* cancel = nullptr);
    void run(const Bytes8& image, uint32_t n);         // RUN: the sign checks the CRC and boots from RAM
    void install(const Bytes8& image, uint32_t n);     // INSTALL: into the update slot, booted from then on
    void uninstall();                                   // UNINSTALL: the boot flash boots again
    void boot();                                        // BOOT: close the window, normal boot from the chip
    Bytes8 read(uint8_t region, uint32_t off, uint32_t len);   // READ (at most 0x4000 bytes)
    // OPTION (bootloader/include/serdump.h OPT_*): returns {debug options, slot tries, boot flags}
    std::array<uint8_t, 3> option(uint8_t op, uint8_t value = 0);
    static constexpr uint8_t OPTION = 0x0B, R_NVRAM = 4;
    static constexpr uint8_t OPT_SET_OPTIONS = 1, OPT_SET_TRIES = 2, OPT_CLEAR_CRASH = 3, OPT_TEST_FAULT = 4,
                             OPT_SET_S1_TRIES = 6, INSTALL_S1 = 0x0C, UNINSTALL_S1 = 0x0D, INSTALL_BOOT = 0x0E;
    // two stages (bootloader/include/serdump.h): a stage-1 boot loader into the main flash's loader sector
    void install_loader(const Bytes8& image);
    void uninstall_loader();
    void install_bootflash(const Bytes8& image);   // write the chip (only from stage 1 / a RUN image); INSTALL_BOOT

    Bytes8 request(uint8_t type, const Bytes8& payload, double timeout, int retries = 6);
    double timeout_scale = 1.0;                         // > 1 against an emulated sign, which is far slower

private:
    bool frame_in(double deadline, uint8_t& type, uint8_t& tag, Bytes8& payload);
    SerialPort& p_;
    std::function<void(const std::string&)> log_;
    Bytes8 buf_;
    std::string recent_;
    uint8_t tag_ = 0;
};

uint32_t crc32_bytes(const uint8_t* p, size_t n, uint32_t crc = 0);
// A request frame with a tag that keeps FORBIDDEN out of every byte sent; throws SignError if none does.
Bytes8 safe_frame(uint8_t type, uint8_t& tag, const Bytes8& payload);

// The app's console (MQ?MON): opens it and returns everything received up to the first prompt.
std::string console_open(SerialPort& port, double seconds);
std::string console_command(SerialPort& port, const std::string& line, double seconds);
// "sraw N" and then the N bytes (the console takes them raw, then prompts): one acknowledged piece of a store file.
std::string console_raw(SerialPort& port, const Bytes8& data, double seconds, const char* cmd = "sraw");
// A console reply as lines: without the echoed command, the prompt and blank lines.
std::vector<std::string> console_lines(const std::string& reply, const std::string& sent);
// Asks our firmware to restart through its console (reset: the firmware's own restart, NVRAM untouched), so the boot
// loader's window opens without a power cycle. False if the sign did not answer (stock firmware, or not running).
bool console_reset(SerialPort& port, double seconds = 3.0);

}  // namespace host
