-- Persistence fixture for the end-to-end command harness.
return {
  version = 1,
  key_config = { default_modifiers = { "ALT" } },
  aliases = {
    ["pco"] = "checkout $1",
    ["pra"] = "remote add $1 $2",
    ["psay"] = "echo $*",
    ["pst"] = "status",
  },
  bindings = {
    { modifiers = { "ALT" }, key = "P", command = "status" },
    { modifiers = { "ALT" }, key = "C", command = "pco persisted" },
  },
}
