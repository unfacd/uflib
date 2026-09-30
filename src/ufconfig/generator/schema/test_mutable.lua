local schema = {
    testharn = {
        knob_a = { type = "integer", required = false, default = 1, mutable = true, min = 0, max = 100 },
        knob_b = { type = "string",  required = false, default = "x", mutable = true, transform = "toupper",
                   validate = { one_of = { "X", "Y" }, type = "fqdn" } },
        knob_c = { type = "boolean", required = false, default = false, mutable = true },
        knob_f = { type = "float",   required = false, default = 1.5, mutable = true }
    }
}
return schema
