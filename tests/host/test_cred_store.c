/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "zv_cred_store.h"
#include "zv_test.h"

static zv_device_credential_t make_cred(uint32_t gen)
{
    zv_device_credential_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.device_id, "puck-lab-0001", 14);
    memcpy(c.gateway_origin, "wss://gateway.invalid/ws", 25);
    memcpy(c.secret, "fixture-nonsecret-00000000000000000000000000000000", 50);
    c.secret_len = 50;
    c.scope_mask = (1u << ZV_SCOPE_SESSION_CONNECT) | (1u << ZV_SCOPE_AUDIO_STREAM);
    c.generation = gen;
    c.expires_at_s = 4102444800;
    return c;
}

static void test_validate(void)
{
    zv_device_credential_t c = make_cred(1);
    ZV_CHECK_EQ(zv_cred_validate(&c), ZV_OK);

    c = make_cred(1);
    c.generation = 0;
    ZV_CHECK_EQ(zv_cred_validate(&c), ZV_ERR_POLICY);

    c = make_cred(1);
    c.scope_mask = 0;
    ZV_CHECK_EQ(zv_cred_validate(&c), ZV_ERR_POLICY);

    c = make_cred(1);
    memcpy(c.gateway_origin, "http://gateway.invalid", 23);
    c.gateway_origin[23] = '\0';
    ZV_CHECK_EQ(zv_cred_validate(&c), ZV_ERR_POLICY);

    c = make_cred(1);
    c.secret_len = 8;
    ZV_CHECK_EQ(zv_cred_validate(&c), ZV_ERR_POLICY);

    c = make_cred(1);
    c.device_id[0] = 'P'; /* uppercase not allowed */
    ZV_CHECK_EQ(zv_cred_validate(&c), ZV_ERR_POLICY);

    ZV_CHECK_EQ(zv_cred_validate(NULL), ZV_ERR_INVALID_ARG);
}

static void test_public_format_never_leaks_secret(void)
{
    zv_device_credential_t c = make_cred(2);
    char buf[160];
    ZV_CHECK_EQ(zv_cred_format_public(&c, buf, sizeof(buf)), ZV_OK);
    ZV_CHECK(strstr(buf, "REDACTED") != NULL);
    ZV_CHECK(strstr(buf, "fixture-nonsecret") == NULL);
    ZV_CHECK(strstr(buf, "puck-lab-0001") != NULL);
    ZV_CHECK_EQ(zv_cred_format_public(&c, buf, 8), ZV_ERR_BUFFER_TOO_SMALL);
}

static void test_store_lifecycle(void)
{
    zv_cred_store_t store;
    zv_cred_store_reset(&store);
    ZV_CHECK(zv_cred_store_current(&store) == NULL);

    zv_device_credential_t c1 = make_cred(1);
    ZV_CHECK_EQ(zv_cred_store_stage(&store, &c1), ZV_ERR_INVALID_STATE);
    ZV_CHECK_EQ(zv_cred_store_install_active(&store, &c1), ZV_OK);
    ZV_CHECK(zv_cred_store_current(&store) != NULL);
    ZV_CHECK_EQ(zv_cred_store_current(&store)->generation, 1);

    zv_device_credential_t replay = make_cred(1);
    ZV_CHECK_EQ(zv_cred_store_install_active(&store, &replay), ZV_ERR_POLICY);

    zv_device_credential_t older = make_cred(1);
    older.generation = 1;
    ZV_CHECK_EQ(zv_cred_store_stage(&store, &older), ZV_ERR_POLICY);

    zv_device_credential_t c2 = make_cred(2);
    ZV_CHECK_EQ(zv_cred_store_stage(&store, &c2), ZV_OK);
    ZV_CHECK_EQ(zv_cred_store_current(&store)->generation, 2); /* prefer staged */
    ZV_CHECK_EQ(zv_cred_store_stage(&store, &c2), ZV_ERR_INVALID_STATE);

    zv_device_credential_t other = make_cred(3);
    memcpy(other.device_id, "puck-lab-9999", 14);
    ZV_CHECK_EQ(zv_cred_store_stage(&store, &c2), ZV_ERR_INVALID_STATE);
    (void)other;

    ZV_CHECK_EQ(zv_cred_store_drop_staged(&store), ZV_OK);
    ZV_CHECK_EQ(zv_cred_store_current(&store)->generation, 1);

    ZV_CHECK_EQ(zv_cred_store_stage(&store, &c2), ZV_OK);
    ZV_CHECK_EQ(zv_cred_store_commit_staged(&store), ZV_OK);
    ZV_CHECK_EQ(zv_cred_store_current(&store)->generation, 2);
    ZV_CHECK_EQ(zv_cred_store_drop_staged(&store), ZV_ERR_INVALID_STATE);
}

static void test_scope_names(void)
{
    zv_cred_scope_t s;
    ZV_CHECK_EQ(zv_cred_scope_from_name("session:connect", &s), ZV_OK);
    ZV_CHECK_EQ(s, ZV_SCOPE_SESSION_CONNECT);
    ZV_CHECK_EQ(zv_cred_scope_from_name("nope", &s), ZV_ERR_POLICY);
    ZV_CHECK_STR(zv_cred_scope_name(ZV_SCOPE_OTA_READ), "ota:read");
}

void test_suite_cred_store(void)
{
    test_validate();
    test_public_format_never_leaks_secret();
    test_store_lifecycle();
    test_scope_names();
}
