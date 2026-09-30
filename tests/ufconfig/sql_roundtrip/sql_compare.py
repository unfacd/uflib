#!/usr/bin/env python3
"""Read a loaded configuration back out of MariaDB and diff it against the
document it came from.

The second half of the round-trip check, and deliberately not written in C: it
shares no code with the loader, and it reaches the store through the *server's
own client* rather than through uflib.  A reader that reuses the writer's
parsing can only show the round trip is stable; this one can show it is faithful.

    sql_compare.py --document FILE --namespace NS [--config NAME] [dsn options]

Rows are pulled with

    docker exec -e MYSQL_PWD=... ufsrv-db mariadb -N -B -u ufsrv ufsrv -e SELECT

so the only requirement is a reachable container with a client in it.

Buckets, and which of them fail the run:

    MATCH       identical after normalisation
    CASE        differs only in case -- see the note on transforms below
    DIFFER      same path, different value            FAILS
    MISSING     in the document, absent from the store FAILS
    EXTRA       in the store, absent from the document (a materialised default)

A row count lower than the loader's pair count is also reported: the store is
keyed (config_id, path) and the write is last-wins, so a repeated path collapses
and, if the two carries disagreed, one silently overwrote the other.
"""

import argparse
import subprocess
import sys


def _split_inline(body):
    """Split `a = 1, b = "x"` or `"x", "y"` at top-level commas."""
    parts, depth, cur = [], 0, ""
    for ch in body:
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
            continue
        if ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
        cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return [p for p in parts if p]


def parse_document(path):
    """Lua-styled nested tables -> {dotted.path: raw value literal}.

    Three things this format does that a naive line reader gets wrong, and all
    three are in the repository's own fixture:

      * inline tables -- `stats_backend = { address = "…", port = 8125 },`
      * inline arrays  -- `allow_list_god = {"GOD_SHA1", "GOD_SHA2"},`
      * an alias block -- a trailing `return { ufsrv = ufsrv, … }`, whose
        entries *reference* a table rather than declare a field.  The store
        correctly holds no such row, so a parser that records them reports them
        as missing and the run fails on its own reader.
    """
    values = {}
    stack = []
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.split("--", 1)[0].strip()
            if not line:
                continue

            while line.startswith("}"):
                if stack:
                    stack.pop()
                line = line[1:].strip().lstrip(",").strip()
            if not line or "=" not in line:
                continue

            key, rest = line.split("=", 1)
            key = key.strip()
            value = rest.strip().rstrip(",").strip()
            if not key or key == "return":
                continue

            if value == "{":
                stack.append(key)
                continue

            if value.startswith("{") and value.endswith("}"):
                body = value[1:-1].strip()
                if not body:                       # {} -- a table with no children
                    values[".".join(stack + [key])] = "{}"
                    continue
                parts = _split_inline(body)
                if all("=" in p for p in parts):   # inline table
                    for p in parts:
                        k2, v2 = p.split("=", 1)
                        values[".".join(stack + [key, k2.strip()])] = v2.strip().rstrip(",")
                else:                              # array -- the store indexes it
                    for i, p in enumerate(parts):
                        values[".".join(stack + [key, str(i)])] = p
                continue

            # A bare identifier is a reference to another table, not a value.
            if value and value[0] not in "\"'" and value not in ("true", "false"):
                try:
                    float(value)
                except ValueError:
                    continue

            values[".".join(stack + [key])] = value
    return values


def normalise(literal):
    """Reduce a document literal and a stored scalar to a comparable pair.

    Returns (kind, value).  The store holds canonical-form scalars -- strings
    quoted, integers bare -- while the document holds Lua literals, so both
    sides land on the same shape here.
    """
    s = literal.strip()
    if len(s) >= 2 and s[0] == s[-1] and s[0] in "\"'":
        return ("str", s[1:-1])
    if s in ("true", "false"):
        return ("bool", s == "true")
    if s == "{}":
        return ("table", "")
    try:
        return ("num", float(s))
    except ValueError:
        pass
    return ("ident", s)


def read_store(args, password):
    query = (
        "SELECT e.path, e.value FROM ufconfig_entry e "
        "JOIN ufconfig_config c ON c.config_id=e.config_id "
        "JOIN ufconfig_namespace n ON n.namespace_id=c.namespace_id "
        f"WHERE n.namespace_name='{args.namespace}' AND c.config_name='{args.config}' "
        "ORDER BY e.path"
    )
    cmd = ["docker", "exec", "-e", f"MYSQL_PWD={password}",
           args.container, "mariadb", "-N", "-B",
           "-u", args.user, "-h", args.host, "-P", str(args.port),
           args.database, "-e", query]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"reading the store failed: {res.stderr.strip()}")

    rows = {}
    for line in res.stdout.splitlines():
        if not line:
            continue
        path, _, value = line.partition("\t")
        rows[path] = value
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--document", required=True)
    ap.add_argument("--namespace", required=True)
    ap.add_argument("--config", default="ufsrvwebsock")
    ap.add_argument("--container", default="ufsrv-db")
    ap.add_argument("--database", default="ufsrv")
    ap.add_argument("--user", default="ufsrv")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", default=3306)
    ap.add_argument("--password-file", default="/tmp/dbpw")
    ap.add_argument("--pairs-written", type=int, default=None,
                    help="what the loader reported, to flag collapse")
    args = ap.parse_args()

    with open(args.password_file, encoding="utf-8") as fh:
        password = fh.read().strip()

    document = parse_document(args.document)
    store = read_store(args, password)

    match, case, differ, missing = [], [], [], []
    for path, literal in sorted(document.items()):
        if path not in store:
            missing.append((path, literal))
            continue
        dk, dv = normalise(literal)
        sk, sv = normalise(store[path])
        if (dk, dv) == (sk, sv):
            match.append(path)
        elif dk == sk and isinstance(dv, str) and dv.lower() == sv.lower():
            case.append((path, literal, store[path]))
        else:
            differ.append((path, literal, store[path]))

    extra = sorted(set(store) - set(document))

    print(f"document    {args.document}: {len(document)} paths")
    print(f"store       {args.namespace}/{args.config}: {len(store)} rows")
    if args.pairs_written is not None and args.pairs_written != len(store):
        print(f"collapse    loader wrote {args.pairs_written} pairs, store holds "
              f"{len(store)} rows -- {args.pairs_written - len(store)} repeated paths, last-wins")

    for path, lit, got in differ:
        print(f"  DIFFER    {path}: document {lit!r} vs store {got!r}")
    for path, lit in missing:
        print(f"  MISSING   {path} (document {lit!r})")
    for path, lit, got in case[:10]:
        print(f"  case      {path}: document {lit!r} vs store {got!r}")
    if len(case) > 10:
        print(f"  case      ... and {len(case) - 10} more")

    print(f"\nMATCH {len(match)}  case-only {len(case)}  DIFFER {len(differ)}  "
          f"MISSING {len(missing)}  EXTRA {len(extra)}")

    if extra:
        print("\nin the store and not in the document -- expected for materialised "
              "defaults, unless a name looks like something the document did declare:")
        for path in extra[:15]:
            print(f"  EXTRA     {path} = {store[path]!r}")
        if len(extra) > 15:
            print(f"  EXTRA     ... and {len(extra) - 15} more")

    if case:
        print("\ncase-only differences are not treated as failures: this schema transforms "
              "some values (the document's \"shadow\" is stored as \"SHADOW\"). "
              "They are listed so a real one cannot hide among them.")

    if differ or missing:
        print("\nFAIL -- the store does not agree with the document")
        return 1
    print("\nPASS -- every path the document declares is stored, with the value it declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
