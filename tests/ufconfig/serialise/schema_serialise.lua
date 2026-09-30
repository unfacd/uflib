-- A small schema for the serialiser payload tests.  Small on purpose: the
-- golden files beside it are meant to be read, and a value that goes missing
-- should be obvious in the diff rather than lost among a hundred lines.
local schema = {
    s = {
        int_v    = { type = "integer", required = false, default = 0 },
        float_v  = { type = "float",   required = false, default = 0.0 },
        bool_v   = { type = "boolean", required = false, default = false },
        str_v    = { type = "string",  required = false, default = "" },
        quoted   = { type = "string",  required = false, default = "" },
        list_v   = { type = "array",   required = false, array_max = 8 },
    },
    t = {
        nested = {
            deep_v = { type = "integer", required = false, default = 0 },
        },
        alias_table = {
            a_v = { type = "integer", required = false, default = 0 },
            b_v = { type = "string",  required = false, default = "" },
        },
    },
}
return schema
