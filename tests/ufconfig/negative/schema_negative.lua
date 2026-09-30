-- A schema built to be violated.  Every rule the loader can enforce has a
-- field here, so a document breaking exactly one rule can be aimed at exactly
-- one field and the status it produces is unambiguous.
local schema = {
    n = {
        -- required, so its absence is a REQUIRED and not something else
        must        = { type = "string",  required = true },

        typed_int   = { type = "integer", required = false, default = 0 },
        typed_bool  = { type = "boolean", required = false, default = false },
        ranged      = { type = "integer", required = false, default = 5, min = 1, max = 10 },
        sized       = { type = "string",  required = false, default = "abc",
                        length = { min = 1, max = 5 } },
        enum        = { type = "string",  required = false, default = "B",
                        validate = { one_of = { "A", "B", "C" } } },
        pattern     = { type = "string",  required = false, default = "abc",
                        validate = { regex = "^[a-z]+$" } },
        ip4         = { type = "string",  required = false, default = "0.0.0.0",
                        validate = { type = "ip4" } },
        ip6         = { type = "string",  required = false, default = "::1",
                        validate = { type = "ip6" } },
        fqdn        = { type = "string",  required = false, default = "a.b.c",
                        validate = { type = "fqdn" } },
        date        = { type = "string",  required = false, default = "2024-01-01",
                        validate = { type = "date" } },
        cidr        = { type = "string",  required = false, default = "10.0.0.0/8",
                        validate = { type = "cidr" } },
        size        = { type = "string",  required = false, default = "1MiB",
                        validate = { type = "file_size" } },
        list        = { type = "array",   required = false, array_max = 3 },
        frozen      = { type = "integer", required = false, default = 1, mutable = false },
    }
}
return schema
