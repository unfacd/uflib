-- The full schema: every transform the catalogue has and every validator it
-- can enforce, plus the structural rules.  One schema so that any permutation
-- of a config file can be written against it and the result is known in
-- advance.
--
-- Every field here exists to be exercised.  Nothing in this schema configures
-- anything; it is the fixture the validator harness runs against.
local schema = {
    -- ── required, and the type rules ─────────────────────────────────────
    req = {
        must       = { type = "string",  required = true },
        typed_int  = { type = "integer", required = false, default = 0 },
        typed_bool = { type = "boolean", required = false, default = false },
        typed_str  = { type = "string",  required = false, default = "" },
    },

    -- ── the nine transforms ──────────────────────────────────────────────
    xf = {
        hex     = { type = "string", required = false, default = "", transform = "hex" },
        b64     = { type = "string", required = false, default = "", transform = "base64" },
        b64url  = { type = "string", required = false, default = "", transform = "base64url" },
        b32     = { type = "string", required = false, default = "", transform = "base32" },
        upper   = { type = "string", required = false, default = "", transform = "toupper" },
        lower   = { type = "string", required = false, default = "", transform = "tolower" },
        sub     = { type = "string", required = false, default = "", transform = "varsub" },
        comp    = { type = "string", required = false, default = "", transform = "compress" },
        rx      = { type = "string", required = false, default = "", transform = "regex/^a+/Z/" },
        both    = { type = "string", required = false, default = "", transform = { "toupper", "tolower" } },
    },

    -- ── the named format validators ──────────────────────────────────────
    v = {
        ip4     = { type = "string",  required = false, default = "0.0.0.0",
                    validate = { type = "ip4" } },
        ip6     = { type = "string",  required = false, default = "::1",
                    validate = { type = "ip6" } },
        email   = { type = "string",  required = false, default = "a@b.co",
                    validate = { type = "email" } },
        url     = { type = "string",  required = false, default = "https://h/",
                    validate = { type = "url" } },
        fqdn    = { type = "string",  required = false, default = "a.b.c",
                    validate = { type = "fqdn" } },
        date    = { type = "string",  required = false, default = "2024-01-01",
                    validate = { type = "date" } },
        size    = { type = "string",  required = false, default = "1MiB",
                    validate = { type = "file_size" } },
        cidr    = { type = "string",  required = false, default = "10.0.0.0/8",
                    validate = { type = "cidr" } },
        netmask = { type = "string",  required = false, default = "255.0.0.0/8",
                    validate = { type = "netmask.cidr" } },
        -- ── the declared constraints ─────────────────────────────────────
        enum    = { type = "string",  required = false, default = "B",
                    validate = { one_of = { "A", "B", "C" } } },
        pattern = { type = "string",  required = false, default = "abc",
                    validate = { regex = "^[a-z]+$" } },
        ranged  = { type = "integer", required = false, default = 5, min = 1, max = 10 },
        sized   = { type = "string",  required = false, default = "abc",
                    length = { min = 1, max = 5 } },
        list    = { type = "array",   required = false, array_max = 3 },
    },

    -- The table the document binds once and reaches by name.  Declared here
    -- because it *is* part of this application's configuration -- an
    -- undeclared top-level binding would be an unknown field in strict mode,
    -- which is the right answer for a typo and the wrong one for this.
    shared_timeouts = {
        connected = { type = "integer", required = false, default = 0 },
        suspended = { type = "integer", required = false, default = 0 },
    },

    -- ── nesting, and a table reached by alias ────────────────────────────
    nest = {
        deep = {
            leaf = { type = "integer", required = false, default = 0 },
        },
        shared = {
            connected = { type = "integer", required = false, default = 0 },
            suspended = { type = "integer", required = false, default = 0 },
        },
    },
}
return schema
