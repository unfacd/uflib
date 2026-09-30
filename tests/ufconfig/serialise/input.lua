-- The fixed input.  Every value is distinctive so that a serialiser which drops
-- one, or emits the wrong one, is visible in the diff rather than merely
-- producing output of a different length.
shared = { a_v = 11, b_v = "aliased" }

s = {
    int_v   = -42,
    float_v = 3.5,
    bool_v  = true,
    str_v   = "plain text",
    quoted  = "has \"quotes\" and a \\ backslash",
    list_v  = { "alpha", "beta", "gamma" },
}

t = {
    nested = { deep_v = 7 },
    alias_table = shared,
}
return { s = s, t = t }
