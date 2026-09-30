-- varsub, in isolation.  Expected to FAIL today: UfConfigLoadOptions.subst_get
-- is declared and documented but never consulted by the implementation, so the
-- transform always reports SUBST_UNRESOLVED whatever the caller supplies.
-- Kept as its own document so the failure is a reported line rather than
-- something that stops the rest of the harness from running.
return { xf = { sub = "port=$${PORT}" } }
