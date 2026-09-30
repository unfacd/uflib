/**
 * @file ufcommand_tests.cpp
 * @brief Adversarial unit tests for the ufcommand command registry and parser.
 *
 * These tests are written against the *documented contract* (the ufcommand
 * design document and module README), never against whatever the implementation
 * happens to do today.  Where the contract is silent, or where the library
 * returns one status for two different conditions, the assertion is on the
 * invariant that actually matters -- the handler did not run, the entry is still
 * independently addressable -- rather than on the incidental code, so that
 * improving the error reporting later does not require rewriting this file.
 *
 * Cases whose failure mode is a hard abort (use-after-free, out-of-bounds read,
 * NULL dereference) live in ufcommand_mem_safety.cpp, which forks a child per
 * case: a sanitizer abort would otherwise take this whole binary down and hide
 * every result after it.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <uflib/ufcommand/ufcommand.h>
}

namespace {

/* The bound sValidName() enforces on a registered name, and the number of
 * tokens the dispatcher will join when resolving a prefix.  Both are restated
 * here so that changing the library's limits shows up as a test failure rather
 * than as a silently relocated boundary. */
constexpr size_t UFCOMMAND_NAME_LIMIT = 4096;
/* Alias expansion is bounded by a configurable depth, and a parser
 * configuration that leaves it zero is rejected outright. */
constexpr size_t UFCOMMAND_ALIAS_DEPTH = 32;

/* In C++ the bitwise OR of two enumerators has type int, so combining modifier
 * bits requires an explicit cast.  A C++ host must therefore write this cast at
 * every call site; that the public API forces it is recorded as a finding
 * against the module rather than hidden by this helper. */
UfCommandModifier
Mods(unsigned bits)
{
    return static_cast<UfCommandModifier>(bits);
}

/* Records nothing: for cases that care only that a dispatch happened. */
UfCommandStatus
NoopHandler(UfCommandContext * /*ctx*/, const char * /*name*/, const UfCommandArg * /*args*/,
            size_t /*arg_count*/, void * /*user_data*/)
{
    return UFCOMMAND_OK;
}

/* A plain RAII environment rather than a gtest fixture: every test below builds
 * one on the stack, so construction and teardown are ordinary C++ lifetime.
 * Everything registered through it is owned by the registry and must be
 * released there -- running under LeakSanitizer is what proves it. */
class Env {
public:
    UfCommandRegistry *reg    = nullptr;
    UfCommandParser   *parser = nullptr;
    UfCommandResult   *res    = nullptr;

    /* What the recording handler saw on its most recent invocation. */
    int                      calls         = 0;
    std::string              command;
    std::vector<std::string> args;
    std::vector<size_t>      arg_lengths;
    bool                     saw_null_args = false;

    Env()
    {
        EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryCreate(nullptr, &reg));
        EXPECT_EQ(UFCOMMAND_OK, UfCommandParserCreate(nullptr, &parser));
        EXPECT_EQ(UFCOMMAND_OK, UfCommandResultCreate(&res));
    }

    /* A parser with an explicit token budget, for boundary cases. */
    explicit Env(size_t max_tokens, size_t max_token_length)
    {
        EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryCreate(nullptr, &reg));
        EXPECT_EQ(UFCOMMAND_OK, UfCommandResultCreate(&res));
        UfCommandParserConfig cfg{max_tokens, max_token_length, '"', '\\', UFCOMMAND_ALIAS_DEPTH};
        EXPECT_EQ(UFCOMMAND_OK, UfCommandParserCreate(&cfg, &parser));
    }

    ~Env()
    {
        UfCommandResultDestroy(res);
        UfCommandParserDestroy(parser);
        UfCommandRegistryDestroy(reg);
    }

    Env(const Env &)            = delete;
    Env &operator=(const Env &) = delete;

    static UfCommandStatus Record(UfCommandContext * /*ctx*/, const char *name, const UfCommandArg *arg,
                                  size_t arg_count, void *user_data)
    {
        Env *env         = static_cast<Env *>(user_data);
        env->calls++;
        env->command       = name ? name : "<null>";
        env->saw_null_args = (arg == nullptr);
        env->args.clear();
        env->arg_lengths.clear();
        for (size_t i = 0; i < arg_count; ++i) {
            env->args.emplace_back(arg[i].value, arg[i].length);
            env->arg_lengths.push_back(arg[i].length);
        }
        return UFCOMMAND_OK;
    }

    static UfCommandStatus Fail(UfCommandContext * /*ctx*/, const char * /*name*/,
                                const UfCommandArg * /*arg*/, size_t /*arg_count*/, void * /*user_data*/)
    {
        return UFCOMMAND_HANDLER_ERROR;
    }

    UfCommandStatus Add(const char *name, size_t min_args = 0, size_t max_args = SIZE_MAX)
    {
        UfCommandDefinition def{name, "", Record, this, min_args, max_args};
        return UfCommandRegistryAdd(reg, &def);
    }

    UfCommandStatus AddWith(const char *name, UfCommandHandlerCallback handler, size_t min_args = 0,
                            size_t max_args = SIZE_MAX)
    {
        UfCommandDefinition def{name, "", handler, this, min_args, max_args};
        return UfCommandRegistryAdd(reg, &def);
    }

    UfCommandStatus Alias(const char *alias, const char *expansion)
    {
        UfCommandAliasDefinition def{alias, expansion};
        return UfCommandRegistryAddAlias(reg, &def);
    }

    UfCommandStatus Bind(UfCommandModifier mods, const char *key, const char *command_name)
    {
        UfCommandBinding binding{mods, key, command_name};
        return UfCommandRegistryAddBinding(reg, &binding);
    }

    UfCommandStatus Run(const char *line) { return UfCommandParserExecute(parser, reg, nullptr, line, res); }

    /* A rejected line must not reach the handler.  This invariant holds whatever
     * status the library chooses to report. */
    void ExpectRejected(const char *line)
    {
        const int before = calls;
        EXPECT_NE(UFCOMMAND_OK, Run(line)) << "input unexpectedly accepted: " << line;
        EXPECT_EQ(before, calls) << "handler ran for a rejected input: " << line;
    }
};

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

TEST(UfCommandLifecycle, NullHandlesAreTolerated)
{
    UfCommandRegistryDestroy(nullptr);
    UfCommandParserDestroy(nullptr);
    UfCommandResultDestroy(nullptr);
    UfCommandResultReset(nullptr);
}

/* A NULL config is legal and must produce a usable object carrying the
 * documented defaults.  Every successful create is matched by a destroy here:
 * running under LeakSanitizer is what makes that obligation visible. */
TEST(UfCommandLifecycle, NullConfigUsesDocumentedDefaults)
{
    UfCommandRegistry *reg = nullptr;
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryCreate(nullptr, &reg));
    ASSERT_NE(nullptr, reg);

    UfCommandKeyConfig kc{};
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryGetKeyConfig(reg, &kc));
    EXPECT_EQ(Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT), kc.default_modifiers);

    UfCommandParser *parser = nullptr;
    ASSERT_EQ(UFCOMMAND_OK, UfCommandParserCreate(nullptr, &parser));
    ASSERT_NE(nullptr, parser);

    UfCommandResult *res = nullptr;
    ASSERT_EQ(UFCOMMAND_OK, UfCommandResultCreate(&res));

    /* The defaults must be sufficient to register a command and dispatch it. */
    UfCommandDefinition def{"status", "", NoopHandler, nullptr, 0, 0};
    EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryAdd(reg, &def));
    EXPECT_EQ(UFCOMMAND_OK, UfCommandParserExecute(parser, reg, nullptr, "status", res));
    EXPECT_STREQ("status", UfCommandResultGetResolvedCommand(res));

    UfCommandResultDestroy(res);
    UfCommandParserDestroy(parser);
    UfCommandRegistryDestroy(reg);
}

/* A NULL out-parameter must be rejected before anything is allocated: a caller
 * that passes one has no way to release what it cannot see. */
TEST(UfCommandLifecycle, NullOutParameterIsRejected)
{
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryCreate(nullptr, nullptr));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandParserCreate(nullptr, nullptr));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandResultCreate(nullptr));
}

TEST(UfCommandLifecycle, AccessorsOnNullReturnSafeDefaults)
{
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandResultGetStatus(nullptr));
    EXPECT_EQ(nullptr, UfCommandResultGetResolvedCommand(nullptr));
    EXPECT_EQ(0u, UfCommandResultGetArgCount(nullptr));
    EXPECT_EQ(nullptr, UfCommandResultGetArgs(nullptr));
}

/* A parser with a zero token or zero length budget could never parse anything;
 * refusing the configuration beats failing every later call. */
TEST(UfCommandLifecycle, ParserRejectsDegenerateConfig)
{
    UfCommandParser *p = nullptr;

    UfCommandParserConfig no_tokens{0, 1024, '"', '\\'};
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandParserCreate(&no_tokens, &p));
    EXPECT_EQ(nullptr, p);

    UfCommandParserConfig no_length{64, 0, '"', '\\'};
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandParserCreate(&no_length, &p));
    EXPECT_EQ(nullptr, p);
}

TEST(UfCommandLifecycle, KeyConfigDefaultsAndRoundTrip)
{
    Env env;
    UfCommandKeyConfig kc{};
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryGetKeyConfig(env.reg, &kc));
    EXPECT_EQ(Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT), kc.default_modifiers)
        << "the documented default modifier set is Ctrl+Shift";

    kc.default_modifiers = UFCOMMAND_MOD_ALT;
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistrySetKeyConfig(env.reg, &kc));
    UfCommandKeyConfig readback{};
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryGetKeyConfig(env.reg, &readback));
    EXPECT_EQ(UFCOMMAND_MOD_ALT, readback.default_modifiers);

    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistrySetKeyConfig(nullptr, &kc));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryGetKeyConfig(nullptr, &readback));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistrySetKeyConfig(env.reg, nullptr));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryGetKeyConfig(env.reg, nullptr));
}

TEST(UfCommandLifecycle, StatusNameCoversEveryEnumerator)
{
    const UfCommandStatus all[] = {
        UFCOMMAND_OK,               UFCOMMAND_NOT_FOUND,       UFCOMMAND_PARSE_ERROR,
        UFCOMMAND_INVALID_ARGUMENT, UFCOMMAND_DUPLICATE,       UFCOMMAND_NO_MEMORY,
        UFCOMMAND_BUFFER_TOO_SMALL, UFCOMMAND_HANDLER_ERROR,   UFCOMMAND_ALIAS_CYCLE,
        UFCOMMAND_CONFLICT,         UFCOMMAND_IO_ERROR,        UFCOMMAND_CONFIG_ERROR,
    };
    for (const auto status : all) {
        const char *name = UfCommandStatusName(status);
        ASSERT_NE(nullptr, name);
        EXPECT_STRNE("UNKNOWN", name) << "status " << static_cast<int>(status) << " has no name";
    }
    EXPECT_STREQ("UNKNOWN", UfCommandStatusName(static_cast<UfCommandStatus>(9999)));
}

/* ── Registration ────────────────────────────────────────────────────────── */

TEST(UfCommandRegistration, RejectsMalformedDefinitions)
{
    Env env;

    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Add(""));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Add(nullptr));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.AddWith("no-handler", nullptr));

    /* min_args > max_args is unsatisfiable: no input could ever dispatch it. */
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Add("inverted", 5, 2));

    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryAdd(nullptr, nullptr));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryAdd(env.reg, nullptr));
}

TEST(UfCommandRegistration, RejectsDuplicateNames)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));
    EXPECT_EQ(UFCOMMAND_DUPLICATE, env.Add("status"));
}

TEST(UfCommandRegistration, NameLengthBoundaryIsEnforced)
{
    Env env;

    const std::string at_limit(UFCOMMAND_NAME_LIMIT - 1, 'n');
    EXPECT_EQ(UFCOMMAND_OK, env.Add(at_limit.c_str())) << "one under the limit must be accepted";

    const std::string over_limit(UFCOMMAND_NAME_LIMIT, 'n');
    EXPECT_EQ(UFCOMMAND_INVALID_COMMAND, env.Add(over_limit.c_str()))
        << "a name over the limit is an invalid command, not a bad argument";

    UfCommandDefinition found{};
    EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, at_limit.c_str(), &found));
}

/* A name the dispatcher can never assemble is a dead registration, and accepting
 * it silently is worse than refusing it: the host believes the command exists. */
/* A registration the dispatcher can never select is a dead registration, and
 * accepting it silently is worse than refusing it.  This module has no fixed
 * prefix cap, so the four-token bound is gone and the invariant that matters is
 * the strong one: anything the registry accepts must actually be dispatchable. */
TEST(UfCommandRegistration, EveryAcceptedNameIsDispatchable)
{
    Env env;

    /* Well past the old four-token cap, at every depth. */
    for (size_t components = 1; components <= 8; ++components) {
        std::string name;
        for (size_t i = 0; i < components; ++i) {
            if (i != 0) {
                name += ' ';
            }
            name += std::string(1, static_cast<char>('a' + i));
        }
        ASSERT_EQ(UFCOMMAND_OK, env.Add(name.c_str())) << components << "-token name rejected";
        ASSERT_EQ(UFCOMMAND_OK, env.Run(name.c_str()))
            << components << "-token name accepted but not dispatchable";
        EXPECT_EQ(name, env.command) << components << "-token name dispatched as something else";
    }
}

/* Separator shape is canonicalised rather than rejected, so the registry and the
 * dispatcher must agree on the canonical form at every entry point. */
TEST(UfCommandRegistration, NameCanonicalisationIsConsistent)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("canon  ical"));

    UfCommandDefinition found{};
    EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, "canon ical", &found));
    EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, "canon  ical", &found));
    EXPECT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, "  canon ical  ", &found));
    EXPECT_STREQ("canon ical", found.name) << "the stored name is the canonical form";

    EXPECT_EQ(UFCOMMAND_DUPLICATE, env.Add(" canon ical "))
        << "the same command under a different spelling is still a duplicate";

    ASSERT_EQ(UFCOMMAND_OK, env.Run("canon    ical"));
    EXPECT_EQ("canon ical", env.command);
    EXPECT_EQ(UFCOMMAND_OK, env.Run("  canon\tical  "));
    EXPECT_EQ("canon ical", env.command);

    /* A name that canonicalises to nothing can be neither stored nor selected. */
    EXPECT_EQ(UFCOMMAND_INVALID_COMMAND, env.Add("   "));
    EXPECT_EQ(UFCOMMAND_INVALID_COMMAND, env.Add("\t"));
}

TEST(UfCommandRegistration, MultiTokenCommandIsReachable)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add", 2, 2));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("remote add origin url"));
    EXPECT_EQ("remote add", env.command);
}

TEST(UfCommandRegistration, FindReturnsRegisteredMetadata)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("build", 1, 3));

    UfCommandDefinition found{};
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, "build", &found));
    EXPECT_STREQ("build", found.name);
    EXPECT_EQ(1u, found.min_args);
    EXPECT_EQ(3u, found.max_args);
    EXPECT_NE(nullptr, found.handler);

    EXPECT_EQ(UFCOMMAND_NOT_FOUND, UfCommandRegistryFind(env.reg, "absent", &found));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryFind(nullptr, "build", &found));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryFind(env.reg, nullptr, &found));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryFind(env.reg, "build", nullptr));
}

TEST(UfCommandRegistration, RemoveRejectsUnknownAndNull)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));

    EXPECT_EQ(UFCOMMAND_NOT_FOUND, UfCommandRegistryRemove(env.reg, "absent"));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryRemove(env.reg, nullptr));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryRemove(nullptr, "status"));

    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemove(env.reg, "status"));
    EXPECT_NE(UFCOMMAND_OK, env.Run("status")) << "a removed command must not dispatch";
}

/* Tombstones must be reusable, and a reused slot must be indistinguishable from
 * a freshly inserted one -- otherwise a table that churns leaks capacity and
 * eventually refuses insertions that should succeed. */
TEST(UfCommandRegistration, TombstoneSlotIsReusableAcrossChurn)
{
    Env env;
    constexpr int kRounds = 500;

    for (int round = 0; round < kRounds; ++round) {
        const std::string name = "cmd" + std::to_string(round);

        ASSERT_EQ(UFCOMMAND_OK, env.Add(name.c_str())) << "round " << round;
        UfCommandDefinition found{};
        ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, name.c_str(), &found));
        EXPECT_STREQ(name.c_str(), found.name);

        ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemove(env.reg, name.c_str()));
        EXPECT_EQ(UFCOMMAND_NOT_FOUND, UfCommandRegistryFind(env.reg, name.c_str(), &found));

        /* Re-registering the same name in the same round forces reuse of the
         * tombstone just created rather than growth. */
        ASSERT_EQ(UFCOMMAND_OK, env.Add(name.c_str())) << "a removed name must be re-registrable";
        ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemove(env.reg, name.c_str()));
    }
}

/* Insert enough keys to force repeated table growth, then verify every one is
 * still reachable.  A growth that rehashes under a different key than the
 * lookup uses loses entries silently. */
TEST(UfCommandRegistration, EveryEntrySurvivesRepeatedGrowth)
{
    Env env;
    constexpr size_t kCount = 4000;
    std::vector<std::string> names;
    names.reserve(kCount);

    for (size_t i = 0; i < kCount; ++i) {
        names.push_back("command-" + std::to_string(i));
        ASSERT_EQ(UFCOMMAND_OK, env.Add(names.back().c_str())) << "insert " << i;
    }
    for (size_t i = 0; i < kCount; ++i) {
        UfCommandDefinition found{};
        ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryFind(env.reg, names[i].c_str(), &found))
            << "lookup " << names[i];
        EXPECT_STREQ(names[i].c_str(), found.name);
    }

    /* Removing every other entry leaves tombstones scattered through the probe
     * runs; the survivors must remain reachable regardless. */
    for (size_t i = 0; i < kCount; i += 2) {
        ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemove(env.reg, names[i].c_str()));
    }
    for (size_t i = 0; i < kCount; ++i) {
        UfCommandDefinition found{};
        const UfCommandStatus got = UfCommandRegistryFind(env.reg, names[i].c_str(), &found);
        if (i % 2 == 0) {
            EXPECT_EQ(UFCOMMAND_NOT_FOUND, got) << "removed entry still visible: " << names[i];
        } else {
            EXPECT_EQ(UFCOMMAND_OK, got) << "survivor lost: " << names[i];
        }
    }
}

/* ── Command / alias namespace ───────────────────────────────────────────── */

/* The documented startup order is "register built-in commands, then load user
 * config".  That only holds if user config cannot take over a built-in name. */
TEST(UfCommandNamespace, AliasMayNotShadowACommand)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));

    EXPECT_EQ(UFCOMMAND_CONFLICT, env.Alias("status", "echo hijacked"))
        << "a user alias must not be able to redirect a registered command";

    ASSERT_EQ(UFCOMMAND_OK, env.Run("status"));
    EXPECT_EQ("status", env.command) << "the built-in must still win";
}

TEST(UfCommandNamespace, CommandMayNotShadowAnAlias)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("co", "checkout $1"));
    EXPECT_EQ(UFCOMMAND_CONFLICT, env.Add("co"));
}

TEST(UfCommandNamespace, AliasMayNotShadowAnotherAlias)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("st", "status"));
    EXPECT_EQ(UFCOMMAND_DUPLICATE, env.Alias("st", "other"));
}

TEST(UfCommandNamespace, RejectsMalformedAliases)
{
    Env env;
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, env.Alias("", "status"));
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, env.Alias(nullptr, "status"));
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, env.Alias("a", ""));
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, env.Alias("a", nullptr));

    UfCommandAliasDefinition def{"a", "status"};
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, UfCommandRegistryAddAlias(nullptr, &def));
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, UfCommandRegistryAddAlias(env.reg, nullptr));

    /* An alias name may not contain a separator: commands may be multi-token,
     * aliases deliberately may not, and rejection is how that is enforced. */
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, env.Alias("two words", "status"));
    EXPECT_EQ(UFCOMMAND_INVALID_ALIAS, env.Alias("two\twords", "status"));

    EXPECT_EQ(UFCOMMAND_NOT_FOUND, UfCommandRegistryRemoveAlias(env.reg, "absent"));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryRemoveAlias(env.reg, nullptr));
}

/* ── Tokenisation ────────────────────────────────────────────────────────── */

TEST(UfCommandTokenisation, QuotingAndEscaping)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo \"two words\""));
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("two words", env.args[0]);

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo a\\ b"));
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("a b", env.args[0]) << "an escaped space must not split the token";

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo \"a\\\"b\""));
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("a\"b", env.args[0]);

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo a\\\\b"));
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("a\\b", env.args[0]);
}

/* Adjacent quoted and unquoted runs form one token: quoting only suspends
 * whitespace splitting, which is the shell behaviour the format documents. */
TEST(UfCommandTokenisation, QuoteTogglingWithinAToken)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo a\"b c\"d"));
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("ab cd", env.args[0]);
}

TEST(UfCommandTokenisation, EmptyQuotedArgumentIsPreserved)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo \"\""));
    ASSERT_EQ(1u, env.args.size()) << "an empty quoted argument is still a real argument";
    EXPECT_EQ("", env.args[0]);
}

TEST(UfCommandTokenisation, MalformedQuotingIsRejected)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    env.ExpectRejected("echo \"unterminated");
    env.ExpectRejected("echo trailing\\");
    env.ExpectRejected("\"");
}

TEST(UfCommandTokenisation, EmptyAndBlankLinesAreRejected)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));

    env.ExpectRejected("");
    env.ExpectRejected("   ");
    env.ExpectRejected("\t\n ");
}

TEST(UfCommandTokenisation, EveryWhitespaceKindSeparates)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    for (const char *sep : {" ", "\t", "\n", "\v", "\f", "\r"}) {
        const std::string line = std::string("echo") + sep + "arg";
        ASSERT_EQ(UFCOMMAND_OK, env.Run(line.c_str())) << "separator " << static_cast<int>(sep[0]);
        ASSERT_EQ(1u, env.args.size()) << "separator " << static_cast<int>(sep[0]);
        EXPECT_EQ("arg", env.args[0]);
    }
}

/* An argument's length is reported separately from its NUL-terminated value, so
 * it must describe the *decoded* argument, not the length of its source
 * spelling -- a host that memcpy's `length` bytes depends on this. */
TEST(UfCommandTokenisation, ArgLengthMatchesDecodedValue)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo \"a b\" c\\\\d"));
    ASSERT_EQ(2u, env.args.size());
    for (size_t i = 0; i < env.args.size(); ++i) {
        EXPECT_EQ(env.args[i].size(), env.arg_lengths[i]) << "arg " << i;
    }
}

/* There is no shell behind this parser and there must never be one: separators,
 * redirections and substitution markers are ordinary bytes inside an argument. */
TEST(UfCommandTokenisation, ShellMetacharactersCarryNoMeaning)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo \"; rm -rf /\" `whoami` $(id) | tee"));
    ASSERT_EQ(5u, env.args.size());
    EXPECT_EQ("; rm -rf /", env.args[0]);
    EXPECT_EQ("`whoami`", env.args[1]);
    EXPECT_EQ("$(id)", env.args[2]);
    EXPECT_EQ("|", env.args[3]);
    EXPECT_EQ("tee", env.args[4]);
    EXPECT_EQ(1, env.calls) << "one line must produce exactly one dispatch";
}

TEST(UfCommandTokenisation, TokenBudgetOverrunIsReported)
{
    Env env(16, 1024);
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));

    std::string at_budget = "echo";
    for (int i = 0; i < 15; ++i) {
        at_budget += " a";
    }
    ASSERT_EQ(UFCOMMAND_OK, env.Run(at_budget.c_str())) << "exactly the budget must be accepted";

    std::string over_budget = at_budget + " a";
    env.ExpectRejected(over_budget.c_str());
}

TEST(UfCommandTokenisation, TokenLengthBudgetOverrunIsReported)
{
    Env env(64, 8);
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo 12345678")) << "exactly the budget must be accepted";
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("12345678", env.args[0]);

    env.ExpectRejected("echo 123456789");
}

/* ── Dispatch and longest-prefix resolution ──────────────────────────────── */

TEST(UfCommandDispatch, LongestPrefixWins)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote"));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add", 2, 2));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add deep", 0, 0));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("remote"));
    EXPECT_EQ("remote", env.command);

    ASSERT_EQ(UFCOMMAND_OK, env.Run("remote add origin url"));
    EXPECT_EQ("remote add", env.command);
    ASSERT_EQ(2u, env.args.size());
    EXPECT_EQ("origin", env.args[0]);
    EXPECT_EQ("url", env.args[1]);

    ASSERT_EQ(UFCOMMAND_OK, env.Run("remote add deep"));
    EXPECT_EQ("remote add deep", env.command);
    EXPECT_EQ(0u, env.args.size());
}

/* Candidates are tried longest first, so an argument that happens to spell a
 * shorter command name must not steal the match. */
TEST(UfCommandDispatch, LongerPrefixBeatsAnArgumentThatNamesAShorterCommand)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote"));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add", 1, SIZE_MAX));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("remote add add"));
    EXPECT_EQ("remote add", env.command);
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("add", env.args[0]);
}

/* A prefix candidate whose joined name does not fit the internal buffer must be
 * abandoned outright.  Matching a truncated name while still consuming the
 * longer token count dispatches the wrong command with the wrong argument count,
 * and reports success while doing it. */
TEST(UfCommandDispatch, OversizedPrefixCandidateIsNotTruncatedIntoAMatch)
{
    Env env(64, 4096);

    const std::string a(1300, 'a');
    const std::string b(1300, 'b');
    const std::string c(1300, 'c');
    const std::string d(200, 'd');

    /* The 3-token name fits the registration bound (3902 bytes); the 4-token
     * join (4103 bytes) overflows the join buffer. */
    const std::string three = a + " " + b + " " + c;
    ASSERT_EQ(UFCOMMAND_OK, env.Add(three.c_str(), 1, 1));

    ASSERT_EQ(UFCOMMAND_OK, env.Run((three + " " + d).c_str()));
    EXPECT_EQ(three, env.command);
    ASSERT_EQ(1u, env.args.size()) << "the fourth token is an argument, not part of the name";
    EXPECT_EQ(d, env.args[0]);
}

TEST(UfCommandDispatch, ArgumentCountContractIsEnforcedBeforeTheHandler)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("exact", 2, 2));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("atmost", 0, 1));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("exact one two"));
    EXPECT_EQ(2u, env.args.size());

    const int before = env.calls;
    EXPECT_NE(UFCOMMAND_OK, env.Run("exact one"));
    EXPECT_NE(UFCOMMAND_OK, env.Run("exact one two three"));
    EXPECT_EQ(before, env.calls) << "the handler must not run when the contract is violated";

    ASSERT_EQ(UFCOMMAND_OK, env.Run("atmost"));
    ASSERT_EQ(UFCOMMAND_OK, env.Run("atmost one"));
    EXPECT_NE(UFCOMMAND_OK, env.Run("atmost one two"));
}

/* SIZE_MAX as max_args is the documented way to express "no upper bound". */
TEST(UfCommandDispatch, UnboundedArgumentCount)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));

    std::string line = "echo";
    for (int i = 0; i < 60; ++i) {
        line += " x";
    }
    ASSERT_EQ(UFCOMMAND_OK, env.Run(line.c_str()));
    EXPECT_EQ(60u, env.args.size());
}

TEST(UfCommandDispatch, UnknownCommandIsNotFound)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));

    EXPECT_EQ(UFCOMMAND_NOT_FOUND, env.Run("nosuch"));
    EXPECT_EQ(UFCOMMAND_NOT_FOUND, env.Run("nosuch with args"));
    EXPECT_EQ(0, env.calls);
    EXPECT_EQ(nullptr, UfCommandResultGetResolvedCommand(env.res));
    EXPECT_EQ(0u, UfCommandResultGetArgCount(env.res));
}

/* The resolved command must identify the command that actually ran, so a host
 * can log or route on it without re-deriving the match. */
TEST(UfCommandDispatch, ResolvedCommandMatchesTheInvocation)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add", 2, 2));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("remote add a b"));
    const char *resolved = UfCommandResultGetResolvedCommand(env.res);
    ASSERT_NE(nullptr, resolved);
    EXPECT_STREQ("remote add", resolved);
    EXPECT_EQ(env.command, std::string(resolved));
    EXPECT_EQ(env.args.size(), UfCommandResultGetArgCount(env.res));
}

/* The handler's own status must travel back to the caller unchanged, otherwise
 * a host cannot distinguish "the command failed" from "the command ran". */
TEST(UfCommandDispatch, HandlerFailureStatusPropagates)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.AddWith("fails", Env::Fail));

    EXPECT_EQ(UFCOMMAND_HANDLER_ERROR, env.Run("fails"));
    EXPECT_EQ(UFCOMMAND_HANDLER_ERROR, UfCommandResultGetStatus(env.res));
}

TEST(UfCommandDispatch, HandlerReceivesAnEmptySliceForAZeroArgCommand)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("status"));
    EXPECT_EQ(1, env.calls);
    EXPECT_TRUE(env.args.empty());
    EXPECT_EQ(0u, UfCommandResultGetArgCount(env.res));
    EXPECT_EQ(nullptr, UfCommandResultGetArgs(env.res))
        << "no arguments means no slice; a handler must not index a NULL array";
}

/* ── Aliases ─────────────────────────────────────────────────────────────── */

TEST(UfCommandAlias, PositionalSubstitution)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 1, 1));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add", 2, 2));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("co", "checkout $1"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("ra", "remote add $1 $2"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("co main"));
    EXPECT_EQ("checkout", env.command);
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("main", env.args[0]);

    ASSERT_EQ(UFCOMMAND_OK, env.Run("ra origin git@example.com"));
    EXPECT_EQ("remote add", env.command);
    ASSERT_EQ(2u, env.args.size());
    EXPECT_EQ("origin", env.args[0]);
    EXPECT_EQ("git@example.com", env.args[1]);
}

TEST(UfCommandAlias, StarSubstitutesEveryArgument)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("say", "echo $*"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("say hello world again"));
    ASSERT_EQ(3u, env.args.size());
    EXPECT_EQ("hello", env.args[0]);
    EXPECT_EQ("world", env.args[1]);
    EXPECT_EQ("again", env.args[2]);
}

TEST(UfCommandAlias, StarWithNoArgumentsSubstitutesNothing)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("say", "echo $*"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("say"));
    EXPECT_EQ(0u, env.args.size());
}

/* The documented placeholder vocabulary is $1..$9 plus $*.  Everything else is a
 * literal token and must reach the handler verbatim: silently rewriting part of
 * a user's argument is data corruption. */
TEST(UfCommandAlias, BeyondTheDocumentedVocabularyIsLiteral)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("lit", "echo $10 $0 $x $$$*x"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("lit"));
    ASSERT_EQ(4u, env.args.size());
    EXPECT_EQ("$10", env.args[0]) << "the vocabulary stops at $9; $10 is not a substitution";
    EXPECT_EQ("$0", env.args[1]);
    EXPECT_EQ("$x", env.args[2]);
    EXPECT_EQ("$$$*x", env.args[3]) << "a $* suffix must not be silently discarded";
}

TEST(UfCommandAlias, NinthPositionalIsReachable)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("ninth", "echo $9"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("ninth a b c d e f g h i"));
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("i", env.args[0]) << "$9 is the ninth positional argument";
}

TEST(UfCommandAlias, OutOfRangePositionalIsRejectedNotSilentlyDropped)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("co", "checkout $3"));

    env.ExpectRejected("co only");
}

/* A two-level chain is the shape that previously read freed memory, so it is
 * asserted explicitly rather than left to the longer-chain case. */
TEST(UfCommandAlias, TwoLevelChainResolves)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("b", "checkout"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("a", "b"));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("a"));
    EXPECT_EQ("checkout", env.command);
}

TEST(UfCommandAlias, LongFiniteChainStillResolves)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("L1", "checkout"));
    for (int i = 2; i <= 20; ++i) {
        ASSERT_EQ(UFCOMMAND_OK,
                  env.Alias(("L" + std::to_string(i)).c_str(), ("L" + std::to_string(i - 1)).c_str()));
    }

    ASSERT_EQ(UFCOMMAND_OK, env.Run("L20"));
    EXPECT_EQ("checkout", env.command);
}

/* A recursive alias is a configuration error; it must be reported as such and
 * must never be "resolved" by running off the end of a token list. */
TEST(UfCommandAlias, SelfReferenceIsReportedAsACycle)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("a", "a b"));

    EXPECT_EQ(UFCOMMAND_ALIAS_CYCLE, env.Run("a"));
    EXPECT_EQ(0, env.calls);
}

TEST(UfCommandAlias, MutualRecursionIsReportedAsACycle)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("a", "b"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("b", "a"));

    EXPECT_EQ(UFCOMMAND_ALIAS_CYCLE, env.Run("a"));
    EXPECT_EQ(0, env.calls);
}

TEST(UfCommandAlias, ThreeWayRecursionIsReportedAsACycle)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("a", "b"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("b", "c"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("c", "a"));

    EXPECT_EQ(UFCOMMAND_ALIAS_CYCLE, env.Run("a"));
    EXPECT_EQ(0, env.calls);
}

/* Expanding without bound is indistinguishable from a cycle in its effect on the
 * host, so it must surface as an error rather than as a misleading "not found". */
TEST(UfCommandAlias, ChainPastTheBoundIsReportedNotSilentlyTruncated)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 0, SIZE_MAX));

    constexpr int kDepth = 80;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("L1", "checkout"));
    for (int i = 2; i <= kDepth; ++i) {
        ASSERT_EQ(UFCOMMAND_OK,
                  env.Alias(("L" + std::to_string(i)).c_str(), ("L" + std::to_string(i - 1)).c_str()));
    }

    const UfCommandStatus got = env.Run(("L" + std::to_string(kDepth)).c_str());
    EXPECT_EQ(UFCOMMAND_ALIAS_DEPTH_EXCEEDED, got)
        << "depth exhaustion is distinguishable from a cycle, and from NOT_FOUND";
    EXPECT_EQ(0, env.calls);
}

TEST(UfCommandAlias, AliasToAnUnknownCommandIsNotFound)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("a", "nosuch"));

    EXPECT_EQ(UFCOMMAND_NOT_FOUND, env.Run("a"));
    EXPECT_EQ(0, env.calls);
}

/* An expansion that reduces to nothing leaves no command to dispatch.  It must
 * be an error, and above all it must not be a crash. */
TEST(UfCommandAlias, ExpansionReducingToNothingIsRejected)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("empty", "$*"));

    env.ExpectRejected("empty");
}

TEST(UfCommandAlias, RemovedAliasStopsResolvingAndFreesItsSlot)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("st", "status"));
    ASSERT_EQ(UFCOMMAND_OK, env.Run("st"));

    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemoveAlias(env.reg, "st"));
    EXPECT_EQ(UFCOMMAND_NOT_FOUND, env.Run("st"));

    ASSERT_EQ(UFCOMMAND_OK, env.Alias("st", "status"));
    EXPECT_EQ(UFCOMMAND_OK, env.Run("st"));
}

/* Case folding would make two distinct names collide, which is a silent
 * ambiguity rather than a convenience. */
TEST(UfCommandAlias, NamesAreCaseSensitive)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("st", "status"));

    EXPECT_EQ(UFCOMMAND_OK, env.Run("st"));
    EXPECT_EQ(UFCOMMAND_NOT_FOUND, env.Run("ST"));
}

/* ── Bindings ────────────────────────────────────────────────────────────── */

TEST(UfCommandBindings, BindingDispatchesThroughTheSamePathAsALine)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));
    ASSERT_EQ(UFCOMMAND_OK, env.Bind(Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT), "P", "status"));

    ASSERT_EQ(UFCOMMAND_OK,
              UfCommandParserExecuteBinding(env.parser, env.reg, nullptr,
                                            Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT), "P", env.res));
    EXPECT_STREQ("status", UfCommandResultGetResolvedCommand(env.res));
    EXPECT_EQ(1, env.calls);
}

/* A binding must be able to name an alias, otherwise the two user-facing
 * features cannot be composed. */
TEST(UfCommandBindings, BindingMayNameAnAlias)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 1, 1));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("co", "checkout $1"));
    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_CTRL, "B", "co main"));

    ASSERT_EQ(UFCOMMAND_OK, UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL,
                                                         "B", env.res));
    EXPECT_EQ("checkout", env.command);
    ASSERT_EQ(1u, env.args.size());
    EXPECT_EQ("main", env.args[0]);
}

TEST(UfCommandBindings, ModifierMaskIsPartOfTheIdentity)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("build", 0, 0));

    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_CTRL, "P", "status"));
    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_ALT, "P", "build"))
        << "the same key under a different modifier is a different binding";
    EXPECT_EQ(UFCOMMAND_DUPLICATE, env.Bind(UFCOMMAND_MOD_CTRL, "P", "build"));

    ASSERT_EQ(UFCOMMAND_OK,
              UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_ALT, "P", env.res));
    EXPECT_STREQ("build", UfCommandResultGetResolvedCommand(env.res));
}

TEST(UfCommandBindings, ModifierBitsCombineDistinctly)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));

    const UfCommandModifier sets[] = {
        UFCOMMAND_MOD_NONE,
        UFCOMMAND_MOD_CTRL,
        UFCOMMAND_MOD_SHIFT,
        UFCOMMAND_MOD_ALT,
        UFCOMMAND_MOD_META,
        Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT),
        Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT | UFCOMMAND_MOD_ALT | UFCOMMAND_MOD_META),
    };
    for (const auto mods : sets) {
        EXPECT_EQ(UFCOMMAND_OK, env.Bind(mods, "K", "status")) << "mods " << mods;
    }
    for (const auto mods : sets) {
        ASSERT_EQ(UFCOMMAND_OK, UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, mods, "K",
                                                             env.res))
            << "mods " << mods;
        EXPECT_STREQ("status", UfCommandResultGetResolvedCommand(env.res));
    }
}

TEST(UfCommandBindings, UnknownBindingIsNotFoundAndLeavesNoResidue)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));

    ASSERT_EQ(UFCOMMAND_OK, env.Run("status"));
    ASSERT_STREQ("status", UfCommandResultGetResolvedCommand(env.res));

    EXPECT_EQ(UFCOMMAND_NO_BINDING,
              UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL, "Z", env.res));
    EXPECT_EQ(nullptr, UfCommandResultGetResolvedCommand(env.res))
        << "a failed lookup must not expose the previous resolution";
    EXPECT_EQ(0u, UfCommandResultGetArgCount(env.res));
}

TEST(UfCommandBindings, DefaultBindingUsesTheConfiguredModifiers)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));
    ASSERT_EQ(UFCOMMAND_OK, env.Bind(Mods(UFCOMMAND_MOD_CTRL | UFCOMMAND_MOD_SHIFT), "P", "status"));

    EXPECT_EQ(UFCOMMAND_OK,
              UfCommandParserExecuteDefaultBinding(env.parser, env.reg, nullptr, "P", env.res))
        << "the documented default modifier set must be the lookup key";

    UfCommandKeyConfig kc{UFCOMMAND_MOD_ALT};
    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistrySetKeyConfig(env.reg, &kc));
    EXPECT_EQ(UFCOMMAND_NO_BINDING,
              UfCommandParserExecuteDefaultBinding(env.parser, env.reg, nullptr, "P", env.res))
        << "after reconfiguration the previous mask must no longer resolve";

    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_ALT, "P", "status"));
    EXPECT_EQ(UFCOMMAND_OK,
              UfCommandParserExecuteDefaultBinding(env.parser, env.reg, nullptr, "P", env.res));
}

TEST(UfCommandBindings, RemoveAndReuse)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));
    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_CTRL, "P", "status"));

    /* A missing entry to remove is NOT_FOUND; NO_BINDING is specific to dispatch,
     * where it separates "no such binding" from "the bound command was absent". */
    EXPECT_EQ(UFCOMMAND_NOT_FOUND, UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_ALT, "P"));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_CTRL, nullptr));

    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_CTRL, "P"));
    EXPECT_EQ(UFCOMMAND_NO_BINDING,
              UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL, "P", env.res));

    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_CTRL, "P", "status"));
    EXPECT_EQ(UFCOMMAND_OK,
              UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL, "P", env.res));
}

TEST(UfCommandBindings, RejectsMalformedBindings)
{
    Env env;
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Bind(UFCOMMAND_MOD_CTRL, "", "status"));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Bind(UFCOMMAND_MOD_CTRL, nullptr, "status"));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Bind(UFCOMMAND_MOD_CTRL, "P", ""));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, env.Bind(UFCOMMAND_MOD_CTRL, "P", nullptr));

    UfCommandBinding b{UFCOMMAND_MOD_CTRL, "P", "status"};
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryAddBinding(nullptr, &b));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandRegistryAddBinding(env.reg, nullptr));
}

/* Probing uses a temporary "modifiers:key" composite string.  Two keys sharing a
 * long prefix must remain independent entries: if the composite is truncated to
 * the point of collision, entries can become unreachable or be conflated. */
TEST(UfCommandBindings, KeysLongerThanTheProbeCompositeStayIndependent)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));

    const std::string shared(600, 'k');
    const std::string key_a = shared + "AAA";
    const std::string key_b = shared + "BBB";

    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_CTRL, key_a.c_str(), "status"));
    ASSERT_EQ(UFCOMMAND_OK, env.Bind(UFCOMMAND_MOD_CTRL, key_b.c_str(), "status"))
        << "distinct long keys must not be treated as the same binding";
    EXPECT_EQ(UFCOMMAND_DUPLICATE, env.Bind(UFCOMMAND_MOD_CTRL, key_a.c_str(), "status"));

    ASSERT_EQ(UFCOMMAND_OK, UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL,
                                                         key_a.c_str(), env.res));
    ASSERT_EQ(UFCOMMAND_OK, UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL,
                                                         key_b.c_str(), env.res));

    ASSERT_EQ(UFCOMMAND_OK, UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_CTRL, key_a.c_str()));
    EXPECT_EQ(UFCOMMAND_NO_BINDING, UfCommandParserExecuteBinding(env.parser, env.reg, nullptr,
                                                                 UFCOMMAND_MOD_CTRL, key_a.c_str(), env.res));
    EXPECT_EQ(UFCOMMAND_OK, UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL,
                                                         key_b.c_str(), env.res))
        << "removing one long key must not disturb its neighbour";
}

/* ── Result object ───────────────────────────────────────────────────────── */

TEST(UfCommandResult, ResetClearsEverythingFromThePreviousRun)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo one two"));
    ASSERT_EQ(2u, UfCommandResultGetArgCount(env.res));

    UfCommandResultReset(env.res);
    EXPECT_EQ(nullptr, UfCommandResultGetResolvedCommand(env.res));
    EXPECT_EQ(0u, UfCommandResultGetArgCount(env.res));
}

TEST(UfCommandResult, RepeatedRunsDoNotAccumulateArguments)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));

    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(UFCOMMAND_OK, env.Run("echo a b c"));
        EXPECT_EQ(3u, UfCommandResultGetArgCount(env.res)) << "run " << i;
    }

    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo"));
    EXPECT_EQ(0u, UfCommandResultGetArgCount(env.res)) << "a later run must not inherit earlier args";
}

/* A call that fails before it can touch the result must not leave the caller
 * reading the status of an earlier, unrelated run. */
TEST(UfCommandResult, FailedCallDoesNotExposeAStaleStatus)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, 0));
    ASSERT_EQ(UFCOMMAND_OK, env.Run("status"));
    ASSERT_EQ(UFCOMMAND_OK, UfCommandResultGetStatus(env.res));

    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT,
              UfCommandParserExecute(env.parser, env.reg, nullptr, nullptr, env.res));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandResultGetStatus(env.res))
        << "the result must describe this call, not the previous one";

    ASSERT_EQ(UFCOMMAND_OK, env.Run("status"));
    ASSERT_EQ(UFCOMMAND_OK, UfCommandResultGetStatus(env.res));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT,
              UfCommandParserExecute(nullptr, env.reg, nullptr, "status", env.res));
    EXPECT_EQ(UFCOMMAND_INVALID_ARGUMENT, UfCommandResultGetStatus(env.res));
}

TEST(UfCommandResult, ArgumentValuesStayValidWhileTheResultLives)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Run("echo alpha beta"));

    const UfCommandArg *args = UfCommandResultGetArgs(env.res);
    ASSERT_NE(nullptr, args);
    ASSERT_EQ(2u, UfCommandResultGetArgCount(env.res));
    EXPECT_EQ("alpha", std::string(args[0].value, args[0].length));
    EXPECT_EQ("beta", std::string(args[1].value, args[1].length));
}

/* ── The documented input/output table, exercised as data ────────────────── */

struct GoldenCase {
    const char     *line;
    UfCommandStatus status;
    const char     *resolved; /* nullptr when resolution must not happen */
    const char     *args;     /* '|'-separated, empty when there are none */
};

class UfCommandGolden : public ::testing::TestWithParam<GoldenCase> {};

TEST_P(UfCommandGolden, MatchesTheDocumentedOutcome)
{
    Env env;
    ASSERT_EQ(UFCOMMAND_OK, env.Add("status", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("remote add", 2, 2));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("checkout", 1, 1));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("echo", 0, SIZE_MAX));
    ASSERT_EQ(UFCOMMAND_OK, env.Add("build", 0, 0));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("st", "status"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("co", "checkout $1"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("ra", "remote add $1 $2"));
    ASSERT_EQ(UFCOMMAND_OK, env.Alias("say", "echo $*"));

    const GoldenCase   &c   = GetParam();
    const UfCommandStatus got = env.Run(c.line);
    ASSERT_EQ(c.status, got) << "input: " << c.line << " (expected " << UfCommandStatusName(c.status)
                             << ", observed " << UfCommandStatusName(got) << ")";

    const char *resolved = UfCommandResultGetResolvedCommand(env.res);
    if (c.resolved == nullptr) {
        EXPECT_EQ(nullptr, resolved) << "input: " << c.line;
    } else {
        ASSERT_NE(nullptr, resolved) << "input: " << c.line;
        EXPECT_STREQ(c.resolved, resolved) << "input: " << c.line;
    }

    std::string joined;
    for (size_t i = 0; i < env.args.size(); ++i) {
        if (i != 0) {
            joined += '|';
        }
        joined += env.args[i];
    }
    EXPECT_EQ(std::string(c.args), joined) << "input: " << c.line;
}

INSTANTIATE_TEST_SUITE_P(
    Documented, UfCommandGolden,
    ::testing::Values(
        /* Direct dispatch. */
        GoldenCase{"status", UFCOMMAND_OK, "status", ""},
        GoldenCase{"build", UFCOMMAND_OK, "build", ""},
        /* Alias dispatch. */
        GoldenCase{"st", UFCOMMAND_OK, "status", ""},
        GoldenCase{"co main", UFCOMMAND_OK, "checkout", "main"},
        GoldenCase{"say hello world", UFCOMMAND_OK, "echo", "hello|world"},
        /* Longest-prefix dispatch, quoted and unquoted. */
        GoldenCase{"remote add origin git@example.com", UFCOMMAND_OK, "remote add",
                   "origin|git@example.com"},
        GoldenCase{"ra origin git@example.com", UFCOMMAND_OK, "remote add", "origin|git@example.com"},
        GoldenCase{"\"remote add\" origin git@example.com", UFCOMMAND_OK, "remote add",
                   "origin|git@example.com"},
        /* Quoting does not change resolution. */
        GoldenCase{"\"st\"", UFCOMMAND_OK, "status", ""},
        /* Unregistered, and malformed beyond recovery. */
        GoldenCase{"nosuch", UFCOMMAND_NOT_FOUND, nullptr, ""},
        GoldenCase{"co", UFCOMMAND_PARSE_ERROR, nullptr, ""},
        GoldenCase{"''", UFCOMMAND_NOT_FOUND, nullptr, ""},
        /* An unbounded command accepts trailing tokens as arguments. */
        GoldenCase{"status extra", UFCOMMAND_OK, "status", "extra"}));

}  // namespace
