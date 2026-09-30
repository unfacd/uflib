-- Schema for the encrypted-secret tests.
--
-- Its shape is chosen so that each encrypted field tests a different property,
-- rather than there being one encrypted field and a set of variations on it.
--
--   app.db_password   encrypted, and constrained by one_of against its
--                     PLAINTEXT.  A ciphertext can never be a member of a
--                     one_of list, so a document that loads proves the value was
--                     decrypted before it was validated rather than after.
--
--   app.api_token     encrypted and unconstrained.  The tamper and malformed
--                     envelope cases work on this one, so a rejected value is
--                     never also a validator failure and the two causes stay
--                     distinguishable.
--
--   app.short_secret  encrypted and length-constrained.  Its length bound is
--                     measured against the decrypted value, which is what lets
--                     one fixture pass and another fail on the plaintext's
--                     length alone -- the second exists to show that a
--                     rejection never quotes the secret it rejected.
--
--   app.db_address    an ordinary field with a default: the control.  A schema
--                     mixing encrypted and plain fields must load plain ones
--                     exactly as it did before.
--
-- Note what is absent.  There is no `default` on any encrypted field and none is
-- mutable: the generator refuses both, and the cases asserting that live in the
-- generator's own test rather than here.

local schema = {
    app = {
        required     = true,
        db_password  = { type = "string", required = false, encrypted = true,
                         validate = { one_of = { "correct-horse" } } },
        api_token    = { type = "string", required = false, encrypted = true },
        short_secret = { type = "string", required = false, encrypted = true,
                         length = { min = 8 } },
        db_address   = { type = "string", required = false, default = "127.0.0.1" },
    }
}
return schema
