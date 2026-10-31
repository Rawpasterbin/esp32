-- Per-member command rate limiting.
-- A compromised session must not be able to toggle the valve continuously.
-- Window/count rows are small; the Worker cleans stale rows opportunistically.

CREATE TABLE IF NOT EXISTS rate_limits (
  key          TEXT PRIMARY KEY,
  window_start INTEGER NOT NULL,
  count        INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_rate_limits_window ON rate_limits(window_start);
