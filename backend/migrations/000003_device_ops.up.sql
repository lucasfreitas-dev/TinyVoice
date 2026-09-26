CREATE TABLE firmware_releases (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    version TEXT NOT NULL UNIQUE,
    storage_key TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size_bytes BIGINT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE device_runtime (
    device_id UUID PRIMARY KEY REFERENCES devices(id) ON DELETE CASCADE,
    firmware_version TEXT,
    desired_firmware_id UUID REFERENCES firmware_releases(id) ON DELETE SET NULL,
    last_rssi INTEGER,
    last_free_heap INTEGER,
    last_uptime_ms BIGINT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE device_logs (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    device_id UUID NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    ts_ms BIGINT,
    level TEXT NOT NULL DEFAULT 'info',
    message TEXT NOT NULL,
    firmware_version TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_device_logs_device_created
    ON device_logs (device_id, created_at DESC);
