-- ufconfig SQL backend schema (MariaDB / InnoDB)
-- One namespace row identifies the Redis-style namespace.
-- ufconfig_config is the single central table containing every user's config.
-- ufconfig_entry contains the flattened config fields.

CREATE TABLE IF NOT EXISTS ufconfig_namespace (
    namespace_id   BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    namespace_name VARCHAR(191)    NOT NULL,
    PRIMARY KEY (namespace_id),
    UNIQUE KEY uq_ufconfig_namespace_name (namespace_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS ufconfig_config (
    config_id      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    namespace_id   BIGINT UNSIGNED NOT NULL,
    config_name    VARCHAR(191)    NOT NULL,
    format_version SMALLINT UNSIGNED NOT NULL DEFAULT 1,
    version        BIGINT UNSIGNED NOT NULL DEFAULT 0,
    schema_hash    BIGINT UNSIGNED NOT NULL DEFAULT 0,
    digest         BINARY(32)      NULL,
    created_at     TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at     TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (config_id),
    UNIQUE KEY uq_ufconfig_config_identity (namespace_id, config_name),
    CONSTRAINT fk_ufconfig_config_namespace
        FOREIGN KEY (namespace_id) REFERENCES ufconfig_namespace(namespace_id)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS ufconfig_entry (
    config_id BIGINT UNSIGNED NOT NULL,
    path      VARBINARY(512)   NOT NULL,
    vtype     TINYINT UNSIGNED NOT NULL,
    value     MEDIUMTEXT      NOT NULL,
    PRIMARY KEY (config_id, path),
    CONSTRAINT fk_ufconfig_entry_config
        FOREIGN KEY (config_id) REFERENCES ufconfig_config(config_id)
        ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
