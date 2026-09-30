-- Coverage schema: one field per transform and per validator, so that every
-- name in the catalogue is exercised by real data rather than by a unit test
-- calling the function directly.  Kept out of the shipped schema on purpose:
-- these fields exist to be tested, not to configure anything.
local schema = {
    xf = {
        -- ── the nine transforms ──────────────────────────────────────────
        hex       = { type = "string", required = false, default = "", transform = "hex" },
        b64       = { type = "string", required = false, default = "", transform = "base64" },
        b64url    = { type = "string", required = false, default = "", transform = "base64url" },
        b32       = { type = "string", required = false, default = "", transform = "base32" },
        upper     = { type = "string", required = false, default = "", transform = "toupper" },
        lower     = { type = "string", required = false, default = "", transform = "tolower" },
        sub       = { type = "string", required = false, default = "", transform = "varsub" },
        comp      = { type = "string", required = false, default = "", transform = "compress" },
        rx        = { type = "string", required = false, default = "", transform = "regex/^a+/Z/" },
        -- two transforms on one field, applied in order
        both      = { type = "string", required = false, default = "", transform = { "varsub", "toupper" } },
    },
    -- ── nested tables, three levels, and a scope reached by alias ────────
    nest = {
        level2 = {
            level3 = {
                leaf_int  = { type = "integer", required = false, default = 0 },
                leaf_str  = { type = "string",  required = false, default = "" },
                leaf_list = { type = "array",   required = false, array_max = 4 },
            },
            sibling = { type = "string", required = false, default = "" },
        },
        -- a second table with the same shape, reached by name in the document
        shared = {
            connected    = { type = "integer", required = false, default = 0 },
            suspended    = { type = "integer", required = false, default = 0 },
        },
    },
    v = {
        -- ── the named format validators ──────────────────────────────────
        ip4       = { type = "string", required = false, default = "0.0.0.0",   validate = { type = "ip4" } },
        ip6       = { type = "string", required = false, default = "::1",       validate = { type = "ip6" } },
        email     = { type = "string", required = false, default = "a@b.co",    validate = { type = "email" } },
        url       = { type = "string", required = false, default = "https://h/", validate = { type = "url" } },
        fqdn      = { type = "string", required = false, default = "a.b.c",     validate = { type = "fqdn" } },
        date      = { type = "string", required = false, default = "2024-02-29", validate = { type = "date" } },
        size      = { type = "string", required = false, default = "1MiB",      validate = { type = "file_size" } },
        cidr      = { type = "string", required = false, default = "10.0.0.0/8", validate = { type = "cidr" } },
        netmask   = { type = "string", required = false, default = "255.0.0.0/8", validate = { type = "netmask.cidr" } },
        -- ── the declared constraints ─────────────────────────────────────
        enum      = { type = "string", required = false, default = "B",
                      validate = { one_of = { "A", "B", "C" } } },
        pattern   = { type = "string", required = false, default = "abc",
                      validate = { regex = "^[a-z]+$" } },
        ranged    = { type = "integer", required = false, default = 5, min = 1, max = 10 },
        sized     = { type = "string", required = false, default = "abc",
                      length = { min = 1, max = 5 } },
        list      = { type = "array", required = false, array_max = 3 },
    }
}
return schema
