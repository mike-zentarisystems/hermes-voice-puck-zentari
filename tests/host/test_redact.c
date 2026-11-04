/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_redact.h"
#include "zv_test.h"

static void expect_redacted(const char *input, const char *expected, size_t expected_count)
{
    char out[256];
    size_t count = 999u;
    ZV_CHECK_EQ(zv_redact(input, out, sizeof(out), &count), ZV_OK);
    ZV_CHECK_STR(out, expected);
    ZV_CHECK_EQ(count, expected_count);
}

static void test_keys_and_headers(void)
{
    expect_redacted("Authorization: Bearer abc.def-123", "Authorization: [REDACTED]", 1);
    expect_redacted("curl -H 'Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig'", "curl -H 'Bearer [REDACTED]'", 1);
    expect_redacted("{\"access_token\":\"zvd_live_9f8e7d\",\"expires_in\":3600}",
                    "{\"access_token\":\"[REDACTED]\",\"expires_in\":3600}", 1);
    expect_redacted("wifi psk=hunter22 ssid=Home", "wifi psk=[REDACTED] ssid=Home", 1);
    expect_redacted("password: s3cr3t", "password: [REDACTED]", 1);
    expect_redacted("x-api_key: shortval", "x-api_key: [REDACTED]", 1);
    expect_redacted("enrollment_code=482-913 pop='abc'", "enrollment_code=[REDACTED] pop='[REDACTED]'", 2);
    expect_redacted("\"token_type\":\"Bearer\"", "\"token_type\":\"Bearer\"", 0);
    expect_redacted("population=5 tokens=3", "population=5 tokens=3", 0);
}

static void test_opaque_tokens(void)
{
    expect_redacted("key sk-proj-AbCdEf0123456789xyz end", "key [REDACTED] end", 1);
    expect_redacted("jwt eyJhbGciOiJFUzI1NiJ9.eyJzdWIiOiJ4In0.c2ln", "jwt [REDACTED]", 1);
    expect_redacted("opaque AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCd end", "opaque [REDACTED] end", 1);

    const char *digest = "fw sha256 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08 ok";
    expect_redacted(digest, digest, 0);
    expect_redacted("mac 7c:9e:bd:3a:41:f0 rssi -61", "mac 7c:9e:bd:3a:41:f0 rssi -61", 0);
}

static void test_truncation_and_args(void)
{
    char small[12];
    size_t count = 0;
    ZV_CHECK_EQ(zv_redact("password=abcdef and more text", small, sizeof(small), &count), ZV_ERR_BUFFER_TOO_SMALL);
    ZV_CHECK_EQ(small[sizeof(small) - 1u], '\0');
    ZV_CHECK(strstr(small, "abcdef") == NULL);

    char out[8];
    ZV_CHECK_EQ(zv_redact(NULL, out, sizeof(out), NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_redact("x", NULL, sizeof(out), NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_redact("x", out, 0, NULL), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_redact("", out, sizeof(out), NULL), ZV_OK);
    ZV_CHECK_STR(out, "");
}

void test_suite_redact(void)
{
    test_keys_and_headers();
    test_opaque_tokens();
    test_truncation_and_args();
}
