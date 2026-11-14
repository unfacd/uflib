// RFC 6455 message reassembly — end-to-end through the frame codec, plus the
// §5.4 rules that decide which frames may follow which.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include <uflib/msg_framing/websocket/ws_frame.h>
#include <uflib/msg_framing/websocket/ws_message.h>
}

namespace {

const unsigned char kMask[4] = {0x21, 0x9f, 0x03, 0x7c};

using Bytes = std::vector<unsigned char>;

Bytes
Frame(WsOpcode opcode, const Bytes &payload, bool fin = true, bool masked = true)
{
    Bytes   out(payload.size() + WS_FRAME_MAX_OVERHEAD);
    ssize_t n = WsFrameEncode(out.data(), out.size(), payload.empty() ? nullptr : payload.data(),
                              payload.size(), opcode, fin ? 1 : 0, masked ? kMask : nullptr);
    EXPECT_GT(n, 0);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

/* One end-to-end driver: bytes on the wire in, message out. */
struct Stream {
    WsMessageAssembler asmb{};
    Bytes              message;   /* assembled message destination */
    Bytes              scratch;   /* per-frame decode destination */
    unsigned           flags = WS_FRAME_ACCEPT_ANY;

    explicit Stream(size_t cap, size_t limit = 0)
    {
        WsMessageAssemblerInit(&asmb, limit);
        message.resize(cap);
        scratch.resize(cap + WS_FRAME_MAX_OVERHEAD);
    }

    /* Feed one wire frame; returns the Consume() result. */
    int
    Feed(const Bytes &wire)
    {
        WsFrameInfo info{};
        size_t      consumed = 0;
        int rc = WsFrameDecode(wire.data(), wire.size(), flags, scratch.data(), scratch.size(),
                               &info, &consumed);
        if (rc != WS_FRAME_DECODED) return rc == WS_FRAME_NEED_MORE ? 100 : rc;

        return WsMessageAssemblerConsume(&asmb, &info, scratch.data(), message.data(),
                                         message.size());
    }

    Bytes
    Take() const
    {
        return Bytes(message.begin(), message.begin() + asmb.length);
    }
};

Bytes
RandomBytes(size_t n)
{
    Bytes    v(n);
    uint32_t x = 0x5eed1234u;
    for (size_t i = 0; i < n; i++) {
        x    = x * 1664525u + 1013904223u;
        v[i] = static_cast<unsigned char>(x >> 24);
    }
    return v;
}

} // namespace

/* ── Complete messages ──────────────────────────────────────────────────── */

TEST(WsMessage, SingleFrameIsAWholeMessage)
{
    Stream s(256);
    Bytes  payload = {'h', 'e', 'l', 'l', 'o'};

    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_BINARY, payload)));
    EXPECT_EQ(WS_MSG_STATE_COMPLETE, s.asmb.state);
    EXPECT_EQ(WS_OPCODE_BINARY, s.asmb.opcode);
    EXPECT_EQ(payload.size(), s.asmb.length);
    EXPECT_EQ(1u, s.asmb.fragments);
    EXPECT_EQ(payload, s.Take());
}

TEST(WsMessage, TwoFragmentsAssembleInOrder)
{
    Stream s(256);
    Bytes  a = {'a', 'b', 'c'};
    Bytes  b = {'d', 'e'};

    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, a, false)));
    EXPECT_EQ(WS_MSG_STATE_IN_PROGRESS, s.asmb.state);
    EXPECT_EQ(WS_OPCODE_TEXT, s.asmb.opcode);

    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, b, true)));
    EXPECT_EQ(Bytes({'a', 'b', 'c', 'd', 'e'}), s.Take());
    EXPECT_EQ(2u, s.asmb.fragments);
}

TEST(WsMessage, ManyFragmentsKeepTheirOrder)
{
    Stream s(4096);
    Bytes  expected;

    for (int i = 0; i < 10; i++) {
        Bytes part(100, static_cast<unsigned char>('a' + i));
        expected.insert(expected.end(), part.begin(), part.end());

        int rc = s.Feed(Frame(i == 0 ? WS_OPCODE_BINARY : WS_OPCODE_CONTINUATION, part, i == 9));
        EXPECT_EQ(i == 9 ? WS_MESSAGE_COMPLETE : WS_MESSAGE_INCOMPLETE, rc);
    }

    EXPECT_EQ(expected, s.Take());
    EXPECT_EQ(10u, s.asmb.fragments);
}

TEST(WsMessage, EmptyMessageIsLegal)
{
    Stream s(64);
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, {}, true)));
    EXPECT_EQ(0u, s.asmb.length);
}

TEST(WsMessage, EmptyFragmentsAssembleToEmptyMessage)
{
    Stream s(64);
    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, {}, false)));
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, {}, true)));
    EXPECT_EQ(0u, s.asmb.length);
}

TEST(WsMessage, FragmentedMessageBeyond64KiB)
{
    const size_t part_size = 70000;
    Stream       s(3 * part_size);
    Bytes        expected;

    for (int i = 0; i < 3; i++) {
        Bytes part = RandomBytes(part_size);
        expected.insert(expected.end(), part.begin(), part.end());
        int rc = s.Feed(Frame(i == 0 ? WS_OPCODE_BINARY : WS_OPCODE_CONTINUATION, part, i == 2));
        EXPECT_EQ(i == 2 ? WS_MESSAGE_COMPLETE : WS_MESSAGE_INCOMPLETE, rc);
    }

    EXPECT_EQ(expected, s.Take());
}

/* ── Control frames interleaved ─────────────────────────────────────────── */

TEST(WsMessage, ControlFramesInterleaveWithoutJoiningTheMessage)
{
    Stream s(256);

    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_BINARY, Bytes{'a'}, false)));
    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_PING, Bytes{'p'})));
    EXPECT_EQ(WS_MSG_STATE_IN_PROGRESS, s.asmb.state);
    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_PONG, Bytes{'p'})));
    EXPECT_EQ(WS_MSG_STATE_IN_PROGRESS, s.asmb.state);
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{'b'}, true)));

    EXPECT_EQ(Bytes({'a', 'b'}), s.Take());
}

/* ── §5.4 violations ────────────────────────────────────────────────────── */

TEST(WsMessage, ContinuationWithoutAMessageIsProtocolError)
{
    Stream s(64);

    EXPECT_EQ(WS_MESSAGE_ERR_PROTOCOL, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{'x'}, true)));
    EXPECT_EQ(WS_MSG_STATE_FAILED, s.asmb.state);

    /* sticky: a well-formed message afterwards is still refused */
    EXPECT_EQ(WS_MESSAGE_ERR_PROTOCOL, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'y'}, true)));
    EXPECT_EQ(WS_MSG_STATE_FAILED, s.asmb.state);
}

TEST(WsMessage, NewDataFrameDuringAFragmentedMessageIsProtocolError)
{
    Stream s(64);

    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_BINARY, Bytes{'a'}, false)));
    EXPECT_EQ(WS_MESSAGE_ERR_PROTOCOL, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'b'}, true)));
    EXPECT_EQ(WS_MSG_STATE_FAILED, s.asmb.state);
}

TEST(WsMessage, FailedAssemblerStaysFailedUntilReinitialised)
{
    Stream s(64);

    EXPECT_EQ(WS_MESSAGE_ERR_PROTOCOL, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{'x'}, true)));
    EXPECT_EQ(WS_MESSAGE_ERR_PROTOCOL, s.Feed(Frame(WS_OPCODE_PING, Bytes{'p'})));

    WsMessageAssemblerInit(&s.asmb, 0);
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'z'}, true)));
    EXPECT_EQ(Bytes({'z'}), s.Take());
}

/* ── Limits and capacity ────────────────────────────────────────────────── */

TEST(WsMessage, LimitRejectsAnOversizedMessage)
{
    Stream s(1024, 10);   /* limit 10, buffer large enough */

    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_BINARY, Bytes(6, 'a'), false)));
    EXPECT_EQ(WS_MESSAGE_ERR_CAPACITY, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes(6, 'b'), true)));
    EXPECT_EQ(WS_MSG_STATE_FAILED, s.asmb.state);
}

TEST(WsMessage, LimitAppliesToASingleFrameToo)
{
    Stream s(1024, 4);
    EXPECT_EQ(WS_MESSAGE_ERR_CAPACITY, s.Feed(Frame(WS_OPCODE_BINARY, Bytes(5, 'a'), true)));
}

TEST(WsMessage, ZeroLimitMeansBufferCapacity)
{
    Stream s(8, 0);
    EXPECT_EQ(WS_MESSAGE_ERR_CAPACITY, s.Feed(Frame(WS_OPCODE_BINARY, Bytes(9, 'a'), true)));
}

/* ── Streaming, masking and device roles ────────────────────────────────── */

TEST(WsMessage, MaskedClientFramesAreReassembledOnTheServer)
{
    Stream s(64);
    s.flags = WS_FRAME_REQUIRE_MASKED;

    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'a', 'b'}, false, true)));
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{'c'}, true, true)));
    EXPECT_EQ(Bytes({'a', 'b', 'c'}), s.Take());
}

TEST(WsMessage, UnmaskedServerFramesAreReassembledOnTheClient)
{
    Stream s(64);
    s.flags = WS_FRAME_REQUIRE_UNMASKED;

    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_BINARY, Bytes{'a'}, false, false)));
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{'b'}, true, false)));
    EXPECT_EQ(Bytes({'a', 'b'}), s.Take());
}

TEST(WsMessage, AssemblerResetsItselfAfterCompletion)
{
    Stream s(64);

    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'1'}, true)));
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'2'}, true)));
    EXPECT_EQ(Bytes({'2'}), s.Take());
    EXPECT_EQ(WS_OPCODE_TEXT, s.asmb.opcode);
}

/* ── Adversarial ────────────────────────────────────────────────────────── */

TEST(WsMessage, RejectsBadArguments)
{
    WsFrameInfo info{};
    info.opcode      = WS_OPCODE_BINARY;
    info.payload_len = 2;
    unsigned char payload[2] = {'a', 'b'};
    unsigned char message[8];

    EXPECT_EQ(WS_MESSAGE_ERR_ARGUMENT, WsMessageAssemblerConsume(nullptr, &info, payload, message,
                                                                 sizeof message));

    WsMessageAssembler a{};
    WsMessageAssemblerInit(&a, 0);
    EXPECT_EQ(WS_MESSAGE_ERR_ARGUMENT, WsMessageAssemblerConsume(&a, nullptr, payload, message,
                                                                 sizeof message));
    EXPECT_EQ(WS_MESSAGE_ERR_ARGUMENT, WsMessageAssemblerConsume(&a, &info, nullptr, message,
                                                                 sizeof message));
    EXPECT_EQ(WS_MESSAGE_ERR_ARGUMENT, WsMessageAssemblerConsume(&a, &info, payload, nullptr,
                                                                 sizeof message));
}

TEST(WsMessage, RandomFrameSequencesNeverBreakTheContract)
{
    uint32_t x = 0xabcdef01u;

    for (int iter = 0; iter < 500; iter++) {
        Stream s(4096, 8192);
        int    steps = 1 + (iter % 6);

        for (int step = 0; step < steps; step++) {
            x = x * 1664525u + 1013904223u;
            WsOpcode op = static_cast<WsOpcode>(
                (x >> 28) % 2 ? WS_OPCODE_CONTINUATION
                              : ((x >> 30) & 1 ? WS_OPCODE_BINARY : WS_OPCODE_TEXT));
            x          = x * 1664525u + 1013904223u;
            size_t len = (x >> 24) % 32u;
            bool   fin = (x >> 20) & 1;

            int rc = s.Feed(Frame(op, RandomBytes(len), fin, true));

            ASSERT_TRUE(rc == WS_MESSAGE_INCOMPLETE || rc == WS_MESSAGE_COMPLETE ||
                        rc == WS_MESSAGE_ERR_PROTOCOL || rc == WS_MESSAGE_ERR_CAPACITY ||
                        rc == WS_MESSAGE_ERR_ARGUMENT || rc == WS_MESSAGE_ERR_ENCODING)
                << "iteration " << iter;
            ASSERT_LE(s.asmb.length, s.message.size());
            if (s.asmb.state == WS_MSG_STATE_FAILED) break;
        }
    }
}

/* ── UTF-8 (§8.1) ───────────────────────────────────────────────────────── */

TEST(WsMessageUtf8, ValidMultibyteTextIsAccepted)
{
    Stream s(64);
    /* "héllo" — 2-byte é */
    Bytes text = {'h', 0xC3, 0xA9, 'l', 'l', 'o'};
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, text)));
    EXPECT_EQ(text, s.Take());
}

TEST(WsMessageUtf8, SequenceSplitAcrossFragmentsIsAccepted)
{
    Stream s(64);
    /* "€" = E2 82 AC, split after the first byte */
    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{0xE2}, false)));
    EXPECT_EQ(WS_MESSAGE_INCOMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{0x82}, false)));
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_CONTINUATION, Bytes{0xAC}, true)));
    EXPECT_EQ(Bytes({0xE2, 0x82, 0xAC}), s.Take());
}

TEST(WsMessageUtf8, TruncatedSequenceAtEndOfMessageIsRejected)
{
    Stream s(64);
    EXPECT_EQ(WS_MESSAGE_ERR_ENCODING, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{0xE2, 0x82}, true)));
    EXPECT_EQ(WS_MSG_STATE_FAILED, s.asmb.state);
}

TEST(WsMessageUtf8, InvalidSequencesAreRejected)
{
    const Bytes cases[] = {
        {0x80},                    /* lone continuation */
        {0xC0, 0xAF},              /* overlong 2-byte */
        {0xE0, 0x80, 0xAF},        /* overlong 3-byte */
        {0xED, 0xA0, 0x80},        /* surrogate */
        {0xF4, 0x90, 0x80, 0x80},  /* above U+10FFFF */
        {0xF5, 0x80, 0x80, 0x80},  /* invalid lead */
        {0xC2, 0x41},              /* continuation missing */
    };

    for (const Bytes &bad : cases) {
        Stream s(64);
        EXPECT_EQ(WS_MESSAGE_ERR_ENCODING, s.Feed(Frame(WS_OPCODE_TEXT, bad, true)))
            << "case length " << bad.size();
        EXPECT_EQ(WS_MSG_STATE_FAILED, s.asmb.state);
    }
}

TEST(WsMessageUtf8, BinaryPayloadsAreNotValidated)
{
    Stream s(64);
    Bytes  binary = {0x80, 0xC0, 0xFF};   /* invalid UTF-8, legal binary */
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_BINARY, binary, true)));
    EXPECT_EQ(binary, s.Take());
}

TEST(WsMessageUtf8, ValidatorResetsBetweenMessages)
{
    Stream s(64);
    EXPECT_EQ(WS_MESSAGE_ERR_ENCODING, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{0xE2}, true)));

    WsMessageAssemblerInit(&s.asmb, 0);
    EXPECT_EQ(WS_MESSAGE_COMPLETE, s.Feed(Frame(WS_OPCODE_TEXT, Bytes{'o', 'k'}, true)));
    EXPECT_EQ(Bytes({'o', 'k'}), s.Take());
}
