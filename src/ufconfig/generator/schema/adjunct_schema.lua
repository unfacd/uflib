local schema = {
    ufsrvwebsock = {
        ufsrvwebsock_var1             = { type = "integer", required = false, default = 1970 },
        max_frame_size              = { type = "integer", required = false, default = 65536,   min = 256 },
        max_message_size            = { type = "integer", required = false, default = 1048576, min = 256 },
        heartbeat_interval_secs     = { type = "integer", required = false, default = 30,      min = 5 },
        close_timeout_secs          = { type = "integer", required = false, default = 10,      min = 1 },
        max_connections_per_session = { type = "integer", required = false, default = 1,       min = 1 }
    },
    ufprobe = {
        cidr = { type = "string", required = false, default = "10.0.0.0/8",
                 validate = { type = "cidr" } },
        tag  = { type = "string", required = false, default = "ab",
                 length = { min = 1, max = 8 }, validate = { regex = "^[a-z]+$" } },
        items = { type = "array", required = false, array_max = 16 }
    }
}
return schema
