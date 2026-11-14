// RFC 6455 frame codec — functional, boundary and adversarial coverage.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include <uflib/msg_framing/websocket/ws_frame.h>
}

namespace {

const unsigned char kMask[4] = {0x37, 0xfa, 0x21, 0x3d};

std::vector<unsigned char>
Encode(WsOpcode opcode, const std::vector<unsigned char> &payload, bool fin = true,
       const unsigned char *mask = nullptr)
{
    std::vector<unsigned char> out(payload.size() + 16);
    ssize_t n = WsFrameEncode(out.data(), out.size(), payload.empty() ? nullptr : payload.data(),
                              payload.size(), opcode, fin ? 1 : 0, mask);
    EXPECT_GT(n, 0);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

struct Decoded {
    int                rc = 0;
    WsFrameInfo        info{};
    std::vector<unsigned char> payload;
    size_t             consumed = 0;
};

Decoded
Decode(const std::vector<unsigned char> &frame, unsigned flags = WS_FRAME_ACCEPT_ANY,
       size_t cap_hint = 0)
{
    Decoded d;
    size_t  cap = cap_hint ? cap_hint : frame.size() + 1;
    d.payload.resize(cap);
    d.rc = WsFrameDecode(frame.data(), frame.size(), flags,
                         d.payload.empty() ? nullptr : d.payload.data(), d.payload.size(),
                         &d.info, &d.consumed);
    if (d.rc == WS_FRAME_DECODED) d.payload.resize(d.info.payload_len);
    return d;
}

std::vector<unsigned char>
RandomBytes(size_t n)
{
    std::vector<unsigned char> v(n);
    uint32_t                   x = 0x12345678u;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        v[i] = static_cast<unsigned char>(x >> 24);
    }
    return v;
}

} // namespace

/* ── Encoding ───────────────────────────────────────────────────────────── */

TEST(WsFrameEncode, HeaderFormFollowsPayloadLength)
{
    EXPECT_EQ(14u, WS_FRAME_MAX_OVERHEAD);

    auto empty = Encode(WS_OPCODE_BINARY, {});
    EXPECT_EQ(2u, empty.size());

    auto p125 = std::vector<unsigned char>(125, 0x41);
    auto f125 = Encode(WS_OPCODE_BINARY, p125);
    EXPECT_EQ(125u, f125.size() - 2u);
    EXPECT_EQ(0x7d, f125[1] & 0x7f);         /* no extended length */

    auto p126 = std::vector<unsigned char>(126, 0x41);
    auto f126 = Encode(WS_OPCODE_BINARY, p126);
    EXPECT_EQ(126, f126[1] & 0x7f);
    EXPECT_EQ(0x00, f126[2]);
    EXPECT_EQ(0x7e, f126[3]);

    auto p65536 = std::vector<unsigned char>(65536, 0x41);
    auto f65536 = Encode(WS_OPCODE_BINARY, p65536);
    EXPECT_EQ(127, f65536[1] & 0x7f);
    EXPECT_EQ(10u + 65536u, f65536.size());
}

TEST(WsFrameEncode, MaskedPayloadIsXored)
{
    std::vector<unsigned char> payload = {'H', 'e', 'l', 'l', 'o'};
    auto frame = Encode(WS_OPCODE_BINARY, payload, true, kMask);

    ASSERT_EQ(2u + 4u + payload.size(), frame.size());
    EXPECT_EQ(0x80u, frame[1] & 0x80u);
    EXPECT_EQ(kMask[0], frame[2]);
    for (size_t i = 0; i < payload.size(); i++)
        EXPECT_EQ(payload[i] ^ kMask[i % 4], frame[6 + i]);
}

TEST(WsFrameEncode, RejectsControlFrameViolations)
{
    std::vector<unsigned char> big(126, 'x');
    std::vector<unsigned char> out(256);

    EXPECT_EQ(-1, WsFrameEncode(out.data(), out.size(), big.data(), big.size(),
                                WS_OPCODE_PING, 1, nullptr));
    EXPECT_EQ(-1, WsFrameEncode(out.data(), out.size(), nullptr, 0,
                                WS_OPCODE_PING, 0, nullptr));   /* fragmented control */
    EXPECT_EQ(-1, WsFrameEncode(out.data(), out.size(), nullptr, 0,
                                WS_OPCODE_CLOSE, 0, nullptr));
}

TEST(WsFrameEncode, RejectsBadArguments)
{
    unsigned char out[64] = {};
    unsigned char one     = 'x';

    EXPECT_EQ(-1, WsFrameEncode(nullptr, 64, &one, 1, WS_OPCODE_BINARY, 1, nullptr));
    EXPECT_EQ(-1, WsFrameEncode(out, sizeof out, nullptr, 1, WS_OPCODE_BINARY, 1, nullptr));
    EXPECT_EQ(-1, WsFrameEncode(out, sizeof out, &one, 1, static_cast<WsOpcode>(0x3), 1, nullptr));
    EXPECT_EQ(-1, WsFrameEncode(out, sizeof out, &one, 1, static_cast<WsOpcode>(0xB), 1, nullptr));
    EXPECT_EQ(-1, WsFrameEncode(out, 2, &one, 1, WS_OPCODE_BINARY, 1, nullptr)); /* cap too small */
}

/* ── Decoding ───────────────────────────────────────────────────────────── */

TEST(WsFrameDecode, RoundTripsEveryOpcode)
{
    const WsOpcode ops[] = {WS_OPCODE_TEXT, WS_OPCODE_BINARY, WS_OPCODE_CONTINUATION,
                            WS_OPCODE_PING, WS_OPCODE_PONG, WS_OPCODE_CLOSE};

    for (WsOpcode op : ops) {
        std::vector<unsigned char> payload = {'a', 'b', 'c'};
        auto                       frame   = Encode(op, payload);
        Decoded                    d       = Decode(frame);

        ASSERT_EQ(WS_FRAME_DECODED, d.rc);
        EXPECT_EQ(op, d.info.opcode);
        EXPECT_TRUE(d.info.fin);
        EXPECT_FALSE(d.info.masked);
        EXPECT_EQ(payload, d.payload);
        EXPECT_EQ(frame.size(), d.consumed);
    }
}

TEST(WsFrameDecode, RoundTripsMaskedFrames)
{
    std::vector<unsigned char> payload = {'m', 'a', 's', 'k', 'e', 'd'};
    auto                       frame   = Encode(WS_OPCODE_BINARY, payload, true, kMask);
    Decoded                    d       = Decode(frame, WS_FRAME_REQUIRE_MASKED);

    ASSERT_EQ(WS_FRAME_DECODED, d.rc);
    EXPECT_TRUE(d.info.masked);
    EXPECT_EQ(payload, d.payload);
}

TEST(WsFrameDecode, HandlesPayloadsBeyond64KiB)
{
    for (size_t size : {size_t{126}, size_t{65535}, size_t{65536}, size_t{70000}, size_t{200000}}) {
        auto payload = RandomBytes(size);
        auto frame   = Encode(WS_OPCODE_BINARY, payload);
        Decoded d    = Decode(frame);

        ASSERT_EQ(WS_FRAME_DECODED, d.rc) << "size " << size;
        EXPECT_EQ(size, d.info.payload_len);
        EXPECT_EQ(payload, d.payload) << "size " << size;
    }
}

TEST(WsFrameDecode, NeedsMoreWhenTruncated)
{
    std::vector<unsigned char> payload(300, 'x');
    auto                       frame = Encode(WS_OPCODE_BINARY, payload);

    std::vector<unsigned char> one(frame.begin(), frame.begin() + 1);
    Decoded                    d = Decode(one);
    EXPECT_EQ(WS_FRAME_NEED_MORE, d.rc);

    const size_t room = 512;   /* room for the payload: truncation is the only finding */

    std::vector<unsigned char> header_only(frame.begin(), frame.begin() + 4);
    EXPECT_EQ(WS_FRAME_NEED_MORE, Decode(header_only, WS_FRAME_ACCEPT_ANY, room).rc);

    std::vector<unsigned char> missing_tail(frame.begin(), frame.end() - 1);
    EXPECT_EQ(WS_FRAME_NEED_MORE, Decode(missing_tail, WS_FRAME_ACCEPT_ANY, room).rc);

    /* masked frame whose mask key is not fully present */
    auto masked  = Encode(WS_OPCODE_BINARY, payload, true, kMask);
    std::vector<unsigned char> no_key(masked.begin(), masked.begin() + 4);
    EXPECT_EQ(WS_FRAME_NEED_MORE, Decode(no_key, WS_FRAME_ACCEPT_ANY, room).rc);
}

TEST(WsFrameDecode, ConsumesOneFrameAtATime)
{
    auto first  = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'1', '2'});
    auto second = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'3', '4'});

    std::vector<unsigned char> both = first;
    both.insert(both.end(), second.begin(), second.end());

    WsFrameInfo   info{};
    size_t        consumed = 0;
    unsigned char out[16];

    ASSERT_EQ(WS_FRAME_DECODED,
              WsFrameDecode(both.data(), both.size(), 0, out, sizeof out, &info, &consumed));
    EXPECT_EQ(first.size(), consumed);
    EXPECT_EQ(std::vector<unsigned char>({'1', '2'}),
              std::vector<unsigned char>(out, out + info.payload_len));

    ASSERT_EQ(WS_FRAME_DECODED,
              WsFrameDecode(both.data() + consumed, both.size() - consumed, 0, out, sizeof out,
                            &info, &consumed));
    EXPECT_EQ(std::vector<unsigned char>({'3', '4'}),
              std::vector<unsigned char>(out, out + info.payload_len));
}

TEST(WsFrameDecode, RejectsRsv1WithItsOwnCode)
{
    auto frame = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'x'});
    frame[0] |= 0x40u;                                  /* RSV1: compression bit */
    EXPECT_EQ(WS_FRAME_ERR_RESERVED, Decode(frame).rc);
}

TEST(WsFrameDecode, RejectsRsv2AndRsv3AsProtocolErrors)
{
    auto frame = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'x'});
    for (unsigned char rsv : {0x20u, 0x10u}) {
        auto bad = frame;
        bad[0] |= rsv;
        EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(bad).rc);
    }
}

TEST(WsFrameDecode, ReportsRsv1AsClearOnAcceptedFrames)
{
    auto frame = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'x'});
    Decoded d  = Decode(frame);
    ASSERT_EQ(WS_FRAME_DECODED, d.rc);
    EXPECT_EQ(0, d.info.rsv1);

    /* and on the capacity path, where the descriptor is filled for sizing */
    WsFrameInfo info{};
    size_t      consumed = 0;
    unsigned char small[1];
    unsigned char big_header[] = {0x82, 0x7e, 0x10, 0x00};
    ASSERT_EQ(WS_FRAME_ERR_CAPACITY,
              WsFrameDecode(big_header, sizeof big_header, 0, small, sizeof small, &info, &consumed));
    EXPECT_EQ(0, info.rsv1);
}

TEST(WsFrameDecode, RejectsUnknownOpcodes)
{
    auto frame = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'x'});
    for (unsigned char op : {0x3u, 0x4u, 0x5u, 0x6u, 0x7u, 0xBu, 0xCu, 0xDu, 0xEu, 0xFu}) {
        auto bad = frame;
        bad[0] = static_cast<unsigned char>((bad[0] & 0xf0u) | op);
        EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(bad).rc);
    }
}

TEST(WsFrameDecode, RejectsIllegalControlFrames)
{
    /* control frame declaring 126 bytes: header form 4, must be refused */
    std::vector<unsigned char> frame = {0x89, 0x7e, 0x00, 0x7e};
    frame.resize(4 + 126);
    EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(frame).rc);

    /* fragmented control frame, built by hand: ping with FIN clear */
    std::vector<unsigned char> fragmented = {0x09, 0x01, 'p'};
    EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(fragmented).rc);
}

TEST(WsFrameDecode, Rejects64BitLengthWithHighBitSet)
{
    std::vector<unsigned char> frame = {0x82, 0x7f, 0x80, 0, 0, 0, 0, 0, 0, 1};
    frame.resize(frame.size() + 8);
    EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(frame).rc);
}

TEST(WsFrameDecode, EnforcesMaskingPolicy)
{
    auto unmasked = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'x'});
    auto masked   = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'x'}, true, kMask);

    EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(unmasked, WS_FRAME_REQUIRE_MASKED).rc);
    EXPECT_EQ(WS_FRAME_ERR_PROTOCOL, Decode(masked, WS_FRAME_REQUIRE_UNMASKED).rc);
    EXPECT_EQ(WS_FRAME_DECODED, Decode(masked, WS_FRAME_ACCEPT_ANY).rc);
    EXPECT_NE(WS_FRAME_REQUIRE_MASKED, WS_FRAME_REQUIRE_UNMASKED);
}

TEST(WsFrameDecode, ReportsCapacityShortfall)
{
    std::vector<unsigned char> payload(200, 'x');
    auto                       frame = Encode(WS_OPCODE_BINARY, payload);
    Decoded                    d     = Decode(frame, WS_FRAME_ACCEPT_ANY, 64);
    EXPECT_EQ(WS_FRAME_ERR_CAPACITY, d.rc);
}

TEST(WsFrameDecode, RejectsNullArguments)
{
    WsFrameInfo   info{};
    size_t        consumed = 0;
    unsigned char out[8];

    EXPECT_EQ(WS_FRAME_ERR_ARGUMENT, WsFrameDecode(nullptr, 2, 0, out, sizeof out, &info, &consumed));
    EXPECT_EQ(WS_FRAME_ERR_ARGUMENT, WsFrameDecode(out, 2, 0, out, sizeof out, nullptr, &consumed));
    EXPECT_EQ(WS_FRAME_ERR_ARGUMENT, WsFrameDecode(out, 2, 0, out, sizeof out, &info, nullptr));
    EXPECT_EQ(WS_FRAME_ERR_ARGUMENT,
              WsFrameDecode(out, 2, WS_FRAME_REQUIRE_MASKED | WS_FRAME_REQUIRE_UNMASKED, out,
                            sizeof out, &info, &consumed));
    EXPECT_EQ(WS_FRAME_ERR_ARGUMENT, WsFrameDecode(out, 2, 0x80u, out, sizeof out, &info, &consumed));
}

TEST(WsFrameDecode, AcceptsEmptyPayloads)
{
    auto        frame    = Encode(WS_OPCODE_BINARY, {});
    WsFrameInfo info{};
    size_t      consumed = 0;

    ASSERT_EQ(WS_FRAME_DECODED,
              WsFrameDecode(frame.data(), frame.size(), 0, nullptr, 0, &info, &consumed));
    EXPECT_EQ(0u, info.payload_len);
    EXPECT_EQ(2u, consumed);
}

/* ── Close payloads ─────────────────────────────────────────────────────── */

TEST(WsClosePayload, RoundTripsCodeAndReason)
{
    unsigned char buf[128];
    ssize_t       n = WsClosePayloadEncode(buf, sizeof buf, 1000, "bye");
    ASSERT_EQ(5, n);

    unsigned short code   = 0;
    char           reason[16] = {};
    size_t         reason_len = 0;
    ASSERT_EQ(0, WsClosePayloadDecode(buf, static_cast<size_t>(n), &code, reason, sizeof reason,
                                      &reason_len));
    EXPECT_EQ(1000, code);
    EXPECT_EQ(3u, reason_len);
    EXPECT_STREQ("bye", reason);
}

TEST(WsClosePayload, EmptyPayloadMeansNoStatus)
{
    unsigned short code = 0;
    size_t         reason_len = 99;

    EXPECT_EQ(0, WsClosePayloadDecode(nullptr, 0, &code, nullptr, 0, &reason_len));
    EXPECT_EQ(1005, code);
    EXPECT_EQ(0u, reason_len);
}

TEST(WsClosePayload, RejectsMalformed)
{
    unsigned short code       = 0;
    size_t         reason_len = 0;
    unsigned char  one[]      = {0x03};

    EXPECT_EQ(-1, WsClosePayloadDecode(one, 1, &code, nullptr, 0, &reason_len));

    for (unsigned short bad : {999, 1004, 1005, 1006, 1015, 2999, 5000}) {
        unsigned char p[] = {static_cast<unsigned char>(bad >> 8), static_cast<unsigned char>(bad)};
        EXPECT_EQ(-1, WsClosePayloadDecode(p, 2, &code, nullptr, 0, &reason_len)) << bad;
    }
}

TEST(WsClosePayload, RejectsOverLongReason)
{
    std::string long_reason(124, 'r');
    unsigned char buf[256];
    EXPECT_EQ(-1, WsClosePayloadEncode(buf, sizeof buf, 1000, long_reason.c_str()));

    std::string max_reason(123, 'r');
    EXPECT_EQ(125, WsClosePayloadEncode(buf, sizeof buf, 1000, max_reason.c_str()));
}

TEST(WsClosePayload, RejectsNonUtf8Reason)
{
    unsigned short code       = 0;
    size_t         reason_len = 0;

    /* status 1000 followed by a lone continuation byte */
    unsigned char bad[] = {0x03, 0xE8, 0x80};
    EXPECT_EQ(-1, WsClosePayloadDecode(bad, sizeof bad, &code, nullptr, 0, &reason_len));

    /* status 1000 followed by a truncated 3-byte sequence */
    unsigned char truncated[] = {0x03, 0xE8, 0xE2, 0x82};
    EXPECT_EQ(-1, WsClosePayloadDecode(truncated, sizeof truncated, &code, nullptr, 0, &reason_len));

    /* status 1000 followed by valid UTF-8 */
    unsigned char good[] = {0x03, 0xE8, 0xC3, 0xA9};
    EXPECT_EQ(0, WsClosePayloadDecode(good, sizeof good, &code, nullptr, 0, &reason_len));
    EXPECT_EQ(1000, code);
    EXPECT_EQ(2u, reason_len);
}

TEST(WsClosePayload, FitsInsideACloseFrame)
{
    unsigned char payload[128];
    ssize_t       plen = WsClosePayloadEncode(payload, sizeof payload, 1001, "going away");
    ASSERT_GT(plen, 0);

    std::vector<unsigned char> out(256);
    ssize_t frame_len = WsFrameEncode(out.data(), out.size(), payload, static_cast<size_t>(plen),
                                      WS_OPCODE_CLOSE, 1, kMask);
    ASSERT_GT(frame_len, 0);
    out.resize(static_cast<size_t>(frame_len));

    Decoded d = Decode(out, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, d.rc);
    EXPECT_EQ(WS_OPCODE_CLOSE, d.info.opcode);

    unsigned short code = 0;
    char           reason[32] = {};
    size_t         reason_len = 0;
    ASSERT_EQ(0, WsClosePayloadDecode(d.payload.data(), d.payload.size(), &code, reason,
                                      sizeof reason, &reason_len));
    EXPECT_EQ(1001, code);
    EXPECT_STREQ("going away", reason);
}

/* ── Adversarial ────────────────────────────────────────────────────────── */

namespace {

bool
OutcomeIsDefined(int rc)
{
    return rc == WS_FRAME_DECODED || rc == WS_FRAME_NEED_MORE || rc == WS_FRAME_ERR_PROTOCOL ||
           rc == WS_FRAME_ERR_CAPACITY || rc == WS_FRAME_ERR_ARGUMENT ||
           rc == WS_FRAME_ERR_RESERVED;
}

} // namespace

TEST(WsFrameAdversarial, FlippedBytesNeverBreakTheContract)
{
    auto frame = Encode(WS_OPCODE_BINARY, std::vector<unsigned char>{'p', 'a', 'y', 'l', 'o', 'a', 'd'},
                        true, kMask);
    const unsigned char values[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    std::vector<unsigned char> out(frame.size() + 16);

    for (size_t i = 0; i < frame.size(); i++) {
        for (unsigned char v : values) {
            auto bad = frame;
            bad[i]   = v;

            WsFrameInfo info{};
            size_t      consumed = 0;
            int rc = WsFrameDecode(bad.data(), bad.size(), WS_FRAME_ACCEPT_ANY, out.data(),
                                   out.size(), &info, &consumed);

            EXPECT_TRUE(OutcomeIsDefined(rc)) << "byte " << i << " value " << int(v);
            if (rc == WS_FRAME_DECODED) {
                EXPECT_LE(consumed, bad.size());
                EXPECT_LE(info.payload_len, out.size());
            }
        }
    }
}

TEST(WsFrameAdversarial, RandomBuffersNeverBreakTheContract)
{
    uint32_t                   x = 0xdeadbeefu;
    std::vector<unsigned char> out(512);

    for (int iter = 0; iter < 2000; iter++) {
        size_t len = 0;
        x          = x * 1664525u + 1013904223u;
        len        = (x >> 24) % 64u;

        std::vector<unsigned char> buf(len);
        for (size_t i = 0; i < len; i++) {
            x    = x * 1664525u + 1013904223u;
            buf[i] = static_cast<unsigned char>(x >> 16);
        }

        WsFrameInfo info{};
        size_t      consumed = 0;
        int rc = WsFrameDecode(buf.data(), buf.size(), WS_FRAME_ACCEPT_ANY, out.data(), out.size(),
                               &info, &consumed);

        ASSERT_TRUE(OutcomeIsDefined(rc)) << "iteration " << iter;
        if (rc == WS_FRAME_DECODED) {
            ASSERT_LE(consumed, buf.size());
            ASSERT_LE(info.payload_len, out.size());
        }
    }
}

TEST(WsFrameAdversarial, OversizedFrameIsRefusedBeforeBuffering)
{
    /* 16-bit length of 4096, only the header present, destination of 64:
     * the caller must learn "too big" now, not after buffering 4 KiB. */
    unsigned char frame[] = {0x82, 0x7e, 0x10, 0x00};
    unsigned char out[64];
    WsFrameInfo   info{};
    size_t        consumed = 0;

    ASSERT_EQ(WS_FRAME_ERR_CAPACITY,
              WsFrameDecode(frame, sizeof frame, 0, out, sizeof out, &info, &consumed));
    EXPECT_EQ(4096u, info.payload_len);   /* the size the caller would need */
    EXPECT_EQ(4u, info.header_len);

    /* the same frame with room to spare is incomplete, not an error */
    unsigned char roomy[4096];
    EXPECT_EQ(WS_FRAME_NEED_MORE,
              WsFrameDecode(frame, sizeof frame, 0, roomy, sizeof roomy, &info, &consumed));
}

TEST(WsFrameAdversarial, HugeDeclaredLengthIsRefusedWithTheSizeItNeeds)
{
    /* 64-bit length of 2^40, only the header present: refused on the spot so a
     * hostile length cannot make the caller buffer unboundedly. */
    unsigned char frame[10] = {0x82, 0x7f, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char out[8];

    WsFrameInfo info{};
    size_t      consumed = 0;
    ASSERT_EQ(WS_FRAME_ERR_CAPACITY,
              WsFrameDecode(frame, sizeof frame, 0, out, sizeof out, &info, &consumed));
    EXPECT_EQ((size_t)1 << 40, info.payload_len);
}

TEST(WsFrameAdversarial, MaximalControlLengthIsRefusedBeforeCopying)
{
    /* ping claiming 125 bytes of payload but carrying none */
    unsigned char frame[2] = {0x89, 0x7d};
    unsigned char out[125];

    WsFrameInfo info{};
    size_t      consumed = 0;
    EXPECT_EQ(WS_FRAME_NEED_MORE,
              WsFrameDecode(frame, sizeof frame, 0, out, sizeof out, &info, &consumed));

    /* and a non-control frame claiming 125 with none present */
    unsigned char bin[2] = {0x82, 0x7d};
    EXPECT_EQ(WS_FRAME_NEED_MORE,
              WsFrameDecode(bin, sizeof bin, 0, out, sizeof out, &info, &consumed));
}

/* ── Control-frame semantics ────────────────────────────────────────────── */

TEST(WsControlReply, PingIsAnsweredWithPongCarryingTheSameData)
{
    std::vector<unsigned char> ping_data = {'p', 'i', 'n', 'g', '-', '1', '2', '3'};
    auto ping = Encode(WS_OPCODE_PING, ping_data, true, kMask);
    Decoded d = Decode(ping, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, d.rc);
    ASSERT_EQ(WS_OPCODE_PING, d.info.opcode);

    std::vector<unsigned char> reply(128);
    ssize_t n = WsFrameControlReply(&d.info, d.payload.data(), reply.data(), reply.size(), kMask);
    ASSERT_GT(n, 0);
    reply.resize(static_cast<size_t>(n));

    Decoded r = Decode(reply, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, r.rc);
    EXPECT_EQ(WS_OPCODE_PONG, r.info.opcode);
    EXPECT_EQ(ping_data, r.payload);
}

TEST(WsControlReply, PingFromServerIsAnsweredUnmaskedByServer)
{
    std::vector<unsigned char> ping_data = {'a', 'b'};
    auto ping = Encode(WS_OPCODE_PING, ping_data);   /* unmasked: server -> client */
    Decoded d = Decode(ping, WS_FRAME_REQUIRE_UNMASKED);
    ASSERT_EQ(WS_FRAME_DECODED, d.rc);

    std::vector<unsigned char> reply(64);
    ssize_t n = WsFrameControlReply(&d.info, d.payload.data(), reply.data(), reply.size(), nullptr);
    ASSERT_GT(n, 0);
    reply.resize(static_cast<size_t>(n));

    Decoded r = Decode(reply, WS_FRAME_REQUIRE_UNMASKED);
    ASSERT_EQ(WS_FRAME_DECODED, r.rc);
    EXPECT_EQ(WS_OPCODE_PONG, r.info.opcode);
    EXPECT_EQ(ping_data, r.payload);
}

TEST(WsControlReply, PongAndDataFramesNeedNoReply)
{
    std::vector<unsigned char> data = {'x'};
    for (WsOpcode op : {WS_OPCODE_PONG, WS_OPCODE_TEXT, WS_OPCODE_BINARY, WS_OPCODE_CONTINUATION}) {
        auto frame = Encode(op, data);
        Decoded d  = Decode(frame);
        ASSERT_EQ(WS_FRAME_DECODED, d.rc);

        std::vector<unsigned char> reply(64);
        EXPECT_EQ(0, WsFrameControlReply(&d.info, d.payload.data(), reply.data(), reply.size(),
                                         kMask))
            << "opcode " << int(op);
    }
}

TEST(WsControlReply, CloseIsEchoedWithTheSameStatus)
{
    unsigned char close_payload[64];
    ssize_t       plen = WsClosePayloadEncode(close_payload, sizeof close_payload, 1001, "leaving");
    ASSERT_GT(plen, 0);

    std::vector<unsigned char> frame(128);
    ssize_t frame_len = WsFrameEncode(frame.data(), frame.size(), close_payload,
                                      static_cast<size_t>(plen), WS_OPCODE_CLOSE, 1, kMask);
    ASSERT_GT(frame_len, 0);
    frame.resize(static_cast<size_t>(frame_len));

    Decoded d = Decode(frame, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, d.rc);

    std::vector<unsigned char> reply(128);
    ssize_t n = WsFrameControlReply(&d.info, d.payload.data(), reply.data(), reply.size(), kMask);
    ASSERT_GT(n, 0);
    reply.resize(static_cast<size_t>(n));

    Decoded r = Decode(reply, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, r.rc);
    EXPECT_EQ(WS_OPCODE_CLOSE, r.info.opcode);

    unsigned short code       = 0;
    char           reason[32] = {};
    size_t         reason_len = 0;
    ASSERT_EQ(0, WsClosePayloadDecode(r.payload.data(), r.payload.size(), &code, reason,
                                      sizeof reason, &reason_len));
    EXPECT_EQ(1001, code);
    EXPECT_STREQ("leaving", reason);
}

TEST(WsControlReply, MalformedCloseIsAnsweredWithProtocolError)
{
    /* a one-byte close payload is a protocol violation */
    std::vector<unsigned char> bad = {0x88, 0x81, 0x01, 0x02, 0x03, 0x04, 0x00 ^ 0x01};
    Decoded d = Decode(bad, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, d.rc);
    ASSERT_EQ(WS_OPCODE_CLOSE, d.info.opcode);
    ASSERT_EQ(1u, d.info.payload_len);

    std::vector<unsigned char> reply(64);
    ssize_t n = WsFrameControlReply(&d.info, d.payload.data(), reply.data(), reply.size(), kMask);
    ASSERT_GT(n, 0);
    reply.resize(static_cast<size_t>(n));

    Decoded r = Decode(reply, WS_FRAME_REQUIRE_MASKED);
    ASSERT_EQ(WS_FRAME_DECODED, r.rc);
    unsigned short code       = 0;
    size_t         reason_len = 0;
    ASSERT_EQ(0, WsClosePayloadDecode(r.payload.data(), r.payload.size(), &code, nullptr, 0,
                                      &reason_len));
    EXPECT_EQ(1002, code);
}

TEST(WsControlReply, RejectsBadArguments)
{
    WsFrameInfo info{};
    info.opcode      = WS_OPCODE_PING;
    info.payload_len = 4;
    unsigned char ping[4] = {'d', 'a', 't', 'a'};
    unsigned char out[8];

    EXPECT_EQ(-1, WsFrameControlReply(nullptr, ping, out, sizeof out, kMask));
    EXPECT_EQ(-1, WsFrameControlReply(&info, nullptr, out, sizeof out, kMask));
    EXPECT_EQ(-1, WsFrameControlReply(&info, ping, nullptr, sizeof out, kMask));
    EXPECT_EQ(-1, WsFrameControlReply(&info, ping, out, sizeof out, kMask)); /* too small */
}
