DROP INDEX IF EXISTS idx_conversations_channel_recipient;

ALTER TABLE conversations DROP CONSTRAINT IF EXISTS conversations_channel_check;

ALTER TABLE conversations DROP COLUMN channel;

ALTER TABLE conversations
    RENAME COLUMN recipient TO whatsapp_recipient;
