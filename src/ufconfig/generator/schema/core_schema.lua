local schema = {
    ufsrv = {
        required = true,
        server_id             = { type = "integer", required = false, default = 0 },
        server_id_by_user     = { type = "integer", required = false, default = 0 },
        server_run_mode       = { type = "string",  required = false, default = "normal", transform = "toupper", validate = { one_of = { "NORMAL", "SHADOW" } } },
        server_cpu_affinity   = { type = "string",  required = false, default = "system_override" },
        intra_ufsrv_classname = { type = "string",  required = false, default = "ufsrv" },
        protocol_id                 = { type = "integer", required = false, default = 1 },
        ufsrv_geogroup = { type = "integer", required = true, min = 0 },
        main_listener_port           = { type = "integer", required = true,  min = 1, max = 65535 },
        main_listener_address        = { type = "string",  required = false, default = "127.0.0.1", validate = { type = "ip4" } },
        main_listener_protocol_id    = { type = "integer", required = false, default = 1 },
        command_console_port         = { type = "integer", required = false, default = 19700, min = 1, max = 65535 },
        command_console_address = { type = "string",  required = false, default = "127.0.0.1" },
        session_workers_thread_pool  = { type = "integer", required = false, default = 2, min = 1 },
        ufsrv_workers_thread_pool    = { type = "integer", required = false, default = 2, min = 1 },
        listener_workers_thread_pool = { type = "integer", required = false, default = 1, min = 1 },
        ssl_command_console = {
            certificate_file = { type = "string", required = false, default = "ufsrv_console_certificate.pem" },
            key_file         = { type = "string", required = false, default = "ufsrv_console.key" },
            is_enabled       = { type = "boolean", required = false, default = false },
            is_client_certificate_required = { type = "boolean", required = false, default = false},
            allow_list_god    = { type = "array",  required = false, default = {} },
            allow_list_browser= { type = "array",  required = false, default = {} },
            deny_list      = { type = "array",  required = false, default = {} }
        },
        ssl_command_console_client = {
            certificate_file = { type = "string",  required = false, default = "ufsrv_console_client_certificate_key.pem" },
            key_file         = { type = "string",  required = false, default = "ufsrv_console_client_key.key" },
            is_enabled       = { type = "boolean", required = false, default = false },
        },
        stats_backend = {
            address = { type = "string",  required = false, default = "127.0.0.1" },
            port    = { type = "integer", required = false, default = 8125, min = 1, max = 65535 }
        },
        user_timeouts = {
            unauthenticated_timeout = { type = "integer", required = false, default = 60,  min = 0 },
            connected_timeout       = { type = "integer", required = false, default = 120, min = 0 },
            suspended_timeout       = { type = "integer", required = false, default = 60,  min = 0 },
            locationless_timeout    = { type = "integer", required = false, default = 300, min = 0 }
        },
        buffer_sizes = {
            incoming_buffer_size    = { type = "integer", required = false, default = 1024,   min = 256 },
            incoming_buffer_maxsize = { type = "integer", required = false, default = 1024000, min = 1024 },
            outgoing_buffer_size    = { type = "integer", required = false, default = 1024,   min = 256 },
            holding_buffer_size     = { type = "integer", required = false, default = 1024,   min = 256 }
        },
        memory_specs_for_session = {
            sessions_per_allocation_group = { type = "integer", required = false, default = 1024, min = 64 },
            allocation_groups             = { type = "integer", required = false, default = 10,   min = 1 },
            allocation_threshold_trigger  = { type = "string",  required = false, default = "10%" }
        },
        fileloader = {
            is_enabled   = { type = "boolean", required = false, default = false },
            registry_size        = { type = "integer", required = false, default = 4096, min = 1, max = 1048576 },
            registry_hot_loaded_size  = { type = "integer", required = false, default = 64, min = 1, max = 1048576 },
            black_listed_files_registry   = { type = "array",  required = false, default = {} }
        },
        sys_runtime = {
            required = true,
            run_as_user = { type = "string", required = false, default = "" },
            chroot      = { type = "string", required = false, default = "/" }
        }
    },
    ufnet = {
        geoip_service = {
            address = { type = "string",  required = false, default = "127.0.0.1" },
            port    = { type = "integer", required = false, default = 19801, min = 1, max = 65535 }
        },
        msgqueue = {
            is_enabled   = { type = "boolean", required = false, default = false },
            address      = { type = "string",  required = false, default = "ufsrvmsgqueue.unfacd.com" },
            port         = { type = "integer", required = false, default = 6380, min = 1, max = 65535 }
        },
        db_backend = {
            port     = { type = "integer", required = true,  min = 1, max = 65535 },
            address  = { type = "string",  required = true },
            username = { type = "string",  required = true },
            password = { type = "string",  required = true }
        },
        persistence_backend = {
            address = { type = "string",  required = false, default = "127.0.0.1" },
            port    = { type = "integer", required = false, default = 19705, min = 1, max = 65535 },
            mode    = { type = "string",  required = false, default = "tcp" },
            timeout = { type = "integer", required = false, default = 500000, min = 0 }
        },
        cache_backend_usrmsg = {
            address = { type = "string",  required = false, default = "127.0.0.1" },
            port    = { type = "integer", required = false, default = 22001, min = 1, max = 65535 },
            mode    = { type = "string",  required = false, default = "tcp" },
            timeout = { type = "integer", required = false, default = 500000, min = 0 }
        },
        cache_backend_fence = {
            address = { type = "string",  required = false, default = "127.0.0.1" },
            port    = { type = "integer", required = false, default = 32001, min = 1, max = 65535 },
            mode    = { type = "string",  required = false, default = "tcp" },
            timeout = { type = "integer", required = false, default = 500000, min = 0 }
        },
    }
}
return schema
