-- Water Valve API v2 — initial schema (Cloudflare D1 / SQLite)
-- Run locally:
--   wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
-- Run remotely after `wrangler d1 create water-valve-v2` and updating database_id:
--   wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql

PRAGMA foreign_keys = ON;

-- Household = one home + one valve in v1.
CREATE TABLE IF NOT EXISTS households (
  id            TEXT PRIMARY KEY,
  name          TEXT NOT NULL,
  timezone      TEXT NOT NULL DEFAULT 'Asia/Bangkok',
  invite_code   TEXT UNIQUE NOT NULL,
  password_hash TEXT NOT NULL,
  created_at    TEXT NOT NULL
);

-- A member is one app installation/person that joined with the invite code.
CREATE TABLE IF NOT EXISTS members (
  id           TEXT PRIMARY KEY,
  household_id TEXT NOT NULL REFERENCES households(id) ON DELETE CASCADE,
  display_name TEXT NOT NULL,
  created_at   TEXT NOT NULL
);

-- Opaque session token; only SHA-256 hash is stored.
CREATE TABLE IF NOT EXISTS sessions (
  id         TEXT PRIMARY KEY,
  member_id  TEXT NOT NULL REFERENCES members(id) ON DELETE CASCADE,
  token_hash TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  revoked_at TEXT
);

-- ESP32 device. Only SHA-256 hash of the device token is stored.
CREATE TABLE IF NOT EXISTS devices (
  id               TEXT PRIMARY KEY,
  household_id     TEXT NOT NULL REFERENCES households(id) ON DELETE CASCADE,
  name             TEXT NOT NULL,
  token_hash       TEXT NOT NULL,
  firmware_version TEXT,
  last_seen_at     TEXT,
  last_status_json TEXT,
  created_at       TEXT NOT NULL
);

-- One config row per household in v1. revision enables optimistic concurrency.
CREATE TABLE IF NOT EXISTS configs (
  household_id   TEXT PRIMARY KEY REFERENCES households(id) ON DELETE CASCADE,
  mode           TEXT NOT NULL CHECK (mode IN ('auto','manual')),
  schedules_json TEXT NOT NULL,
  revision       INTEGER NOT NULL DEFAULT 1,
  updated_at     TEXT NOT NULL,
  updated_by     TEXT REFERENCES members(id)
);

-- Commands are short-lived events, not permanent state.
CREATE TABLE IF NOT EXISTS commands (
  id              TEXT PRIMARY KEY,
  household_id    TEXT NOT NULL REFERENCES households(id) ON DELETE CASCADE,
  device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
  action          TEXT NOT NULL CHECK (action IN ('open','close')),
  status          TEXT NOT NULL DEFAULT 'pending'
                  CHECK (status IN ('pending','acked','expired','failed')),
  idempotency_key TEXT,
  created_by      TEXT REFERENCES members(id),
  created_at      TEXT NOT NULL,
  expires_at      TEXT NOT NULL,
  acked_at        TEXT,
  error           TEXT
);

CREATE TABLE IF NOT EXISTS audit_log (
  id           TEXT PRIMARY KEY,
  household_id TEXT NOT NULL,
  actor_type   TEXT NOT NULL CHECK (actor_type IN ('member','device','system')),
  actor_id     TEXT,
  action       TEXT NOT NULL,
  detail_json  TEXT,
  created_at   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_devices_household ON devices(household_id);
CREATE INDEX IF NOT EXISTS idx_members_household ON members(household_id);
CREATE INDEX IF NOT EXISTS idx_sessions_member ON sessions(member_id, expires_at);
CREATE INDEX IF NOT EXISTS idx_commands_pending ON commands(device_id, status, expires_at);
CREATE INDEX IF NOT EXISTS idx_commands_household_time ON commands(household_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_audit_household_time ON audit_log(household_id, created_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_commands_idempotency
  ON commands(household_id, idempotency_key)
  WHERE idempotency_key IS NOT NULL;
