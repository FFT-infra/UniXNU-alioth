// Host test of QcomGlink. It runs both ends of the link in one process: two QcomGlink instances
// share a pair of FIFOs, so one end's transmit is the other end's receive. That exercises the
// handshake, the channel open, the intent exchange, reassembly-free sends and the FIFO wraparound
// without a device. Build:
//
//   cc -std=c11 -Wall -Wextra -Werror -o glink_host package/tests/glink_host.c package/QcomGlink.c
//   ./glink_host
//
// What is NOT covered here: whether the DSP agrees to the same version, whether the real FIFO
// addresses and doorbell are right, and whether the other end ever answers. Those need the phone.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../QcomGlink.h"

#define FIFO_BYTES 4096
#define INTENT_SIZE 512
#define INTENT_COUNT 20
#define CHANNEL_NAME "apr_audio_svc"

// One FIFO in shared memory. Both ends point at the same words: the sender owns head, the
// receiver owns tail, which is exactly the contract QcomGlink assumes.
typedef struct {
  volatile uint32_t head, tail;
  volatile uint8_t data[FIFO_BYTES];
} FifoMem;

typedef struct {
  int kicks;
  int up;          // the last channel state the client was told
  int states;      // how many state callbacks arrived
  uint32_t messages;
  uint8_t got[INTENT_SIZE];
  uint32_t got_len;
} End;

static void on_kick(void *ctx) { ((End *)ctx)->kicks++; }
static void on_state(void *ctx, QcomGlinkChannel *c, int up) {
  (void)c;
  End *e = (End *)ctx;
  e->up = up;
  e->states++;
}
static void on_receive(void *ctx, QcomGlinkChannel *c, const uint8_t *data, uint32_t length) {
  (void)c;
  End *e = (End *)ctx;
  assert(length <= sizeof e->got);   // the channel's intents are 512 bytes
  memcpy(e->got, data, length);
  e->got_len = length;
  e->messages++;
}

static FifoMem ab, ba;   // a's transmit is b's receive, and the other way round
static QcomGlink ga, gb;
static End ea, eb;
static uint8_t storage_a[INTENT_SIZE * INTENT_COUNT];
static uint8_t storage_b[INTENT_SIZE * INTENT_COUNT];

// Poll both ends until neither has anything left to do. progress() may queue work without
// handled messages, so a fixed number of rounds is not enough on its own.
static void pump(void) {
  for (unsigned i = 0; i < 64; ++i) {
    const int a = qcom_glink_poll(&ga);
    const int b = qcom_glink_poll(&gb);
    assert(a != QcomGlinkCorrupt && b != QcomGlinkCorrupt);
    if (a <= 0 && b <= 0) return;
  }
  assert(0 && "the link never went quiet");
}

int main(void) {
  static const QcomGlinkClient client = {on_state, on_receive};

  const QcomGlinkFifo a_tx = {&ab.head, &ab.tail, ab.data, FIFO_BYTES};
  const QcomGlinkFifo a_rx = {&ba.head, &ba.tail, ba.data, FIFO_BYTES};
  const QcomGlinkFifo b_tx = {&ba.head, &ba.tail, ba.data, FIFO_BYTES};
  const QcomGlinkFifo b_rx = {&ab.head, &ab.tail, ab.data, FIFO_BYTES};
  qcom_glink_init(&ga, a_tx, a_rx, on_kick, &ea);
  qcom_glink_init(&gb, b_tx, b_rx, on_kick, &eb);

  // A fresh link: nothing to read anywhere, and the writable room is the whole FIFO less the
  // sender's own reserve (8 bytes plus 16 for a READ_NOTIF, QcomGlink.c FULL_RESERVE/BLOCKED_RESERVE).
  assert(qcom_glink_rx_avail(&ga.rx) == 0);
  assert(qcom_glink_rx_avail(&gb.rx) == 0);
  assert(qcom_glink_tx_avail(&ga.tx) == FIFO_BYTES - 24);

  // a opens the channel; b waits for the remote's OPEN, which is the side this board needs for a
  // channel the DSP owns (sheng registers adsp_apps with open_now 0 for the same reason).
  QcomGlinkChannel *cha = qcom_glink_channel(&ga, CHANNEL_NAME, INTENT_SIZE, INTENT_COUNT, storage_a, &client, &ea, 1);
  QcomGlinkChannel *chb = qcom_glink_channel(&gb, CHANNEL_NAME, INTENT_SIZE, INTENT_COUNT, storage_b, &client, &eb, 0);
  assert(cha && chb);

  // Before the handshake there is no channel to send on.
  assert(qcom_glink_send(&ga, cha, "x", 1) == QcomGlinkNotOpen);

  pump();
  assert(ea.up == 1 && eb.up == 1);
  assert(ea.states == 1 && eb.states == 1);
  assert(cha->up && chb->up);
  assert(cha->open_acked && cha->remote_open);
  assert(chb->open_acked && chb->remote_open);
  // Both directions ringed the doorbell at least once.
  assert(ea.kicks > 0 && eb.kicks > 0);
  // Our intents came back from the remote as sendable slots.
  assert(chb->remote[0].valid && chb->remote[0].size == INTENT_SIZE);
  assert(cha->remote[0].valid && cha->remote[0].size == INTENT_SIZE);

  // One message, byte for byte.
  {
    const char *text = "apr: hello";
    const uint32_t n = (uint32_t)strlen(text) + 1;
    assert(qcom_glink_send(&ga, cha, text, n) == QcomGlinkOk);
    pump();
    assert(eb.messages == 1);
    assert(eb.got_len == n);
    assert(memcmp(eb.got, text, n) == 0);
    assert(ea.messages == 0);
  }

  // A full intent's worth of payload, and the reverse direction, prove the intent id and the
  // remote channel id are both carried correctly in each direction.
  {
    uint8_t big[INTENT_SIZE];
    for (unsigned i = 0; i < sizeof big; ++i) big[i] = (uint8_t)(i * 7u + 1u);
    assert(qcom_glink_send(&gb, chb, big, sizeof big) == QcomGlinkOk);
    pump();
    assert(ea.messages == 1);
    assert(ea.got_len == INTENT_SIZE);
    assert(memcmp(ea.got, big, sizeof big) == 0);
    assert(eb.messages == 1);   // a's echo did not go back to b
  }

  // The FIFO wraps. Twenty sends of 400 bytes move 8000 bytes through a 4096-byte FIFO, so both
  // head and tail cross the end at least once while messages are in flight.
  {
    uint32_t received = 0;
    for (unsigned m = 0; m < 20; ++m) {
      uint8_t payload[400];
      memset(payload, (int)(m + 1), sizeof payload);
      assert(qcom_glink_send(&ga, cha, payload, sizeof payload) == QcomGlinkOk);
      pump();
      // Each send was delivered before the next one started, so the FIFO emptied in between; the
      // wraparound still happened because the byte offsets do not reset.
      assert(eb.got_len == sizeof payload);
      assert(eb.got[0] == (uint8_t)(m + 1));
      assert(eb.got[sizeof payload - 1] == (uint8_t)(m + 1));
      received += sizeof payload;
    }
    assert(received == 8000);
    assert(ab.head != 0 && ba.head != 0);
  }

  // One byte more than the remote's intent is not sent: no intent is big enough, the link asks
  // for one, and the remote cannot grant it (its own intents are the same size). Nothing is the
  // caller's problem but a retry later, and no partial message reaches the other end.
  {
    uint8_t over[INTENT_SIZE + 1];
    memset(over, 0x5A, sizeof over);
    assert(qcom_glink_send(&ga, cha, over, sizeof over) == QcomGlinkAgain);
    pump();
    assert(qcom_glink_send(&ga, cha, over, sizeof over) == QcomGlinkAgain);
  }

  // Larger than half the FIFO is refused outright rather than split: QcomGlink sends one message
  // as one chunk, so anything above half the ring cannot be guaranteed to fit.
  {
    static uint8_t huge[FIFO_BYTES];
    memset(huge, 0, sizeof huge);
    assert(qcom_glink_send(&ga, cha, huge, FIFO_BYTES / 2) == QcomGlinkTooBig);
  }

  // A message that cannot be a message stops the link instead of being skipped, and every later
  // poll reports it. This is the behaviour that keeps a corrupt stream from desynchronising.
  {
    const uint32_t at = ba.head;
    const uint16_t bad = 0x00FF;   // no such command
    ba.data[at] = (uint8_t)bad;
    ba.data[(at + 1) % FIFO_BYTES] = (uint8_t)(bad >> 8);
    for (unsigned i = 2; i < 8; ++i) ba.data[(at + i) % FIFO_BYTES] = 0;
    ba.head = (at + 8) % FIFO_BYTES;
    assert(qcom_glink_poll(&ga) == QcomGlinkCorrupt);
    assert(qcom_glink_poll(&ga) == QcomGlinkCorrupt);
    assert(qcom_glink_send(&ga, cha, "x", 1) == QcomGlinkNotOpen);
  }

  printf("glink_host: all assertions passed\n");
  return 0;
}
