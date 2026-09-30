-- db_password decrypts to 'not-in-the-list', which is not a member of the
-- one_of list its field declares.  The one_of rejection is one of the messages
-- that interpolates the value it refused, so this fixture is what proves the
-- value is masked for an encrypted field: without the mask the plaintext would
-- appear verbatim in UfConfigLastError().
app = {
    db_password  = "enc:v1:38e3c42d940ffa62aff3ecaf:8e43cdff32459a1287f1d517e33c24:b6f183559ae33e8764981b42eb37016d",
    api_token    = "enc:v1:c1b2bdd0f58dfb2bfd204989:c985938dd38f8054d81a7372:22a6fe4b859a15e9225541d7a47e62ad",
    short_secret = "enc:v1:82ca62f605b8aa3456a0bdc1:f845bf932b30f58f:b7f8096ba2c130cda8313fdfb2f1da19",
    db_address   = "10.0.0.5",
}
