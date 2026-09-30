-- UfCommand user configuration (Lua-compatible)
return {
  version = 1,
  key_config = { default_modifiers = { "CTRL", "SHIFT" } },
  aliases = {
    ["co"] = "checkout $1",
    ["ra"] = "remote add $1 $2",
    ["say"] = "echo $*",
    ["st"] = "status",
  },
  bindings = {
    { modifiers = { "CTRL", "SHIFT" }, key = "P", command = "status" },
  },
}
