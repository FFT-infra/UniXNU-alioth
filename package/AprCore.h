// SPDX-License-Identifier: BSD-2-Clause-Patent
// The APR packet header: its wire layout, its validation, and the one result shape every APR
// client needs. Kept out of the driver so a host test can pin the byte offsets without a device.
//
// APR is the protocol this board's DSP speaks. It is not GPR: the device tree selects the APR
// generation of the kona audio tree (kona-audio.dtsi, reached from alioth-sm8250.dtsi:8), while
// the AudioReach generation with qcom,gpr lives in kona-audio-ar.dtsi for other boards. Everything
// below comes from the vendor kernel under reference/k40-kernel/techpack/audio: the header struct
// and constants from include/ipc/apr.h, and the receive-side validation from ipc/apr.c:621-660.
// No MMIO and no IOKit here, so package/tests/apr_host.c compiles it with a host compiler.
#ifndef APR_CORE_H
#define APR_CORE_H
#include <stdint.h>
#include <stdbool.h>

// The header is five little-endian words. Its size is not padded by any compiler here, because
// every field is written into a byte buffer by hand: the same header must survive a device with a
// different alignment rule, and the DSP reads the bytes, not the struct.
enum {
  APR_HDR_BYTES = 20,
  APR_HDR_WORDS = 5,
  APR_MAX_BUF = 512,   // apr_tal.h:26, under CONFIG_MSM_QDSP6_APRV2_GLINK
};

// hdr_field[3:0], apr.h:45-46. Version 0 is what every client sends; the receiver also accepts 1
// (apr.c:629-632 rejects only a version above APR_PKT_VER + 1).
enum { APR_PKT_VER = 0 };

// hdr_field[9:8], apr.h:48-52.
enum {
  APR_MSG_EVENT = 0,
  APR_MSG_CMD_RSP = 1,
  APR_MSG_SEQ_CMD = 2,
  APR_MSG_NSEQ_CMD = 3,
  APR_MSG_MAX = 4,
};

// apr.h:59-64. src and dest each carry a domain and a service; the service id is the same on both
// sides for a client talking to its own service, which is what apr_send_pkt does (apr.c:395-397).
enum {
  APR_DOMAIN_SIM = 1, APR_DOMAIN_PC = 2, APR_DOMAIN_MODEM = 3,
  APR_DOMAIN_ADSP = 4, APR_DOMAIN_APPS = 5, APR_DOMAIN_MAX = 6,
};

// apr.h:67-80.
enum {
  APR_SVC_TEST_CLIENT = 0x02, APR_SVC_ADSP_CORE = 0x03, APR_SVC_AFE = 0x04,
  APR_SVC_VSM = 0x05, APR_SVC_VPM = 0x06, APR_SVC_ASM = 0x07, APR_SVC_ADM = 0x08,
  APR_SVC_ADSP_MVM = 0x09, APR_SVC_ADSP_CVS = 0x0A, APR_SVC_ADSP_CVP = 0x0B,
  APR_SVC_USM = 0x0C, APR_SVC_LSM = 0x0D, APR_SVC_VIDC = 0x16, APR_SVC_MAX = 0x17,
};

// apr.h:55-56. These are opcodes, not message types: a basic result arrives as an ordinary
// command response whose opcode is one of these, with a two-word {command, status} payload
// (q6core.c:267-282 rejects an empty or short payload).
enum { APR_BASIC_RSP_RESULT = 0x000110E8u, APR_RSP_ACCEPTED = 0x000100BEu };

// The fields as the DSP sees them. This is a decoded view, not a struct to cast over the wire.
typedef struct {
  uint16_t hdr_field, pkt_size;
  uint8_t src_svc, src_domain, dest_svc, dest_domain;
  uint16_t src_port, dest_port;
  uint32_t token, opcode;
} AprPacket;

// hdr_field is message type[9:8], header size in words[7:4], version[3:0] (apr.h:39-40).
static inline uint16_t apr_hdr_field(uint32_t msg_type, uint32_t hdr_words, uint32_t ver) {
  return (uint16_t)(((msg_type & 0x3u) << 8) | ((hdr_words & 0xFu) << 4) | (ver & 0xFu));
}
static inline uint32_t apr_msg_type(uint16_t hdr_field) { return (hdr_field >> 8) & 0x3u; }
static inline uint32_t apr_hdr_words(uint16_t hdr_field) { return (hdr_field >> 4) & 0xFu; }
static inline uint32_t apr_pkt_ver(uint16_t hdr_field) { return hdr_field & 0xFu; }

// Byte helpers. Little-endian on both sides: the DSP is not the one choosing, the shared memory
// format is.
static inline void apr_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void apr_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t apr_get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t apr_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Write one packet. The size is computed and written into pkt_size, so a caller cannot disagree
// with itself about it; every other field is taken from p. Returns the packet size, or 0 when it
// does not fit in APR_MAX_BUF or in capacity.
static inline uint32_t apr_encode(uint8_t *out, uint32_t capacity, const AprPacket *p,
                                  const void *payload, uint32_t payload_bytes) {
  if (!out || !p) return 0;
  if (payload_bytes > APR_MAX_BUF - APR_HDR_BYTES) return 0;
  if (payload_bytes && !payload) return 0;
  const uint32_t size = APR_HDR_BYTES + payload_bytes;
  if (size > capacity) return 0;
  apr_put16(out + 0, p->hdr_field);
  apr_put16(out + 2, (uint16_t)size);
  out[4] = p->src_svc;
  out[5] = p->src_domain;
  apr_put16(out + 6, p->src_port);
  out[8] = p->dest_svc;
  out[9] = p->dest_domain;
  apr_put16(out + 10, p->dest_port);
  apr_put32(out + 12, p->token);
  apr_put32(out + 16, p->opcode);
  const uint8_t *src = (const uint8_t *)payload;
  for (uint32_t i = 0; i < payload_bytes; ++i) out[APR_HDR_BYTES + i] = src[i];
  return size;
}

enum {
  AprOk = 0,
  AprShort = -1,      // shorter than a header, which the vendor receiver also refuses at exactly
                      // one header's length (apr.c:621: len <= APR_HDR_SIZE is an error)
  AprBadVersion = -2,
  AprBadHeader = -3,
  AprBadSize = -4,
  AprBadAddress = -5,
};

// Parse a received packet and locate its payload. The checks are the vendor receiver's own, in
// its order (apr.c:627-660): version, then header length, then packet size, then addresses. A
// caller that gets AprOk may read payload[0 .. *payload_bytes).
static inline int apr_decode(const uint8_t *in, uint32_t length, AprPacket *p,
                             const uint8_t **payload, uint32_t *payload_bytes) {
  if (!in || !p) return AprShort;
  if (length <= APR_HDR_BYTES) return AprShort;
  p->hdr_field = apr_get16(in + 0);
  p->pkt_size = apr_get16(in + 2);
  p->src_svc = in[4];
  p->src_domain = in[5];
  p->src_port = apr_get16(in + 6);
  p->dest_svc = in[8];
  p->dest_domain = in[9];
  p->dest_port = apr_get16(in + 10);
  p->token = apr_get32(in + 12);
  p->opcode = apr_get32(in + 16);
  if (apr_pkt_ver(p->hdr_field) > APR_PKT_VER + 1) return AprBadVersion;
  const uint32_t hdr_bytes = apr_hdr_words(p->hdr_field) * 4u;
  if (hdr_bytes < APR_HDR_BYTES) return AprBadHeader;
  if (p->pkt_size < APR_HDR_BYTES) return AprBadSize;
  if (p->pkt_size < hdr_bytes) return AprBadSize;
  if (p->pkt_size > length) return AprBadSize;
  if (p->src_domain >= APR_DOMAIN_MAX || p->dest_domain >= APR_DOMAIN_MAX) return AprBadAddress;
  if (p->src_svc >= APR_SVC_MAX || p->dest_svc >= APR_SVC_MAX) return AprBadAddress;
  if (payload) *payload = in + hdr_bytes;
  if (payload_bytes) *payload_bytes = p->pkt_size - hdr_bytes;
  return AprOk;
}

// A basic result's {command, status}. Returns 0 when the packet is one, and does not read the
// payload unless there are at least two words of it (q6core.c:271-282).
static inline int apr_basic_result(uint32_t opcode, const uint8_t *payload, uint32_t payload_bytes,
                                   uint32_t *command, uint32_t *status) {
  if (opcode != APR_BASIC_RSP_RESULT || !payload || payload_bytes < 8) return -1;
  if (command) *command = apr_get32(payload + 0);
  if (status) *status = apr_get32(payload + 4);
  return 0;
}

#endif
