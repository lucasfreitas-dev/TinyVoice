ALTER TABLE conversations
    RENAME COLUMN whatsapp_recipient TO recipient;

ALTER TABLE conversations
    ADD COLUMN channel TEXT NOT NULL DEFAULT 'whatsapp';

ALTER TABLE conversations
    ADD CONSTRAINT conversations_channel_check
    CHECK (channel IN ('whatsapp', 'telegram'));

CREATE INDEX idx_conversations_channel_recipient
    ON conversations (channel, recipient);
