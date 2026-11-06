#include "signproto.h"

#include <stdexcept>

namespace host {

uint16_t crc16_xmodem(const uint8_t* p, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i] << 8;
        for (int k = 0; k < 8; k++)
            crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint16_t crc16_ccitt_refl(const uint8_t* p, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? (uint16_t)(crc >> 1 ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}

Bytes rs485_frame(uint16_t dest, const Bytes& payload, uint8_t ctl) {
    Bytes body;
    be16(body, (uint32_t)payload.size() + 5);
    be16(body, dest);
    body.push_back(ctl);
    body.insert(body.end(), payload.begin(), payload.end());
    be16(body, crc16_ccitt_refl(body.data(), body.size()));
    Bytes out{RS485_STX};
    for (uint8_t b : body)
        if (b == RS485_STX || b == RS485_ETX || b == RS485_DLE) {
            out.push_back(RS485_DLE);
            out.push_back((uint8_t)(b + 0x10));
        } else
            out.push_back(b);
    out.push_back(RS485_ETX);
    return out;
}

static Bytes record(Bytes text, int level, bool show, uint32_t a, uint32_t b) {
    Bytes rec{(uint8_t)((level & 0x0F) | (show ? 0x20 : 0))};
    be32(rec, a);
    be32(rec, b);
    text.resize(RECORD_TEXT, 0);
    rec.insert(rec.end(), text.begin(), text.end());
    return rec;
}

Bytes rs485_text_record(const std::u32string& text, int level, bool show, uint32_t value_a, uint32_t value_b) {
    Bytes t = latin1(text, true);                        // emoji not in the font -> '?'
    if (t.size() > RECORD_TEXT)
        t.resize(RECORD_TEXT);
    return record(t, level, show, value_a, value_b);
}

Bytes rs485_markup_record(const Bytes& data, int level, bool show, uint32_t value_a, uint32_t value_b) {
    if (data.size() > RECORD_TEXT)
        throw std::invalid_argument(std::to_string(data.size()) + " bytes: a jackpot/text record holds 60");
    return record(data, level, show, value_a, value_b);
}

std::vector<std::pair<uint16_t, Bytes>> coug_receive(const Bytes& stream) {
    std::vector<std::pair<uint16_t, Bytes>> out;
    int state = 0, dle = 0;
    uint16_t crc = 0, rx_crc = 0, plen = 0, addr = 0, idx = 0;
    Bytes buf;
    for (uint8_t b : stream) {
        if (b == 3)
            state = dle = 0;
        if (b == 4 && state != 9)
            state = dle = 0;
        if (!dle) {
            if (b == 0x10)
                dle = 1;
        } else {
            uint8_t u = (uint8_t)(b - 0x10);
            if (u < 0x11) {
                dle = 0;
                b = u;
            } else
                state = 0;
        }
        if (dle)
            continue;
        switch (state) {
        case 0:
            if (b == 3)
                state = 1;
            break;
        case 1:
            crc = crc16_ccitt_refl(&b, 1);
            plen = (uint16_t)(b << 8);
            state = 2;
            break;
        case 2:
            crc = crc16_ccitt_refl(&b, 1, crc);
            plen = (uint16_t)((plen | b) - 5);
            state = 3;
            break;
        case 3:
            crc = crc16_ccitt_refl(&b, 1, crc);
            addr = (uint16_t)(b << 8);
            state = 4;
            break;
        case 4:
            crc = crc16_ccitt_refl(&b, 1, crc);
            addr |= b;
            state = 5;
            break;
        case 5:
            crc = crc16_ccitt_refl(&b, 1, crc);
            idx = 0;
            buf.clear();
            state = 6;
            break;
        case 6:
            crc = crc16_ccitt_refl(&b, 1, crc);
            buf.push_back(b);
            if (++idx == plen)
                state = 7;
            break;
        case 7:
            rx_crc = (uint16_t)(b << 8);
            state = 8;
            break;
        case 8:
            rx_crc |= b;
            state = 9;
            break;
        case 9:
            if (b == 4 && (rx_crc == crc || rx_crc == 0xFFFF))
                out.emplace_back(addr, buf);
            state = 0;
            break;
        }
    }
    return out;
}

}  // namespace host
