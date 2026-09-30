/**
 * @file ufconfig_secrets_tests.cpp
 * @brief Encrypted config values: `encrypted = true` and the secrets file.
 *
 * Every case here asserts a *refusal* or an exact value, never that the module
 * currently does whatever it currently does.  The interesting ones are the
 * negative cases, because the whole feature is a security control and a control
 * that silently stops controlling is indistinguishable from one that works:
 *
 *   - an envelope whose ciphertext, tag or nonce has been altered by one hex
 *     digit must be refused, which is the only evidence that the GCM tag is
 *     being checked at all rather than merely computed;
 *   - a value on an encrypted field that carries no `enc:v1:` prefix must be
 *     refused, because otherwise deleting four characters from the config file
 *     downgrades the field to plaintext and the declaration enforces nothing;
 *   - a diagnostic about a rejected secret must not contain the secret.
 *
 * The fixtures are not products of this code.  The envelopes were produced by
 * ufsrvcorelib's `encrypt_plaintext.py`, the reference producer for the scheme,
 * with the keys in `fixtures/ufconfig.secrets`.  A round trip through them is
 * therefore a statement about compatibility with what is already deployed, not
 * a statement that this translation unit agrees with itself.
 */

#include <gtest/gtest.h>

extern "C" {
#include <uflib/ufconfig/ufconfig.h>
#include <uflib/utils_secrets.h>
}

#include "ufconfig_secrets_paths.h"

/* These are the generated artefacts this suite links, as the parent suite's
   ufconfig_test_schema.h declares them for its own tree.  They must sit at file
   scope with C linkage: inside the anonymous namespace below they would take
   internal linkage and never resolve to the definitions in the generated
   translation units. */
extern "C" {
extern const UfConfigFieldDesc g_ufconfig_fields[];
extern const int               g_ufconfig_field_count;
extern int                     UfConfigLookupPath(const char *path);
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char *kSecretsFile = UFCONFIG_SECRETS_FIXTURE_DIR "/ufconfig.secrets";

std::string Fixture(const char *leaf) {
    return std::string(UFCONFIG_SECRETS_FIXTURE_DIR) + "/" + leaf;
}

UfConfigLoadOptions Lenient() {
    UfConfigLoadOptions opt = {};
    opt.size    = sizeof(opt);
    opt.version = 1;
    opt.mode    = UF_CONFIG_LOAD_LENIENT;
    return opt;
}

/*!
 * A handle against this suite's schema.
 *
 * @p secrets_path is what the descriptor advertises.  NULL means the descriptor
 * names none, which is the case that consults `./ufconfig.secrets`.
 */
UfConfigStatus CreateWith(const char *secrets_path, UfConfig **out) {
    UfConfigDescriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fields              = g_ufconfig_fields;
    descriptor.field_count         = (size_t)g_ufconfig_field_count;
    descriptor.lookup              = UfConfigLookupPath;
    descriptor.config_file_secrets = secrets_path;
    return UfConfigCreate(out, &descriptor);
}

/*!
 * Creates against this suite's repository of keys, which every successful case
 * needs.  Returns nullptr when the handle could not be created, after recording
 * that as the failure it is.
 */
UfConfig *CreateDefault() {
    UfConfig *h = nullptr;
    EXPECT_EQ(CreateWith(kSecretsFile, &h), UF_CONFIG_OK);
    return h;
}

std::string LastErrorText() {
    const UfConfigError *e = UfConfigLastError();
    if (!e) return std::string();
    /* message is an array member, not a pointer, so it is never absent. */
    return std::string(e->message);
}

/*!
 * A `./ufconfig.secrets` in the working directory, for the cases that exercise
 * the default location.  Removed on destruction so the default is absent again
 * for every other case in the file.
 */
class CwdSecrets {
public:
    CwdSecrets() {
        FILE *src = fopen(kSecretsFile, "r");
        FILE *dst = fopen("ufconfig.secrets", "w");
        if (src && dst) {
            char   buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
        wrote_ = (dst != nullptr);
    }
    ~CwdSecrets() {
        if (wrote_) remove("ufconfig.secrets");
    }

    CwdSecrets(const CwdSecrets &)            = delete;
    CwdSecrets &operator=(const CwdSecrets &) = delete;

private:
    bool wrote_ = false;
};

/*! Replaces the envelope's first hex digit of its ciphertext field. */
std::string TamperCiphertext(const std::string &envelope) {
    size_t       prefix = strlen("enc:v1:");
    std::string  payload = envelope.substr(prefix);
    size_t       colon1 = payload.find(':');
    size_t       colon2 = payload.find(':', colon1 + 1);
    std::string  nonce   = payload.substr(0, colon1);
    std::string  ct      = payload.substr(colon1 + 1, colon2 - colon1 - 1);
    std::string  tag     = payload.substr(colon2 + 1);
    if (!ct.empty()) ct[0] = (ct[0] == '0') ? '1' : '0';
    return std::string("enc:v1:") + nonce + ":" + ct + ":" + tag;
}

std::string TamperTag(const std::string &envelope) {
    size_t      prefix  = strlen("enc:v1:");
    std::string payload = envelope.substr(prefix);
    size_t      colon1  = payload.find(':');
    size_t      colon2  = payload.find(':', colon1 + 1);
    std::string nonce   = payload.substr(0, colon1);
    std::string ct      = payload.substr(colon1 + 1, colon2 - colon1 - 1);
    std::string tag     = payload.substr(colon2 + 1);
    if (!tag.empty()) tag[0] = (tag[0] == '0') ? '1' : '0';
    return std::string("enc:v1:") + nonce + ":" + ct + ":" + tag;
}

}  // namespace

// ── The happy path, against fixtures the reference producer made ─────────────

TEST(UfConfigSecrets, ReferenceProducedEnvelopesDecryptToTheirPlaintexts) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "app.db_password", &v), UF_CONFIG_OK);
    ASSERT_EQ(v.kind, UF_CONFIG_KIND_STRING);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "correct-horse");

    ASSERT_EQ(UfConfigGetField(h, "app.api_token", &v), UF_CONFIG_OK);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "tok_9f2c41ab");

    ASSERT_EQ(UfConfigGetField(h, "app.short_secret", &v), UF_CONFIG_OK);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "tooshort");

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, TheEnvelopeIsStillWhatThePreTransformValueHolds) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "app.db_password", &v), UF_CONFIG_OK);

    /* raw keeps the envelope while as carries the plaintext, which is the whole
       mechanism: one field, two values, and the caller chooses. */
    ASSERT_NE(v.raw.str.ptr, nullptr);
    EXPECT_EQ(std::string(v.raw.str.ptr, v.raw.str.len).compare(0, 7, "enc:v1:"), 0);
    EXPECT_NE(std::string(v.as.str.ptr, v.as.str.len).compare(0, 7, "enc:v1:"), 0);

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, AnOrdinaryFieldBesideASecretIsUnaffected) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "app.db_address", &v), UF_CONFIG_OK);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "10.0.0.5");

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, ADocumentMayOmitAnEncryptedFieldEntirely) {
    /* Not every field is mandatory, so the presence of an encrypted field in a
       document is not what drives any of this -- the schema's declaration is.
       A document that simply does not mention the secret must still load. */
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    const char *doc = "/tmp/ufconfig_secrets_no_secret.lua";
    FILE       *f   = fopen(doc, "w");
    ASSERT_NE(f, nullptr);
    fputs("app = { db_address = \"192.168.1.1\" }\n", f);
    fclose(f);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, doc, &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    EXPECT_EQ(UfConfigGetField(h, "app.db_address", &v), UF_CONFIG_OK);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "192.168.1.1");

    remove(doc);
    UfConfigDestroy(h);
}

// ── Serialisation must not carry the plaintext out ───────────────────────────

TEST(UfConfigSecrets, EverySerialisedFormStillHoldsTheEnvelope) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    char *json = nullptr;
    ASSERT_EQ(UfConfigToJsonAlloc(h, &json), UF_CONFIG_OK);
    ASSERT_NE(json, nullptr);

    const std::string out(json);
    EXPECT_NE(out.find("enc:v1:"), std::string::npos)
        << "the envelope should be what is written out";
    EXPECT_EQ(out.find("correct-horse"), std::string::npos)
        << "the plaintext must not appear in a serialised form";
    EXPECT_EQ(out.find("tok_9f2c41ab"), std::string::npos);

    free(json);
    UfConfigDestroy(h);
}

// ── The declaration is binding: no envelope, no load ────────────────────────

TEST(UfConfigSecrets, AValueWithoutTheEnvelopePrefixIsRefused) {
    /* The downgrade case.  If this passed, an attacker able to edit the config
       file would only have to delete four characters to turn a secret into an
       accepted plaintext. */
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, Fixture("stripped_prefix.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_SECRET);

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, ATamperedCiphertextIsRefused) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    /* Take a real fixture and move one hex digit of the ciphertext.  Nothing
       about the value's shape changes, so only the authentication tag can
       reject it -- which is exactly what this establishes is happening. */
    FILE *f = fopen(Fixture("full.lua").c_str(), "r");
    ASSERT_NE(f, nullptr);
    char   buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const std::string needle = "enc:v1:";
    std::string       doc(buf);
    size_t            at = doc.find(needle);
    ASSERT_NE(at, std::string::npos);
    size_t end = doc.find('"', at);
    ASSERT_NE(end, std::string::npos);

    const std::string envelope = doc.substr(at, end - at);
    doc.replace(at, envelope.size(), TamperCiphertext(envelope));

    const char *path = "/tmp/ufconfig_secrets_tampered_ct.lua";
    FILE       *o    = fopen(path, "w");
    ASSERT_NE(o, nullptr);
    fputs(doc.c_str(), o);
    fclose(o);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, path, &opt, nullptr), UF_CONFIG_ERR_SECRET);

    remove(path);
    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, ATamperedTagIsRefused) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    FILE *f = fopen(Fixture("full.lua").c_str(), "r");
    ASSERT_NE(f, nullptr);
    char   buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const std::string needle = "enc:v1:";
    std::string       doc(buf);
    size_t            at = doc.find(needle);
    ASSERT_NE(at, std::string::npos);
    size_t end = doc.find('"', at);
    ASSERT_NE(end, std::string::npos);

    const std::string envelope = doc.substr(at, end - at);
    doc.replace(at, envelope.size(), TamperTag(envelope));

    const char *path = "/tmp/ufconfig_secrets_tampered_tag.lua";
    FILE       *o    = fopen(path, "w");
    ASSERT_NE(o, nullptr);
    fputs(doc.c_str(), o);
    fclose(o);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, path, &opt, nullptr), UF_CONFIG_ERR_SECRET);

    remove(path);
    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, StructurallyMalformedEnvelopesAreRefused) {
    struct Case {
        const char *name;
        const char *value;
    };
    const Case cases[] = {
        {"no components", "enc:v1:"},
        {"no tag", "enc:v1:00112233445566778899aabb:0011223344556677"},
        {"empty nonce", "enc:v1::0011223344556677:00112233445566778899aabbccddeeff"},
        {"empty tag", "enc:v1:00112233445566778899aabb:0011223344556677:"},
        {"short nonce", "enc:v1:00112233445566778899aa:0011223344556677:00112233445566778899aabbccddeeff"},
        {"long tag", "enc:v1:00112233445566778899aabb:0011223344556677:00112233445566778899aabbccddeeff00"},
        {"extra colon", "enc:v1:00112233445566778899aabb:0011223344556677:00112233445566778899aabbccddeeff:00"},
        {"non-hex ciphertext", "enc:v1:00112233445566778899aabb:zz11223344556677:00112233445566778899aabbccddeeff"},
        {"odd ciphertext", "enc:v1:00112233445566778899aabb:001122334455667:00112233445566778899aabbccddeeff"},
        {"wrong version", "enc:v2:00112233445566778899aabb:0011223344556677:00112233445566778899aabbccddeeff"},
    };

    for (const Case &c : cases) {
        UfConfig *h = CreateDefault();
        ASSERT_NE(h, nullptr) << c.name;

        std::string doc = std::string("app = { api_token = \"") + c.value + "\" }\n";
        const char *path = "/tmp/ufconfig_secrets_malformed.lua";
        FILE       *o    = fopen(path, "w");
        ASSERT_NE(o, nullptr) << c.name;
        fputs(doc.c_str(), o);
        fclose(o);

        UfConfigLoadOptions opt = Lenient();
        EXPECT_EQ(UfConfigLoadFile(h, path, &opt, nullptr), UF_CONFIG_ERR_SECRET) << c.name;

        remove(path);
        UfConfigDestroy(h);
    }
}

// ── A diagnostic about a secret must not contain the secret ─────────────────

TEST(UfConfigSecrets, ARejectedSecretIsNotQuotedBack) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    /* This fixture's db_password decrypts to a value that is absent from its
       field's one_of list, and the one_of message is one that interpolates the
       value it refused.  Without the mask, the plaintext would be sitting in
       UfConfigLastError() for any caller to read -- and every harness prints
       it. */
    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, Fixture("validator_quotes.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_VALIDATION);

    const std::string msg = LastErrorText();
    EXPECT_EQ(msg.find("not-in-the-list"), std::string::npos)
        << "the plaintext of a secret must never appear in a diagnostic";
    EXPECT_NE(msg.find("redacted"), std::string::npos)
        << "the message should say a value was withheld, not silently omit it";

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, ALengthBoundIsMeasuredAgainstThePlaintext) {
    /* short_secret's envelope holds "abc" and its field declares a minimum of 8.
       Measuring the envelope instead -- around 100 characters -- would pass, so
       a refusal here is what shows the bound sees the decrypted value. */
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, Fixture("validator_rejects.lua").c_str(), &opt, nullptr),
              UF_CONFIG_ERR_VALIDATION);

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, AOneOfBoundIsMeasuredAgainstThePlaintext) {
    /* The mirror of the case above, stated positively: full.lua's db_password
       satisfies one_of only because the value compared against the list is the
       plaintext.  Nothing in an envelope's own text is ever a member of a
       one_of list. */
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigDestroy(h);
}

// ── Writes ──────────────────────────────────────────────────────────────────

TEST(UfConfigSecrets, AnEncryptedFieldRefusesAWrite) {
    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    /* Read-only, and structurally so: the generator refuses `encrypted` together
       with `mutable`, so is_mutable is already zero for every encrypted field
       and the module's existing immutability check covers this. */
    EXPECT_EQ(UfConfigSetString(h, "app.db_password", "something-else"),
              UF_CONFIG_ERR_IMMUTABLE);

    /* The refusal must not have written anything on its way out. */
    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "app.db_password", &v), UF_CONFIG_OK);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "correct-horse");

    UfConfigDestroy(h);
}

// ── Where the keys come from ────────────────────────────────────────────────

TEST(UfConfigSecrets, AnUnreadableConfiguredPathIsRefused) {
    UfConfig *h = nullptr;
    EXPECT_EQ(CreateWith("/nonexistent/path/to/ufconfig.secrets", &h), UF_CONFIG_ERR_SECRET);
    EXPECT_EQ(h, nullptr);
}

TEST(UfConfigSecrets, AConfiguredPathDoesNotFallBackToTheWorkingDirectory) {
    /* The security property, and the reason the fallback only applies when the
       caller names nothing at all.  If an explicitly configured path could fall
       through to `./ufconfig.secrets`, then anyone able to create a file in the
       process's working directory could substitute the keys of a deployment
       that had already said where its keys are. */
    CwdSecrets planted;

    UfConfig *h = nullptr;
    EXPECT_EQ(CreateWith("/nonexistent/path/to/ufconfig.secrets", &h), UF_CONFIG_ERR_SECRET)
        << "a readable ./ufconfig.secrets must not rescue an explicit path that failed";
    EXPECT_EQ(h, nullptr);
}

TEST(UfConfigSecrets, TheWorkingDirectoryIsUsedWhenTheDescriptorNamesNothing) {
    CwdSecrets planted;

    UfConfig *h = nullptr;
    ASSERT_EQ(CreateWith(nullptr, &h), UF_CONFIG_OK);
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "app.db_password", &v), UF_CONFIG_OK);
    EXPECT_EQ(std::string(v.as.str.ptr, v.as.str.len), "correct-horse");

    UfConfigDestroy(h);
}

TEST(UfConfigSecrets, ADeclaredSecretWithNoKeyRefusesTheHandle) {
    /* A secrets file that is present and readable but silent about one of the
       declared paths.  Refusing at create puts the failure where the schema is
       known, rather than leaving it to surface later as an authentication
       failure on a value that looks perfectly well-formed. */
    const char *partial = "/tmp/ufconfig_secrets_partial.secrets";
    FILE       *f       = fopen(partial, "w");
    ASSERT_NE(f, nullptr);
    fputs("app.db_password=00f21e742f673e421e9cd57e50a343c15a4a2f950563412e7b1699ee4066daec\n", f);
    fclose(f);

    UfConfig *h = nullptr;
    EXPECT_EQ(CreateWith(partial, &h), UF_CONFIG_ERR_SECRET);
    EXPECT_EQ(h, nullptr);

    remove(partial);
}

TEST(UfConfigSecrets, AMalformedSecretsFileRefusesTheHandle) {
    const char *bad = "/tmp/ufconfig_secrets_bad.secrets";
    FILE       *f   = fopen(bad, "w");
    ASSERT_NE(f, nullptr);
    fputs("app.db_password=this-is-not-sixty-four-hex-characters-long-at-all-really\n", f);
    fclose(f);

    UfConfig *h = nullptr;
    EXPECT_EQ(CreateWith(bad, &h), UF_CONFIG_ERR_SECRET);
    EXPECT_EQ(h, nullptr);

    remove(bad);
}

TEST(UfConfigSecrets, AKeyOfTheWrongLengthRefusesTheHandle) {
    /* Valid hex, wrong size.  Distinct from the case above, which is not hex at
       all: a truncated or padded key is the shape a copied-and-pasted key file
       most often takes, and it must be refused at create rather than accepted
       and then failing every decryption as though the document were at fault.

       Derived from a real key by truncation and extension rather than written
       out by hand, because a hand-written "65 characters" is a claim nobody
       checks -- the first draft of this case was 66 and only failed because the
       length was asserted. */
    const std::string good = "00f21e742f673e421e9cd57e50a343c15a4a2f950563412e7b1699ee4066daec";
    ASSERT_EQ(good.size(), (size_t)UFLIB_SECRET_KEY_BYTES * 2);

    const std::string cases[] = {
        good.substr(0, good.size() - 1),  /* 63: one digit short */
        good + "ff",                      /* 66: two digits over */
        "",                               /* empty: the key is absent, not wrong */
    };

    for (const std::string &k : cases) {
        const char *path = "/tmp/ufconfig_secrets_wronglen.secrets";
        FILE       *f    = fopen(path, "w");
        ASSERT_NE(f, nullptr);
        fprintf(f, "app.db_password=%s\napp.api_token=%s\napp.short_secret=%s\n",
                k.c_str(), k.c_str(), k.c_str());
        fclose(f);

        UfConfig *h = nullptr;
        EXPECT_EQ(CreateWith(path, &h), UF_CONFIG_ERR_SECRET)
            << "a " << k.size() << "-character key must be refused";
        EXPECT_EQ(h, nullptr);

        remove(path);
    }
}

TEST(UfConfigSecrets, AKeyThatIsWellFormedButWrongRefusesTheLoad) {
    /* The operational case the tamper tests do not reach: nothing about the
       document is altered, and the key is a perfectly valid 64 hex characters --
       it is simply not the key that produced the envelope.  That is what a
       rotated key, or a secrets file swapped for another environment's, looks
       like, and the module has to refuse it rather than present a value.
       A tampered tag and a wrong key are the same failure by construction: GCM
       cannot tell them apart, and this asserts the module surfaces it either
       way. */
    const char *other_key =
        "1111111111111111111111111111111111111111111111111111111111111111";

    const char *path = "/tmp/ufconfig_secrets_wrongkey.secrets";
    FILE       *f    = fopen(path, "w");
    ASSERT_NE(f, nullptr);
    fprintf(f, "app.db_password=%s\napp.api_token=%s\napp.short_secret=%s\n",
            other_key, other_key, other_key);
    fclose(f);

    UfConfig *h = nullptr;
    ASSERT_EQ(CreateWith(path, &h), UF_CONFIG_OK) << "the key is well-formed, so create succeeds";
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    EXPECT_EQ(UfConfigLoadFile(h, Fixture("full.lua").c_str(), &opt, nullptr), UF_CONFIG_ERR_SECRET);

    /* And the reason must be the authentication check, not a structural
       complaint -- otherwise this would pass for the wrong reason. */
    const std::string msg = LastErrorText();
    EXPECT_NE(msg.find("AUTH"), std::string::npos)
        << "expected an authentication failure, got: " << msg;

    remove(path);
    UfConfigDestroy(h);
}

// ── The reload path re-derives everything ───────────────────────────────────

TEST(UfConfigSecrets, AReloadReDecryptsRatherThanReusingTheOldPlaintext) {
    /* Every reload builds a fresh tree through the pairs path and reapplies the
       schema to it, so nothing carries an already-decrypted value forward.  A
       reload re-reads the handle's own origin, so the document itself is
       rewritten between the two loads -- otherwise this would assert nothing. */
    const char *path = "/tmp/ufconfig_secrets_reload.lua";

    auto copy_fixture = [&](const char *leaf) {
        FILE *src = fopen(Fixture(leaf).c_str(), "r");
        FILE *dst = fopen(path, "w");
        EXPECT_NE(src, nullptr);
        EXPECT_NE(dst, nullptr);
        if (src && dst) {
            char   buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
        }
        if (src) fclose(src);
        if (dst) fclose(dst);
    };

    copy_fixture("full.lua");

    UfConfig *h = CreateDefault();
    ASSERT_NE(h, nullptr);

    UfConfigLoadOptions opt = Lenient();
    ASSERT_EQ(UfConfigLoadFile(h, path, &opt, nullptr), UF_CONFIG_OK);

    UfConfigValue v;
    ASSERT_EQ(UfConfigGetField(h, "app.db_password", &v), UF_CONFIG_OK);
    ASSERT_EQ(std::string(v.as.str.ptr, v.as.str.len), "correct-horse");

    /* The same document with db_password's envelope swapped for one that
       decrypts to a value its one_of list does not contain.  A reload that
       reused the plaintext already in the tree would keep reporting
       "correct-horse" and succeed; one that re-derives must refuse. */
    copy_fixture("validator_quotes.lua");

    UfConfigReloadReport report;
    memset(&report, 0, sizeof(report));
    EXPECT_NE(UfConfigReload(h, &report), UF_CONFIG_OK)
        << "the reload should have re-decrypted and rejected the new envelope";

    remove(path);
    UfConfigDestroy(h);
}
