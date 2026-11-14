-- varsub, in isolation: the document names a value the caller supplies, and
-- the harness passes a callback for it.  Its own document so a refusal here is
-- a reported line rather than something that stops the rest of the run.
return { xf = { sub = "port=$${PORT}" } }
