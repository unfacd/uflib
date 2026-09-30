-- Every transform fed real input, every validator fed a passing value.
xf = {
    hex    = "abc",
    b64    = "hello",
    b64url = "hello?",
    b32    = "hello",
    upper  = "shadow",
    lower  = "SHADOW",
    sub    = "port=19701",
    comp   = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    rx     = "aaabbb",
    both   = "user=devops",
}
v = {
    ip4     = "192.168.1.20",
    ip6     = "2001:db8::1",
    email   = "ops@unfacd.io",
    url     = "https://tig.unfacd.io/uflib",
    fqdn    = "db.ufsrv.unfacd.com",
    date    = "2024-02-29",
    size    = "10MiB",
    cidr    = "172.16.0.0/12",
    netmask = "255.255.0.0/16",
    enum    = "C",
    pattern = "abcdef",
    ranged  = 7,
    sized   = "abcd",
    list    = { "one", "two", "three" },
}
-- ── nested tables ─────────────────────────────────────────────────────────
-- Aliasing: `shared_timeouts` is bound once, then referenced by name from
-- inside another table literal.  There is no dotted assignment in this format
-- -- `nest.shared = x` is not valid -- so an alias is always reached by naming
-- it as a value, never by assigning into a path.
shared_timeouts = { connected = 120, suspended = 0 }

nest = {
    level2 = {
        level3 = {
            leaf_int  = 42,
            leaf_str  = "three levels down",
            leaf_list = { "a", "b" },
        },
        sibling = "at level 2",
    },
    shared = shared_timeouts,
}

return { xf = xf, v = v, nest = nest }
