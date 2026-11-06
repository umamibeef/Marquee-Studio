// The sign's RS-485 protocol as the Cougar task reads it (studio/marquee_studio/signproto.py, proto_coug_task
// 0x2F7BC): DLE-stuffed frames 03 len addr ctl payload crc 04 carrying 69-byte jackpot/text records.
#pragma once

#include "text.h"

#include <utility>

namespace host {

constexpr uint8_t RS485_STX = 0x03, RS485_ETX = 0x04, RS485_DLE = 0x10;
constexpr size_t RECORD_TEXT = 60;                       // text bytes in a record

uint16_t crc16_xmodem(const uint8_t* p, size_t n, uint16_t crc = 0);
uint16_t crc16_ccitt_refl(const uint8_t* p, size_t n, uint16_t crc = 0);

Bytes rs485_frame(uint16_t dest, const Bytes& payload, uint8_t ctl = 0);
// One record: flags (level | 0x20 to show the text), value A, value B (big-endian), the text NUL-padded to 60.
// The text record encodes Latin-1 with '?' for anything else and cuts at 60; the markup record takes bytes as
// they are and throws std::invalid_argument when there are more than 60.
Bytes rs485_text_record(const std::u32string& text, int level = 0, bool show = true, uint32_t value_a = 0,
                        uint32_t value_b = 0);
Bytes rs485_markup_record(const Bytes& data, int level = 0, bool show = true, uint32_t value_a = 0,
                          uint32_t value_b = 0);
inline Bytes rs485_text_frame(const std::u32string& text, int level = 0, uint16_t dest = 1) {
    return rs485_frame(dest, rs485_text_record(text, level));
}
inline Bytes rs485_markup_frame(const Bytes& data, int level = 0, uint16_t dest = 1) {
    return rs485_frame(dest, rs485_markup_record(data, level));
}
// The firmware's receiver: (dest, payload) of every frame it would dispatch.
std::vector<std::pair<uint16_t, Bytes>> coug_receive(const Bytes& stream);

}  // namespace host
