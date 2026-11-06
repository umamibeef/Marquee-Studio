/* UMFRAME: the from-source framed/addressed delivery codec for DB9 + RS-485 (see umami-delivery-proposal.md).
 * Decision-independent primitives -- COBS framing + CRC16-CCITT + frame build/parse. Freestanding (stdint only),
 * usable in the firmware and host-testable. The frame field layout and the auto-detect wiring are pending the
 * user's approval; these primitives stand regardless. */
#ifndef UMFRAME_H
#define UMFRAME_H
#include <stdint.h>

#define UMF_SOF          0x7Eu
#define UMF_MAXPAYLOAD   512
#define UMF_HDR          7      /* dest, src, type, seq_hi, seq_lo, len_hi, len_lo */
#define UMF_DECMAX       (UMF_HDR + UMF_MAXPAYLOAD + 2)   /* header + payload + CRC16 */

/* frame types (initial set) */
enum { UMF_PING = 0x01, UMF_PONG = 0x02, UMF_SET_TIME = 0x10, UMF_FEED = 0x11,
       UMF_SCHED_BEGIN = 0x20, UMF_SCHED_CHUNK = 0x21, UMF_SCHED_COMMIT = 0x22,
       UMF_UPDATE_BEGIN = 0x30, UMF_UPDATE_CHUNK = 0x31, UMF_UPDATE_COMMIT = 0x32,
       UMF_STATUS = 0x40, UMF_ACK = 0x7E /*reserved*/, UMF_ACKF = 0x7F, UMF_NAK = 0x7D };

struct umf { uint8_t dest, src, type; uint16_t seq; const uint8_t *payload; int len; };

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) over n bytes. */
uint16_t umf_crc16(const uint8_t *p, int n);
/* COBS encode/decode, excluding the trailing 0x00 delimiter. encode needs out >= n + n/254 + 2; returns length.
 * decode returns the decoded length, or -1 if the input is malformed. */
int umf_cobs_encode(const uint8_t *in, int n, uint8_t *out);
int umf_cobs_decode(const uint8_t *in, int n, uint8_t *out);
/* Build one on-wire frame into `wire` (SOF, COBS(header + payload + CRC16), 0x00). Returns the wire length, or -1. */
int umf_build(uint8_t dest, uint8_t src, uint8_t type, uint16_t seq,
              const uint8_t *payload, int len, uint8_t *wire, int cap);
/* Parse the COBS bytes `body` (between SOF and the 0x00 terminator) into `f`, using `dec` (>= UMF_DECMAX) as the
 * decode scratch that f->payload points into. Returns the payload length (>=0) on success, -1 on a bad CRC/length. */
int umf_parse(const uint8_t *body, int n, struct umf *f, uint8_t *dec, int cap);

#endif
