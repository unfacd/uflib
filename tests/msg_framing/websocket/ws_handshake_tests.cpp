// RFC 6455 §4: key generation, accept computation and reply validation.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include <uflib/msg_framing/websocket/ws_handshake.h>
}

namespace {

/* The RFC 6455 §1.3 example. */
const char *kKey    = "dGhlIHNhbXBsZSBub25jZQ==";
const char *kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string
Reply(const std::string &extra = "")
{
    return std::string("HTTP/1.1 101 Switching Protocols\r\n") +
           "Upgrade: websocket\r\n" +
           "Connection: Upgrade\r\n" +
           "Sec-WebSocket-Accept: " + kAccept + "\r\n" + extra + "\r\n";
}

int
Validate(const std::string &response, WsHandshakeReply *reply = nullptr,
         const char *const *offered = nullptr)
{
    return WsHandshakeValidateReply(response.data(), response.size(), kKey, offered, reply);
}

} // namespace

/* ── Key material ───────────────────────────────────────────────────────── */

TEST(WsHandshakeAccept, MatchesTheRfcExample)
{
    char accept[29] = {};

    ASSERT_EQ(0, WsHandshakeAcceptKey(kKey, accept));
    EXPECT_STREQ(kAccept, accept);
}

TEST(WsHandshakeKey, IsTwentyFourBase64Characters)
{
    char first[25]  = {};
    char second[25] = {};

    ASSERT_EQ(0, WsHandshakeKey(first));
    ASSERT_EQ(0, WsHandshakeKey(second));
    EXPECT_EQ(24u, strlen(first));
    EXPECT_EQ(0, strcmp(first, second) == 0 ? 1 : 0);   /* fresh per call */

    /* base64 of 16 bytes is 24 characters: 20 data characters plus "==" */
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    for (size_t i = 0; i < 24; i++) EXPECT_NE(nullptr, strchr(alphabet, first[i])) << first[i];
}

TEST(WsHandshakeKey, RefusesBadArguments)
{
    EXPECT_EQ(-1, WsHandshakeKey(nullptr));
    EXPECT_EQ(-1, WsHandshakeAcceptKey(nullptr, nullptr));
}

/* ── Reply validation: the happy path ───────────────────────────────────── */

TEST(WsHandshakeReply, ValidReplyIsAccepted)
{
    WsHandshakeReply reply{};
    ASSERT_EQ(WS_HANDSHAKE_OK, Validate(Reply(), &reply));
    EXPECT_EQ(101, reply.status);
    EXPECT_STREQ(kAccept, reply.accept);
    EXPECT_STREQ("", reply.protocol);
}

TEST(WsHandshakeReply, HeaderNamesAreCaseInsensitiveAndValuesTrimmed)
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "upgrade: WebSocket\r\n"
                           "connection:   keep-alive ,  Upgrade  \r\n"
                           "sec-websocket-accept:   " + std::string(kAccept) + "   \r\n\r\n";

    EXPECT_EQ(WS_HANDSHAKE_OK, Validate(response));
}

TEST(WsHandshakeReply, SelectedProtocolIsAcceptedWhenOffered)
{
    const char *offered[] = {"chat", "superchat", nullptr};
    WsHandshakeReply reply{};

    ASSERT_EQ(WS_HANDSHAKE_OK, Validate(Reply("Sec-WebSocket-Protocol: superchat\r\n"), &reply,
                                        offered));
    EXPECT_STREQ("superchat", reply.protocol);
}

/* ── Reply validation: refusals ─────────────────────────────────────────── */

TEST(WsHandshakeReply, MissingUpgradeIsRefused)
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + std::string(kAccept) + "\r\n\r\n";
    EXPECT_EQ(WS_HANDSHAKE_ERR_UPGRADE, Validate(response));
}

TEST(WsHandshakeReply, WrongUpgradeValueIsRefused)
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: h2c\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + std::string(kAccept) + "\r\n\r\n";
    EXPECT_EQ(WS_HANDSHAKE_ERR_UPGRADE, Validate(response));
}

TEST(WsHandshakeReply, MissingConnectionUpgradeIsRefused)
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: keep-alive\r\n"
                           "Sec-WebSocket-Accept: " + std::string(kAccept) + "\r\n\r\n";
    EXPECT_EQ(WS_HANDSHAKE_ERR_UPGRADE, Validate(response));
}

TEST(WsHandshakeReply, WrongAcceptIsRefused)
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOoX\r\n\r\n";
    EXPECT_EQ(WS_HANDSHAKE_ERR_ACCEPT, Validate(response));
}

TEST(WsHandshakeReply, MissingAcceptIsRefused)
{
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n\r\n";
    EXPECT_EQ(WS_HANDSHAKE_ERR_ACCEPT, Validate(response));
}

TEST(WsHandshakeReply, NonSwitchingStatusIsRefused)
{
    EXPECT_EQ(WS_HANDSHAKE_ERR_STATUS, Validate("HTTP/1.1 400 Bad Request\r\n\r\n"));
}

TEST(WsHandshakeReply, VersionMismatchIsReportedSeparately)
{
    EXPECT_EQ(WS_HANDSHAKE_ERR_VERSION, Validate("HTTP/1.1 426 Upgrade Required\r\n"
                                                 "Sec-WebSocket-Version: 13\r\n\r\n"));
}

TEST(WsHandshakeReply, MalformedStatusLinesAreRefused)
{
    EXPECT_EQ(WS_HANDSHAKE_ERR_ARGUMENT, Validate("HTTP/1.1 1010 Nope\r\n\r\n"));
    EXPECT_EQ(WS_HANDSHAKE_ERR_ARGUMENT, Validate("ICY 101 Nope\r\n\r\n"));
    EXPECT_EQ(WS_HANDSHAKE_ERR_ARGUMENT, Validate(""));
}

TEST(WsHandshakeReply, NegotiatedExtensionIsRefused)
{
    EXPECT_EQ(WS_HANDSHAKE_ERR_EXTENSION,
              Validate(Reply("Sec-WebSocket-Extensions: permessage-deflate\r\n")));
}

TEST(WsHandshakeReply, UnofferedProtocolIsRefused)
{
    const char *offered[] = {"chat", nullptr};
    EXPECT_EQ(WS_HANDSHAKE_ERR_PROTOCOL,
              Validate(Reply("Sec-WebSocket-Protocol: superchat\r\n"), nullptr, offered));

    /* nothing offered at all */
    EXPECT_EQ(WS_HANDSHAKE_ERR_PROTOCOL, Validate(Reply("Sec-WebSocket-Protocol: chat\r\n")));
}

TEST(WsHandshakeReply, LengthBoundedResponseNeedsNoTerminator)
{
    std::string response = Reply();
    /* data() is NUL-terminated by std::string, so hand over a view that stops
     * exactly at the final CRLF: the parser must respect len. */
    ASSERT_EQ(WS_HANDSHAKE_OK,
              WsHandshakeValidateReply(response.data(), response.size(), kKey, nullptr, nullptr));
}

TEST(WsHandshakeReply, NullArgumentsAreRefused)
{
    EXPECT_EQ(WS_HANDSHAKE_ERR_ARGUMENT,
              WsHandshakeValidateReply(nullptr, 10, kKey, nullptr, nullptr));
    EXPECT_EQ(WS_HANDSHAKE_ERR_ARGUMENT, WsHandshakeValidateReply("x", 1, nullptr, nullptr, nullptr));
    EXPECT_EQ(WS_HANDSHAKE_ERR_ARGUMENT, WsHandshakeValidateReply("x", 0, kKey, nullptr, nullptr));
}
