/**
 * @file ufcommand_mem_safety.cpp
 * @brief Memory-safety and robustness cases for ufcommand, run one per process.
 *
 * Every case here is a shape that has either crashed or corrupted memory in an
 * earlier revision: a use-after-free in alias chain resolution, a heap read past
 * the end of a config buffer, a NULL dereference from the remove entry points,
 * and a double free when an alias expansion fails on its second round.
 *
 * The reason for the fork: under AddressSanitizer a use-after-free or an
 * out-of-bounds access terminates the process immediately.  Run in-process, the
 * first such case would take the whole suite down and hide every result after
 * it -- including the result of the case that actually failed.  Each case
 * therefore runs in a child of its own, and this file asserts on how that child
 * ended rather than on what it returned.
 *
 * The children call _exit() directly so that LeakSanitizer's exit-time check
 * does not run there: this file is about crashes, not leaks.  Leak coverage
 * comes from the ordinary suite, which allocates and releases normally.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <uflib/ufcommand/ufcommand.h>
}

namespace {

/* ── Fork harness ────────────────────────────────────────────────────────── */

struct ChildOutcome {
    bool        exited       = false;
    int         exit_code    = 0;
    bool        signalled    = false;
    int         signal       = 0;
    std::string diagnostics; /* first few lines the child wrote */
};

std::string
FirstDiagnostic(const std::string &captured)
{
    /* The sanitizer's own summary line is the useful part; anything the case
     * itself printed is noise by comparison.  Fall back to the first non-empty
     * line so a plain assertion failure is still reported. */
    size_t start = 0;
    std::string fallback;
    while (start < captured.size()) {
        size_t end = captured.find('\n', start);
        if (end == std::string::npos) {
            end = captured.size();
        }
        const std::string line = captured.substr(start, end - start);
        if (line.find("SUMMARY:") != std::string::npos) {
            return line;
        }
        if (fallback.empty() && line.find_first_not_of(" \t\r") != std::string::npos) {
            fallback = line;
        }
        start = end + 1;
    }
    return fallback;
}

ChildOutcome
RunInChild(void (*body)())
{
    int fds[2];
    if (pipe(fds) != 0) {
        ADD_FAILURE() << "pipe() failed";
        return {};
    }

    /* Flush before forking so the child cannot inherit and duplicate buffered
     * output from the parent. */
    fflush(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        ADD_FAILURE() << "fork() failed";
        return {};
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        body();
        _exit(0);
    }

    close(fds[1]);

    /* Drain before waiting: a child that writes a full sanitizer report into a
     * pipe nobody is reading would block forever. */
    std::string captured;
    char        buf[4096];
    for (;;) {
        const ssize_t got = read(fds[0], buf, sizeof buf);
        if (got <= 0) {
            break;
        }
        if (captured.size() < 65536) {
            captured.append(buf, static_cast<size_t>(got));
        }
    }
    close(fds[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        ADD_FAILURE() << "waitpid() failed";
        return {};
    }

    ChildOutcome outcome;
    outcome.diagnostics = FirstDiagnostic(captured);
    if (WIFEXITED(status)) {
        outcome.exited    = true;
        outcome.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outcome.signalled = true;
        outcome.signal    = WTERMSIG(status);
    }
    return outcome;
}

void
ExpectChildSurvived(const ChildOutcome &outcome, const char *what)
{
    if (outcome.signalled) {
        FAIL() << what << ": died from signal " << outcome.signal << " ("
               << strsignal(outcome.signal) << ")" << (outcome.diagnostics.empty()
                                                           ? ""
                                                           : "\n  " + outcome.diagnostics);
        return;
    }
    ASSERT_TRUE(outcome.exited) << what;
    EXPECT_EQ(0, outcome.exit_code) << what << ": exited " << outcome.exit_code
                                    << (outcome.diagnostics.empty() ? "" : "\n  " + outcome.diagnostics);
}

/* ── Minimal environment for a case body ─────────────────────────────────── */

class Env {
public:
    UfCommandRegistry *reg    = nullptr;
    UfCommandParser   *parser = nullptr;
    UfCommandResult   *res    = nullptr;
    int                calls  = 0;

    Env()
    {
        UfCommandRegistryCreate(nullptr, &reg);
        UfCommandParserCreate(nullptr, &parser);
        UfCommandResultCreate(&res);
    }
    Env(const Env &)            = delete;
    Env &operator=(const Env &) = delete;
    ~Env()
    {
        UfCommandResultDestroy(res);
        UfCommandParserDestroy(parser);
        UfCommandRegistryDestroy(reg);
    }

    static UfCommandStatus Handler(UfCommandContext * /*ctx*/, const char * /*name*/,
                                   const UfCommandArg * /*args*/, size_t /*arg_count*/, void *user_data)
    {
        static_cast<Env *>(user_data)->calls++;
        return UFCOMMAND_OK;
    }

    void Add(const char *name, size_t min_args = 0, size_t max_args = SIZE_MAX)
    {
        UfCommandDefinition def{name, "", Handler, this, min_args, max_args};
        UfCommandRegistryAdd(reg, &def);
    }
    void Alias(const char *alias, const char *expansion)
    {
        UfCommandAliasDefinition def{alias, expansion};
        UfCommandRegistryAddAlias(reg, &def);
    }
    UfCommandStatus Run(const char *line) { return UfCommandParserExecute(parser, reg, nullptr, line, res); }
};

/* ── Alias resolution ────────────────────────────────────────────────────── */

/* A two-level chain is the smallest shape that reused a freed token. */
TEST(UfCommandMemSafety, TwoLevelAliasChain)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("checkout");
        env.Alias("a", "b");
        env.Alias("b", "checkout");
        env.Run("a");
    });
    ExpectChildSurvived(outcome, "two-level alias chain");
}

TEST(UfCommandMemSafety, TenLevelAliasChain)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("checkout");
        env.Alias("L1", "checkout");
        for (int i = 2; i <= 10; ++i) {
            env.Alias(("L" + std::to_string(i)).c_str(), ("L" + std::to_string(i - 1)).c_str());
        }
        env.Run("L10");
    });
    ExpectChildSurvived(outcome, "ten-level alias chain");
}

/* A chain that exceeds the resolver's bound exercises the exhaustion path,
 * which is where the freed-token comparison used to happen. */
TEST(UfCommandMemSafety, ChainBeyondTheResolveBound)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("checkout");
        env.Alias("L1", "checkout");
        for (int i = 2; i <= 200; ++i) {
            env.Alias(("L" + std::to_string(i)).c_str(), ("L" + std::to_string(i - 1)).c_str());
        }
        env.Run("L200");
    });
    ExpectChildSurvived(outcome, "alias chain beyond the resolve bound");
}

TEST(UfCommandMemSafety, SelfReferentialAlias)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Alias("a", "a");
        env.Run("a");
    });
    ExpectChildSurvived(outcome, "self-referential alias");
}

TEST(UfCommandMemSafety, MutuallyRecursiveAliases)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Alias("a", "b");
        env.Alias("b", "a");
        env.Run("a");
        env.Run("b");
    });
    ExpectChildSurvived(outcome, "mutually recursive aliases");
}

TEST(UfCommandMemSafety, ThreeWayAliasCycle)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Alias("a", "b");
        env.Alias("b", "c");
        env.Alias("c", "a");
        for (const char *entry : {"a", "b", "c"}) {
            env.Run(entry);
        }
    });
    ExpectChildSurvived(outcome, "three-way alias cycle");
}

/* A cycle reached only on the second expansion round: the first round succeeds
 * and replaces the token array, so the failure path must free the array the
 * caller still holds rather than the one it already released. */
TEST(UfCommandMemSafety, FailureOnTheSecondExpansionRound)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("checkout");
        env.Alias("a", "b");
        env.Alias("b", "checkout $3"); /* round 2: $3 cannot be satisfied */
        env.Run("a");
    });
    ExpectChildSurvived(outcome, "expansion failure on the second round");
}

TEST(UfCommandMemSafety, FailureOnTheThirdExpansionRound)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("checkout");
        env.Alias("a", "b");
        env.Alias("b", "c");
        env.Alias("c", "checkout $4");
        env.Run("a");
    });
    ExpectChildSurvived(outcome, "expansion failure on the third round");
}

/* An expansion that reduces to zero tokens leaves the resolver holding a list
 * whose head is NULL; looking that up must not dereference it. */
TEST(UfCommandMemSafety, ExpansionReducingToNothing)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("echo");
        env.Alias("empty", "$*");
        env.Run("empty");
        env.Alias("quoted", "\"\"");
        env.Run("quoted");
    });
    ExpectChildSurvived(outcome, "expansion reducing to nothing");
}

TEST(UfCommandMemSafety, AliasExpansionAtTheTokenBudget)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("w0");
        std::string expansion;
        for (int i = 0; i < 64; ++i) {
            expansion += (i ? " " : "");
            expansion += "w" + std::to_string(i);
        }
        env.Alias("big", expansion.c_str());
        env.Run("big");
    });
    ExpectChildSurvived(outcome, "alias expansion at the token budget");
}

TEST(UfCommandMemSafety, AliasExpansionWithManyPositionalReferences)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("echo", 0, SIZE_MAX);
        env.Alias("many", "echo $1 $2 $3 $4 $5 $6 $7 $8 $9 $* $* $*");
        env.Run("many a b c d e f g h i j k");
    });
    ExpectChildSurvived(outcome, "expansion with many positional references");
}

TEST(UfCommandMemSafety, RepeatedResolutionOfTheSameChain)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("checkout", 0, SIZE_MAX);
        env.Alias("a", "b");
        env.Alias("b", "checkout $1");
        for (int i = 0; i < 5000; ++i) {
            env.Run("a payload");
        }
    });
    ExpectChildSurvived(outcome, "repeated resolution of the same chain");
}

/* ── Registry entry points with degenerate arguments ─────────────────────── */

TEST(UfCommandMemSafety, RemoveEntryPointsRejectNullNames)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("status");
        env.Alias("st", "status");
        UfCommandBinding b{UFCOMMAND_MOD_CTRL, "P", "status"};
        UfCommandRegistryAddBinding(env.reg, &b);

        UfCommandRegistryRemove(env.reg, nullptr);
        UfCommandRegistryRemoveAlias(env.reg, nullptr);
        UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_CTRL, nullptr);
        UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_NONE, nullptr);

        /* The registry must still be usable afterwards. */
        env.Run("status");
        env.Run("st");
    });
    ExpectChildSurvived(outcome, "remove entry points with NULL names");
}

TEST(UfCommandMemSafety, LookupEntryPointsRejectNullNames)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        UfCommandDefinition found{};
        UfCommandRegistryFind(env.reg, nullptr, &found);
        UfCommandRegistryFind(env.reg, "x", nullptr);
        UfCommandParserExecute(env.parser, env.reg, nullptr, nullptr, env.res);
        UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL, nullptr, env.res);
        UfCommandParserExecuteDefaultBinding(env.parser, env.reg, nullptr, nullptr, env.res);
    });
    ExpectChildSurvived(outcome, "lookup entry points with NULL names");
}

TEST(UfCommandMemSafety, NamesAroundTheLengthBound)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        for (size_t len : {size_t{1}, size_t{4094}, size_t{4095}, size_t{4096}, size_t{4097},
                           size_t{8192}}) {
            const std::string name(len, 'n');
            env.Add(name.c_str());
            UfCommandDefinition found{};
            UfCommandRegistryFind(env.reg, name.c_str(), &found);
            UfCommandRegistryRemove(env.reg, name.c_str());
        }
    });
    ExpectChildSurvived(outcome, "names around the length bound");
}

/* A prefix candidate whose joined name overflows the internal buffer must not be
 * used, and must not be read past its end while being discarded. */
TEST(UfCommandMemSafety, OversizedPrefixJoin)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        const std::string a(4000, 'a');
        const std::string b(4000, 'b');
        env.Add(a.c_str());
        env.Run((a + " " + b).c_str());
        env.Run((a + " " + b + " " + b).c_str());
    });
    ExpectChildSurvived(outcome, "oversized prefix join");
}

TEST(UfCommandMemSafety, BindingKeysLongerThanTheProbeComposite)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("status");
        const std::string shared(4096, 'k');
        for (int i = 0; i < 40; ++i) {
            const std::string key = shared + std::to_string(i);
            UfCommandBinding b{UFCOMMAND_MOD_CTRL, key.c_str(), "status"};
            UfCommandRegistryAddBinding(env.reg, &b);
            UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL, key.c_str(),
                                          env.res);
            UfCommandRegistryRemoveBinding(env.reg, UFCOMMAND_MOD_CTRL, key.c_str());
        }
    });
    ExpectChildSurvived(outcome, "binding keys longer than the probe composite");
}

/* ── Churn: growth, tombstone reuse and removal interleaved ──────────────── */

TEST(UfCommandMemSafety, InterleavedGrowthRemovalAndReuse)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        for (int round = 0; round < 60; ++round) {
            for (int i = 0; i < 64; ++i) {
                env.Add(("cmd" + std::to_string(i)).c_str());
            }
            for (int i = 0; i < 64; i += 2) {
                UfCommandRegistryRemove(env.reg, ("cmd" + std::to_string(i)).c_str());
            }
            for (int i = 0; i < 64; ++i) {
                UfCommandDefinition found{};
                UfCommandRegistryFind(env.reg, ("cmd" + std::to_string(i)).c_str(), &found);
            }
        }
    });
    ExpectChildSurvived(outcome, "interleaved growth, removal and reuse");
}

TEST(UfCommandMemSafety, ManyAliasesAndBindingsAcrossGrowth)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("status");
        env.Add("echo", 0, SIZE_MAX);
        for (int i = 0; i < 2000; ++i) {
            const std::string alias_name = "a" + std::to_string(i);
            const std::string binding_key = "k" + std::to_string(i);
            env.Alias(alias_name.c_str(), "echo $*");
            UfCommandBinding b{UFCOMMAND_MOD_CTRL, binding_key.c_str(), "status"};
            UfCommandRegistryAddBinding(env.reg, &b);
        }
        for (int i = 0; i < 2000; ++i) {
            const std::string alias_name = "a" + std::to_string(i);
            const std::string binding_key = "k" + std::to_string(i);
            env.Run(alias_name.c_str());
            UfCommandParserExecuteBinding(env.parser, env.reg, nullptr, UFCOMMAND_MOD_CTRL,
                                          binding_key.c_str(), env.res);
        }
    });
    ExpectChildSurvived(outcome, "many aliases and bindings across growth");
}

/* ── A deterministic fuzz corpus ─────────────────────────────────────────── */

/* Not a correctness oracle: the point is that no input, however malformed, may
 * crash or corrupt memory.  The generator is seeded so a failure is
 * reproducible. */
TEST(UfCommandMemSafety, RandomisedInputsDoNotCrash)
{
    const ChildOutcome outcome = RunInChild([] {
        Env env;
        env.Add("status", 0, SIZE_MAX);
        env.Add("remote add", 0, SIZE_MAX);
        env.Add("echo", 0, SIZE_MAX);
        env.Alias("a", "b");
        env.Alias("b", "status $1");
        env.Alias("say", "echo $*");
        env.Alias("cycle", "cycle");

        const char *alphabet = "abc\"\\$*012 \t\n";
        uint64_t    state    = 0x9E3779B97F4A7C15ull;
        auto next            = [&state]() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };

        for (int iteration = 0; iteration < 20000; ++iteration) {
            const size_t len = static_cast<size_t>(next() % 40);
            std::string  line;
            line.reserve(len);
            for (size_t i = 0; i < len; ++i) {
                line += alphabet[next() % (sizeof(alphabet) - 1)];
            }
            env.Run(line.c_str());
            UfCommandResultReset(env.res);
        }
    });
    ExpectChildSurvived(outcome, "randomised input corpus");
}

}  // namespace
