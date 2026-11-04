/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdio.h>
#include <string.h>

/* Minimal dependency-free assertion helpers for host tests. */

extern int zv_test_failures;
extern int zv_test_checks;

#define ZV_CHECK(cond)                                                                                    \
    do {                                                                                                  \
        zv_test_checks++;                                                                                 \
        if (!(cond)) {                                                                                    \
            zv_test_failures++;                                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                                 \
    } while (0)

#define ZV_CHECK_EQ(actual, expected)                                                                     \
    do {                                                                                                  \
        long long zv_a_ = (long long)(actual);                                                            \
        long long zv_e_ = (long long)(expected);                                                          \
        zv_test_checks++;                                                                                 \
        if (zv_a_ != zv_e_) {                                                                             \
            zv_test_failures++;                                                                           \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, expected %s == %lld\n", __FILE__,         \
                    __LINE__, #actual, zv_a_, #expected, zv_e_);                                          \
        }                                                                                                 \
    } while (0)

#define ZV_CHECK_STR(actual, expected)                                                                    \
    do {                                                                                                  \
        const char *zv_a_ = (actual);                                                                     \
        const char *zv_e_ = (expected);                                                                   \
        zv_test_checks++;                                                                                 \
        if (zv_a_ == NULL || strcmp(zv_a_, zv_e_) != 0) {                                                 \
            zv_test_failures++;                                                                           \
            fprintf(stderr, "%s:%d: CHECK_STR failed:\n  actual:   \"%s\"\n  expected: \"%s\"\n",          \
                    __FILE__, __LINE__, zv_a_ == NULL ? "(null)" : zv_a_, zv_e_);                         \
        }                                                                                                 \
    } while (0)

void test_suite_app_state(void);
void test_suite_event_queue(void);
void test_suite_backoff(void);
void test_suite_circuit_breaker(void);
void test_suite_link_supervisor(void);
void test_suite_link_acceptance(void);
void test_suite_redact(void);
void test_suite_lipsync(void);
void test_suite_avatar_model(void);
void test_suite_ui_draw(void);
void test_suite_volume(void);
void test_suite_approval(void);
void test_suite_trust_store(void);
void test_suite_device_command(void);
void test_suite_touch(void);
void test_suite_ota_policy(void);
void test_suite_ota_slot(void);
void test_suite_memory_budget(void);
void test_suite_cred_rotation(void);
void test_suite_cred_store(void);
void test_suite_gateway(void);
void test_suite_audio_probe(void);
