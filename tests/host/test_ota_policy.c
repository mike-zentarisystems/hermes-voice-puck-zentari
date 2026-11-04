/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_identity.h"
#include "zv_ota_policy.h"
#include "zv_test.h"

static void copy_field(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void test_semver(void)
{
    zv_semver_t v;
    ZV_CHECK_EQ(zv_semver_parse("1.2.3", &v), ZV_OK);
    ZV_CHECK_EQ(v.major, 1);
    ZV_CHECK_EQ(v.minor, 2);
    ZV_CHECK_EQ(v.patch, 3);
    ZV_CHECK_EQ(zv_semver_parse("0.0.0", &v), ZV_OK);
    ZV_CHECK_EQ(zv_semver_parse("65535.0.10", &v), ZV_OK);

    const char *invalid[] = {"01.2.3", "1.2", "1.2.3-rc1", "v1.2.3", "1.2.3.4", "65536.0.0", "", "1..3", "1.2.x"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        ZV_CHECK_EQ(zv_semver_parse(invalid[i], &v), ZV_ERR_INVALID_ARG);
    }

    zv_semver_t a = {1, 2, 3};
    zv_semver_t b = {1, 10, 0};
    ZV_CHECK_EQ(zv_semver_compare(&a, &b), -1);
    ZV_CHECK_EQ(zv_semver_compare(&b, &a), 1);
    ZV_CHECK_EQ(zv_semver_compare(&a, &a), 0);
}

static zv_ota_manifest_t good_manifest(void)
{
    zv_ota_manifest_t m;
    memset(&m, 0, sizeof(m));
    copy_field(m.version, sizeof(m.version), "0.2.0");
    copy_field(m.board_id, sizeof(m.board_id), ZV_BOARD_ID);
    copy_field(m.channel, sizeof(m.channel), "stable");
    copy_field(m.sha256, sizeof(m.sha256), "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
    copy_field(m.signature_scheme, sizeof(m.signature_scheme), ZV_OTA_SIG_SCHEME_SECURE_BOOT_V2);
    m.secure_version = 2u;
    m.image_size = 3u * 1024u * 1024u;
    return m;
}

static const zv_ota_device_t k_device = {
    .running_version = "0.1.4",
    .board_id = ZV_BOARD_ID,
    .channel = "stable",
    .efuse_secure_version = 2u,
    .partition_size = 6u * 1024u * 1024u,
};

static void expect_reason(const zv_ota_manifest_t *m, zv_ota_reason_t expected)
{
    zv_ota_reason_t reason = ZV_OTA_REASON_NONE;
    ZV_CHECK_EQ(zv_ota_evaluate(m, &k_device, &reason), ZV_ERR_POLICY);
    ZV_CHECK_EQ(reason, expected);
}

static void test_evaluate(void)
{
    zv_ota_reason_t reason = ZV_OTA_REASON_BAD_MANIFEST;
    zv_ota_manifest_t m = good_manifest();
    ZV_CHECK_EQ(zv_ota_evaluate(&m, &k_device, &reason), ZV_OK);
    ZV_CHECK_EQ(reason, ZV_OTA_REASON_NONE);

    m = good_manifest();
    copy_field(m.board_id, sizeof(m.board_id), "waveshare-esp32-s3-touch-lcd-1.85c-box-v1");
    expect_reason(&m, ZV_OTA_REASON_BOARD_MISMATCH);

    m = good_manifest();
    copy_field(m.channel, sizeof(m.channel), "beta");
    expect_reason(&m, ZV_OTA_REASON_CHANNEL_MISMATCH);

    m = good_manifest();
    copy_field(m.signature_scheme, sizeof(m.signature_scheme), "none");
    expect_reason(&m, ZV_OTA_REASON_UNSIGNED);

    m = good_manifest();
    m.secure_version = 1u;
    expect_reason(&m, ZV_OTA_REASON_SECURE_VERSION_ROLLBACK);

    m = good_manifest();
    m.image_size = k_device.partition_size + 1u;
    expect_reason(&m, ZV_OTA_REASON_TOO_LARGE);

    m = good_manifest();
    copy_field(m.version, sizeof(m.version), "0.1.4");
    expect_reason(&m, ZV_OTA_REASON_NOT_NEWER);

    m = good_manifest();
    m.sha256[0] = 'Z';
    expect_reason(&m, ZV_OTA_REASON_BAD_MANIFEST);

    m = good_manifest();
    copy_field(m.version, sizeof(m.version), "0.2");
    expect_reason(&m, ZV_OTA_REASON_BAD_MANIFEST);

    zv_ota_device_t bad_device = k_device;
    bad_device.running_version = "garbage";
    m = good_manifest();
    ZV_CHECK_EQ(zv_ota_evaluate(&m, &bad_device, &reason), ZV_ERR_INVALID_ARG);
    ZV_CHECK_EQ(zv_ota_evaluate(NULL, &k_device, &reason), ZV_ERR_INVALID_ARG);
    ZV_CHECK_STR(zv_ota_reason_name(ZV_OTA_REASON_SECURE_VERSION_ROLLBACK), "secure_version_rollback");
}

void test_suite_ota_policy(void)
{
    test_semver();
    test_evaluate();
}
