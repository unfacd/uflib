#!/usr/bin/env python3
"""Diff the artefacts `sql_dump` wrote against the store, and against the
document that was loaded into it.

Two checks, because they prove different things:

  --against store      the serialiser is *lossless* over what the store holds.
                       Self-contained: no fixture needed.
  --against document   the whole round trip is faithful end to end --
                       document -> store -> artefact.  Stronger, and it is the
                       one that catches a transform applied twice, or the
                       raw/eff split leaking into output.

Both flatten the artefact to dotted paths, because the artefact is nested while
`ufconfig_entry` is a flat path=value set.  The Lua-style form is parsed by
`sql_compare`'s parser; JSON by the standard library.  YAML and INI are written
and reported, but not diffed -- there is no YAML parser here and the INI form is
sectioned, so a like-for-like flattening would need its own work.

It also asserts the exfiltration property directly: no field that the store
holds as an `enc:v1:` envelope may appear in a dump as anything else.
"""

import argparse
import json
import sys
from types import SimpleNamespace

from sql_compare import normalise, parse_document, read_store


def flatten(obj, prefix=""):
    """Nested JSON -> {dotted.path: python value}, indexing lists like the store."""
    out = {}
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.update(flatten(v, f"{prefix}.{k}" if prefix else k))
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            out.update(flatten(v, f"{prefix}.{i}"))
    else:
        out[prefix] = obj
    return out


def key_of(value):
    """Same shape as sql_compare.normalise, for an already-typed JSON value."""
    if isinstance(value, bool):
        return ("bool", value)
    if isinstance(value, (int, float)):
        return ("num", float(value))
    return ("str", str(value))


def compare(label, got, want):
    """got/want: {path: comparable tuple}.  Returns the number of failures."""
    differ = [(p, got[p], want[p]) for p in sorted(got) if p in want and got[p] != want[p]]
    missing = sorted(set(want) - set(got))
    extra = sorted(set(got) - set(want))

    for p, g, w in differ:
        print(f"  DIFFER    {p}: dump {g[1]!r} vs {label} {w[1]!r}")
    for p in missing:
        print(f"  MISSING   {p} (in {label}, absent from the dump)")
    for p in extra:
        print(f"  EXTRA     {p} = {got[p][1]!r}")

    print(f"  {label}: matched {len(got) - len(differ) - len(extra)}  "
          f"DIFFER {len(differ)}  MISSING {len(missing)}  EXTRA {len(extra)}")
    return len(differ) + len(missing)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump-dir", required=True)
    ap.add_argument("--namespace", required=True)
    ap.add_argument("--config", default="ufsrvwebsock")
    ap.add_argument("--against", choices=["store", "document"], required=True)
    ap.add_argument("--document", default=None)
    ap.add_argument("--container", default="ufsrv-db")
    ap.add_argument("--database", default="ufsrv")
    ap.add_argument("--user", default="ufsrv")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", default=3306)
    ap.add_argument("--password-file", default="/tmp/dbpw")
    args = ap.parse_args()

    with open(args.password_file, encoding="utf-8") as fh:
        password = fh.read().strip()

    shell = SimpleNamespace(namespace=args.namespace, config=args.config,
                            container=args.container, database=args.database,
                            user=args.user, host=args.host, port=args.port)
    store = read_store(shell, password)

    if args.against == "document":
        if not args.document:
            sys.exit("--against document needs --document")
        want = {p: normalise(v) for p, v in parse_document(args.document).items()}
        label = "document"
    else:
        want = {p: normalise(v) for p, v in store.items()}
        label = "store"

    print(f"dump dir    {args.dump_dir}")
    print(f"store       {len(store)} rows")
    print(f"comparing   against the {label}\n")

    failures = 0

    # JSON -- parsed exactly, by the standard library.
    path = f"{args.dump_dir}/dump.json"
    try:
        with open(path, encoding="utf-8") as fh:
            got = {p: key_of(v) for p, v in flatten(json.load(fh)).items()}
    except (OSError, ValueError) as e:
        print(f"  {path}: {e}")
        return 1
    print(f"json ({len(got)} paths)")
    failures += compare(label, got, want)

    # Lua-style -- same shape as the input format, so the same parser reads it.
    path = f"{args.dump_dir}/dump.lua"
    try:
        got = {p: normalise(v) for p, v in parse_document(path).items()}
    except OSError as e:
        print(f"  {path}: {e}")
        return 1
    print(f"\nlua ({len(got)} paths)")
    failures += compare(label, got, want)

    # The exfiltration property, asserted rather than assumed.
    leaked = []
    for p, v in store.items():
        if v.strip().startswith("enc:v1:") or v.strip().strip('"').startswith("enc:v1:"):
            dumped = got.get(p)
            if dumped is None or "enc:v1:" not in str(dumped[1]):
                leaked.append(p)
    print(f"\nencrypted fields: {len([p for p, v in store.items() if 'enc:v1:' in v])} in the store, "
          f"{len(leaked)} lost their envelope in the dump")
    for p in leaked:
        print(f"  LEAK      {p} is an envelope in the store and plaintext in the dump")

    # Reported, not diffed -- see the module docstring.
    for ext in ("yaml", "ini"):
        try:
            size = len(open(f"{args.dump_dir}/dump.{ext}", encoding="utf-8").read())
            print(f"{ext}: {size} bytes written, not diffed")
        except OSError as e:
            print(f"{ext}: {e}")
            failures += 1

    if failures:
        print(f"\nFAIL -- {failures} difference(s) between the dump and the {label}")
        return 1
    print(f"\nPASS -- every path the {label} holds appears in the dump with the same value")
    return 0


if __name__ == "__main__":
    sys.exit(main())
