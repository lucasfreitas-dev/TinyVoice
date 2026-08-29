CREATE EXTENSION IF NOT EXISTS "pgcrypto";

CREATE TABLE devices (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT NOT NULL,
    token_hash TEXT NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT true,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_seen_at TIMESTAMPTZ
);

CREATE TABLE conversations (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name TEXT NOT NULL,
    whatsapp_recipient TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE device_conversations (
    device_id UUID NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    conversation_id UUID NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    PRIMARY KEY (device_id, conversation_id)
);

CREATE TABLE messages (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    conversation_id UUID NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    device_id UUID NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    direction TEXT NOT NULL CHECK (direction IN ('OUTBOUND', 'INBOUND')),
    status TEXT NOT NULL CHECK (status IN ('PENDING', 'PROCESSING', 'SENT', 'AVAILABLE', 'PLAYED', 'FAILED')),
    audio_storage_key TEXT,
    audio_duration_ms INTEGER,
    audio_size_bytes BIGINT,
    mime_type TEXT,
    attempts INTEGER NOT NULL DEFAULT 0,
    last_error TEXT,
    external_id TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    sent_at TIMESTAMPTZ,
    received_at TIMESTAMPTZ,
    played_at TIMESTAMPTZ
);

CREATE INDEX idx_messages_device_status ON messages (device_id, status);
CREATE INDEX idx_messages_pending ON messages (status, created_at) WHERE status = 'PENDING';
CREATE INDEX idx_messages_available ON messages (device_id, status) WHERE status = 'AVAILABLE';

CREATE TABLE webhook_events (
    id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    provider TEXT NOT NULL,
    external_id TEXT NOT NULL UNIQUE,
    processed_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
