-- Every field in the schema, every value compliant.  This is the file to copy
-- when adding a case: change one thing, add a line to expected.tsv.
shared_timeouts = { connected = 120, suspended = 0 }

req = { must = "present", typed_int = 3, typed_bool = true, typed_str = "text" }

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
    both   = "MiXeD",
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
    pattern = "ABCDEF",
    ranged  = 7,
    sized   = "abcd",
    list    = { "one", "two", "three" },
}

nest = {
    deep = { leaf = 42 },
    shared = shared_timeouts,
}
return { req = req, xf = xf, v = v, nest = nest }
