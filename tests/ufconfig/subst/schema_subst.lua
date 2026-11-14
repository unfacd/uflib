-- Substitution, in isolation.  The values are supplied by the caller through
-- UfConfigLoadOptions.subst_get, so every case here is about the seam rather
-- than about what a particular value means.

local schema = {
    sub = {
        plain    = { type = "string", required = false, default = "", transform = "varsub" },
        braced   = { type = "string", required = false, default = "", transform = "varsub" },
        escaped  = { type = "string", required = false, default = "", transform = "varsub" },
        subfirst = { type = "string", required = false, default = "",
                     transform = { "varsub", "toupper" } },
        firstsub = { type = "string", required = false, default = "",
                     transform = { "toupper", "varsub" } },
        plainlit = { type = "string", required = false, default = "" },
        nested   = { type = "string", required = false, default = "", transform = "varsub" },
        cycle    = { type = "string", required = false, default = "", transform = "varsub" },
        fixed    = { type = "string", required = false, transform = "varsub",
                     validate = { one_of = { "A", "B" } } }
    }
}
return schema
