// Host test of AprCore.h. It checks the wire layout and the receiver's validation against the
// vendor kernel's own header and receive path, with no device involved. Build:
//
//   cc -std=c11 -Wall -Wextra -Werror -o apr_host package/tests/apr_host.c
//   ./apr_host
//
// What is NOT covered here: whether the DSP answers, what its status words mean, and whether the
// GLINK channel delivers the packet. Those need the phone.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../AprCore.h"

// The vendor's own header-field macro (apr.h:39-40), restated here so that apr_hdr_field() is
// checked against it rather than against itself.
static uint16_t vendor_hdr_field(uint32_t msg_type, uint32_t hdr_words, uint32_t ver) {
  return (uint16_t)(((msg_type & 0x3) << 8) | ((hdr_words & 0xF) << 4) | (ver & 0xF));
}

int main(void) {
  // The header's size is the sum of the fields the vendor struct declares, not a number anyone
  // chose: 2 + 2 + 1 + 1 + 2 + 1 + 1 + 2 + 4 + 4.
  assert(APR_HDR_BYTES == 2 + 2 + 1 + 1 + 2 + 1 + 1 + 2 + 4 + 4);
  assert(APR_HDR_WORDS * 4 == APR_HDR_BYTES);

  // hdr_field packing, against the vendor macro.
  for (uint32_t t = 0; t < APR_MSG_MAX; ++t)
    for (uint32_t w = 0; w <= 0xF; ++w)
      for (uint32_t v = 0; v <= 0xF; ++v)
        assert(apr_hdr_field(t, w, v) == vendor_hdr_field(t, w, v));
  assert(apr_hdr_field(APR_MSG_NSEQ_CMD, 5, 0) == 0x0350);   // 3 << 8 | 5 << 4 | 0
  assert(apr_hdr_field(APR_MSG_CMD_RSP, 5, 0) == 0x0150);
  assert(apr_msg_type(0x0350) == APR_MSG_NSEQ_CMD);
  assert(apr_hdr_words(0x0350) == 5);
  assert(apr_pkt_ver(0x0350) == 0);

  // The bytes, at the offsets the vendor struct puts them. A packet with a two-word payload.
  AprPacket p = {
      .hdr_field = apr_hdr_field(APR_MSG_NSEQ_CMD, APR_HDR_WORDS, APR_PKT_VER),
      .pkt_size = 0,  // ignored on encode; the encoder computes it
      .src_svc = APR_SVC_AFE,
      .src_domain = APR_DOMAIN_APPS,
      .dest_svc = APR_SVC_AFE,
      .dest_domain = APR_DOMAIN_ADSP,
      .src_port = 0x1234,
      .dest_port = 0xB001,   // AFE_PORT_ID_WSA_CODEC_DMA_TX_0, the worked example in the record
      .token = 0x11223344,
      .opcode = APR_BASIC_RSP_RESULT,
  };
  const uint32_t payload[2] = {0xAABBCCDD, 0x00000000};
  uint8_t out[64];
  memset(out, 0xEE, sizeof out);
  const uint32_t size = apr_encode(out, sizeof out, &p, payload, sizeof payload);
  assert(size == 28);
  assert(apr_get16(out + 0) == 0x0350);          // hdr_field
  assert(apr_get16(out + 2) == 28);              // pkt_size is the whole packet
  assert(out[4] == APR_SVC_AFE);                 // src_svc
  assert(out[5] == APR_DOMAIN_APPS);             // src_domain
  assert(apr_get16(out + 6) == 0x1234);          // src_port
  assert(out[8] == APR_SVC_AFE);                 // dest_svc
  assert(out[9] == APR_DOMAIN_ADSP);             // dest_domain
  assert(apr_get16(out + 10) == 0xB001);         // dest_port
  assert(apr_get32(out + 12) == 0x11223344);     // token
  assert(apr_get32(out + 16) == APR_BASIC_RSP_RESULT);
  assert(apr_get32(out + 20) == 0xAABBCCDD);     // payload starts right after the 20-byte header
  assert(apr_get32(out + 24) == 0x00000000);

  // Decode takes the packet back apart, field for field.
  AprPacket d;
  const uint8_t *body = NULL;
  uint32_t body_bytes = 0;
  assert(apr_decode(out, size, &d, &body, &body_bytes) == AprOk);
  assert(d.hdr_field == p.hdr_field && d.pkt_size == 28);
  assert(d.src_svc == p.src_svc && d.src_domain == p.src_domain);
  assert(d.dest_svc == p.dest_svc && d.dest_domain == p.dest_domain);
  assert(d.src_port == 0x1234 && d.dest_port == 0xB001);
  assert(d.token == 0x11223344 && d.opcode == APR_BASIC_RSP_RESULT);
  assert(body == out + APR_HDR_BYTES);
  assert(body_bytes == 8);

  // A header-only packet is refused, and that is the vendor receiver's rule, not a choice here:
  // apr.c:621 rejects len <= APR_HDR_SIZE before it looks at any field.
  assert(apr_decode(out, APR_HDR_BYTES, &d, &body, &body_bytes) == AprShort);
  assert(apr_decode(out, APR_HDR_BYTES - 1, &d, &body, &body_bytes) == AprShort);

  // Version: 0 and 1 pass (apr.c:629 accepts up to APR_PKT_VER + 1), 2 does not.
  {
    uint8_t q[64];
    AprPacket v = p;
    v.hdr_field = apr_hdr_field(APR_MSG_NSEQ_CMD, 5, 1);
    uint32_t n = apr_encode(q, sizeof q, &v, payload, sizeof payload);
    assert(apr_decode(q, n, &d, &body, &body_bytes) == AprOk);
    v.hdr_field = apr_hdr_field(APR_MSG_NSEQ_CMD, 5, 2);
    n = apr_encode(q, sizeof q, &v, payload, sizeof payload);
    assert(apr_decode(q, n, &d, &body, &body_bytes) == AprBadVersion);
  }

  // Header size below one header: four words is 16 bytes.
  {
    AprPacket v = p;
    v.hdr_field = apr_hdr_field(APR_MSG_NSEQ_CMD, 4, 0);
    assert(apr_encode(out, sizeof out, &v, payload, sizeof payload) == 28);
    assert(apr_decode(out, 28, &d, &body, &body_bytes) == AprBadHeader);
  }

  // Packet size: below a header, below the declared header, and longer than what arrived.
  {
    AprPacket v = p;  // hdr_field says five words = 20 bytes
    apr_encode(out, sizeof out, &v, payload, sizeof payload);
    apr_put16(out + 2, 19);
    assert(apr_decode(out, 28, &d, &body, &body_bytes) == AprBadSize);
    apr_put16(out + 2, 28);
    assert(apr_decode(out, 27, &d, &body, &body_bytes) == AprBadSize);   // declared longer than received
    v.hdr_field = apr_hdr_field(APR_MSG_NSEQ_CMD, 6, 0);                // declares 24 bytes of header
    apr_encode(out, sizeof out, &v, payload, sizeof payload);
    apr_put16(out + 2, 20);                                             // but claims 20 total
    assert(apr_decode(out, 28, &d, &body, &body_bytes) == AprBadSize);
  }

  // Addresses: a domain at or above APR_DOMAIN_MAX, a service at or above APR_SVC_MAX.
  {
    uint8_t q[64];
    AprPacket v = p;
    v.dest_domain = APR_DOMAIN_MAX;
    uint32_t n = apr_encode(q, sizeof q, &v, payload, sizeof payload);
    assert(apr_decode(q, n, &d, &body, &body_bytes) == AprBadAddress);
    v = p;
    v.src_svc = APR_SVC_MAX;
    n = apr_encode(q, sizeof q, &v, payload, sizeof payload);
    assert(apr_decode(q, n, &d, &body, &body_bytes) == AprBadAddress);
    v = p;
    v.dest_svc = APR_SVC_MAX - 1;   // VIDC is the highest legal service
    n = apr_encode(q, sizeof q, &v, payload, sizeof payload);
    assert(apr_decode(q, n, &d, &body, &body_bytes) == AprOk);
  }

  // The buffer limit: 512 bytes of packet, so 492 of payload.
  {
    uint8_t big[APR_MAX_BUF];      // the largest legal payload
    uint8_t packet[APR_MAX_BUF];   // and the packet that holds it
    memset(big, 0, sizeof big);
    assert(apr_encode(packet, sizeof packet, &p, big, APR_MAX_BUF - APR_HDR_BYTES) == APR_MAX_BUF);
    assert(apr_encode(packet, sizeof packet, &p, big, APR_MAX_BUF - APR_HDR_BYTES + 1) == 0);
    uint8_t tiny[16];
    assert(apr_encode(tiny, sizeof tiny, &p, payload, sizeof payload) == 0);  // capacity too small
  }

  // The basic result, which is the only shape every client must parse.
  {
    uint32_t command = 0, status = 0;
    assert(apr_basic_result(APR_BASIC_RSP_RESULT, (const uint8_t *)payload, 8, &command, &status) == 0);
    assert(command == 0xAABBCCDD && status == 0);
    assert(apr_basic_result(APR_BASIC_RSP_RESULT, (const uint8_t *)payload, 4, &command, &status) == -1);
    assert(apr_basic_result(APR_BASIC_RSP_RESULT, NULL, 8, &command, &status) == -1);
    assert(apr_basic_result(APR_RSP_ACCEPTED, (const uint8_t *)payload, 8, &command, &status) == -1);
  }

  // The opcode values themselves, so a typo cannot pass as a protocol.
  assert(APR_BASIC_RSP_RESULT == 0x000110E8u);
  assert(APR_RSP_ACCEPTED == 0x000100BEu);
  assert(APR_DOMAIN_APPS == 5 && APR_DOMAIN_ADSP == 4);
  assert(APR_SVC_AFE == 4 && APR_SVC_ASM == 7 && APR_SVC_VIDC == 0x16);

  printf("apr_host: all assertions passed\n");
  return 0;
}
