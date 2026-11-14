// RFC 6455 closing handshake and error→close-code mapping.
#include <gtest/gtest.h>

extern "C" {
#include <uflib/msg_framing/websocket/ws_close.h>
#include <uflib/msg_framing/websocket/ws_frame.h>
}

namespace {

WsFrameInfo
FrameOf(WsOpcode opcode)
{
    WsFrameInfo info{};
    info.opcode = opcode;
    return info;
}

} // namespace

TEST(WsCloseHandshake, PeerCloseIsAnsweredOnce)
{
    WsCloseHandshake h{};
    WsCloseHandshakeInit(&h);

    WsFrameInfo close = FrameOf(WS_OPCODE_CLOSE);
    EXPECT_EQ(WS_CLOSE_ACTION_REPLY, WsCloseHandshakeOnReceive(&h, &close));
    EXPECT_EQ(1, h.received);
    EXPECT_FALSE(WsCloseHandshakeCanSendData(&h));

    /* a second CLOSE never earns a second reply */
    EXPECT_EQ(WS_CLOSE_ACTION_CLOSED, WsCloseHandshakeOnReceive(&h, &close));
}

TEST(WsCloseHandshake, DataAfterPeerCloseIsInvalid)
{
    WsCloseHandshake h{};
    WsCloseHandshakeInit(&h);

    WsFrameInfo close = FrameOf(WS_OPCODE_CLOSE);
    WsFrameInfo text  = FrameOf(WS_OPCODE_TEXT);

    EXPECT_EQ(WS_CLOSE_ACTION_REPLY, WsCloseHandshakeOnReceive(&h, &close));
    EXPECT_EQ(WS_CLOSE_ACTION_INVALID, WsCloseHandshakeOnReceive(&h, &text));
}

TEST(WsCloseHandshake, OurCloseThenPeerCloseNeedsNoReply)
{
    WsCloseHandshake h{};
    WsCloseHandshakeInit(&h);

    EXPECT_EQ(WS_CLOSE_ACTION_NONE, WsCloseHandshakeOnSent(&h, 1000));
    EXPECT_FALSE(WsCloseHandshakeCanSendData(&h));

    WsFrameInfo close = FrameOf(WS_OPCODE_CLOSE);
    EXPECT_EQ(WS_CLOSE_ACTION_CLOSED, WsCloseHandshakeOnReceive(&h, &close));
}

TEST(WsCloseHandshake, SendingACloseTwiceIsRefused)
{
    WsCloseHandshake h{};
    WsCloseHandshakeInit(&h);

    EXPECT_EQ(WS_CLOSE_ACTION_NONE, WsCloseHandshakeOnSent(&h, 1000));
    EXPECT_EQ(WS_CLOSE_ACTION_CLOSED, WsCloseHandshakeOnSent(&h, 1000));
}

TEST(WsCloseHandshake, DataBeforeCloseIsAllowed)
{
    WsCloseHandshake h{};
    WsCloseHandshakeInit(&h);

    WsFrameInfo binary = FrameOf(WS_OPCODE_BINARY);
    WsFrameInfo ping   = FrameOf(WS_OPCODE_PING);

    EXPECT_TRUE(WsCloseHandshakeCanSendData(&h));
    EXPECT_EQ(WS_CLOSE_ACTION_NONE, WsCloseHandshakeOnReceive(&h, &binary));
    EXPECT_EQ(WS_CLOSE_ACTION_NONE, WsCloseHandshakeOnReceive(&h, &ping));
    EXPECT_TRUE(WsCloseHandshakeCanSendData(&h));
}

TEST(WsCloseHandshake, NullsAreRefused)
{
    WsCloseHandshake h{};
    WsCloseHandshakeInit(&h);

    WsCloseHandshakeInit(nullptr);   /* no-op, not a crash */
    EXPECT_EQ(WS_CLOSE_ACTION_INVALID, WsCloseHandshakeOnSent(nullptr, 1000));
    EXPECT_EQ(WS_CLOSE_ACTION_INVALID, WsCloseHandshakeOnReceive(nullptr, nullptr));
    EXPECT_EQ(WS_CLOSE_ACTION_INVALID,
              WsCloseHandshakeOnReceive(&h, nullptr));
    EXPECT_EQ(0, WsCloseHandshakeCanSendData(nullptr));
}

TEST(WsCloseCodeMapping, CodecResultsMapToCloseCodes)
{
    EXPECT_EQ(1002, WsCloseCodeForError(-1));   /* protocol */
    EXPECT_EQ(1009, WsCloseCodeForError(-2));   /* capacity */
    EXPECT_EQ(1011, WsCloseCodeForError(-3));   /* internal misuse */
    EXPECT_EQ(1007, WsCloseCodeForError(-4));   /* invalid UTF-8 */
    EXPECT_EQ(1002, WsCloseCodeForError(-5));   /* reserved bit */
    EXPECT_EQ(1000, WsCloseCodeForError(0));
    EXPECT_EQ(1000, WsCloseCodeForError(WS_FRAME_DECODED));
}
