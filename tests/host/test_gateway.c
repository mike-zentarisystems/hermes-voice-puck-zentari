/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "zv_gateway.h"
#include "zv_identity.h"
#include "zv_test.h"

static void test_origin_policy(void)
{
    zv_gateway_endpoint_t ep;
    ZV_CHECK_EQ(zv_gateway_parse_origin("wss://gateway.invalid/ws", &ep), ZV_OK);
    ZV_CHECK_STR(ep.host, "gateway.invalid");
    ZV_CHECK_STR(ep.path, "/ws");
    ZV_CHECK_EQ(ep.port, 443);
    ZV_CHECK(ep.use_tls);

    ZV_CHECK_EQ(zv_gateway_parse_origin("wss://192.0.2.10:8443/ws", &ep), ZV_OK);
    ZV_CHECK_STR(ep.host, "192.0.2.10");
    ZV_CHECK_EQ(ep.port, 8443);

    ZV_CHECK_EQ(zv_gateway_parse_origin("ws://gateway.invalid/ws", &ep), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_gateway_parse_origin("https://gateway.invalid", &ep), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_gateway_parse_origin("wss://user:pass@gateway.invalid/", &ep), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_gateway_parse_origin("wss://gateway.invalid/ws?x=1", &ep), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_gateway_parse_origin("wss://gateway.invalid/ws#frag", &ep), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_gateway_parse_origin("wss://", &ep), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_gateway_parse_origin(NULL, &ep), ZV_ERR_INVALID_ARG);
}

static void test_origin_matches(void)
{
    /* Same endpoint, including the default path and host case. */
    ZV_CHECK(zv_gateway_origin_matches("wss://192.0.2.2:8443/", "wss://192.0.2.2:8443/"));
    ZV_CHECK(zv_gateway_origin_matches("wss://192.0.2.2:8443", "wss://192.0.2.2:8443/"));
    ZV_CHECK(zv_gateway_origin_matches("wss://Gateway.Invalid/ws", "wss://gateway.invalid:443/ws"));
    /* 2026-10-14: a build pointed at the lab gateway while the credential was enrolled for a different gateway. */
    ZV_CHECK(!zv_gateway_origin_matches("wss://192.0.2.10:8443/", "wss://192.0.2.2:8443/"));
    ZV_CHECK(!zv_gateway_origin_matches("wss://192.0.2.2:8444/", "wss://192.0.2.2:8443/"));
    ZV_CHECK(!zv_gateway_origin_matches("wss://192.0.2.2:8443/ws", "wss://192.0.2.2:8443/"));
    /* Anything that fails origin policy never matches. */
    ZV_CHECK(!zv_gateway_origin_matches("ws://192.0.2.2:8443/", "ws://192.0.2.2:8443/"));
    ZV_CHECK(!zv_gateway_origin_matches(NULL, "wss://192.0.2.2:8443/"));
    ZV_CHECK(!zv_gateway_origin_matches("wss://192.0.2.2:8443/", ""));
}

static void test_hello_frame(void)
{
    char buf[ZV_HELLO_JSON_MAX];
    const char *uuid = "c25bd6fd-0b0d-4f62-b4d7-f5a24ba52967";
    ZV_CHECK(zv_hello_id_is_uuid(uuid));
    ZV_CHECK(!zv_hello_id_is_uuid("not-a-uuid"));
    ZV_CHECK(!zv_hello_id_is_uuid("c25bd6fd-0b0d-4f62-b4d7-f5a24ba5296"));

    ZV_CHECK_EQ(zv_hello_build(buf, sizeof(buf), uuid, "puck-lab-0001", "0.1.0"), ZV_OK);
    ZV_CHECK(strstr(buf, "\"v\":1") != NULL);
    ZV_CHECK(strstr(buf, "\"type\":\"device.hello\"") != NULL);
    ZV_CHECK(strstr(buf, uuid) != NULL);
    ZV_CHECK(strstr(buf, "puck-lab-0001") != NULL);
    ZV_CHECK(strstr(buf, ZV_BOARD_ID) != NULL);
    ZV_CHECK(strstr(buf, "0.1.0") != NULL);

    ZV_CHECK_EQ(zv_hello_build(buf, sizeof(buf), "bad", "puck-lab-0001", "0.1.0"), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_hello_build(buf, sizeof(buf), uuid, "Puck-Lab-0001", "0.1.0"), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_hello_build(buf, sizeof(buf), uuid, "puck-lab-0001", "0.1"), ZV_ERR_POLICY);
    ZV_CHECK_EQ(zv_hello_build(buf, 16, uuid, "puck-lab-0001", "0.1.0"), ZV_ERR_BUFFER_TOO_SMALL);
}

void test_suite_gateway(void)
{
    test_origin_policy();
    test_origin_matches();
    test_hello_frame();
}
