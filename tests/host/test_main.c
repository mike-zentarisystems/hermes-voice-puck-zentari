/*
 * SPDX-FileCopyrightText: 2026 Zentari Systems
 * SPDX-License-Identifier: Apache-2.0
 */
#include "zv_test.h"

int zv_test_failures = 0;
int zv_test_checks = 0;

typedef struct {
    const char *name;
    void (*run)(void);
} suite_t;

static const suite_t k_suites[] = {
    {"app_state", test_suite_app_state},
    {"event_queue", test_suite_event_queue},
    {"backoff", test_suite_backoff},
    {"circuit_breaker", test_suite_circuit_breaker},
    {"link_supervisor", test_suite_link_supervisor},
    {"link_acceptance", test_suite_link_acceptance},
    {"redact", test_suite_redact},
    {"lipsync", test_suite_lipsync},
    {"avatar_model", test_suite_avatar_model},
    {"ui_draw", test_suite_ui_draw},
    {"volume", test_suite_volume},
    {"approval", test_suite_approval},
    {"trust_store", test_suite_trust_store},
    {"device_command", test_suite_device_command},
    {"touch", test_suite_touch},
    {"ota_policy", test_suite_ota_policy},
    {"ota_slot", test_suite_ota_slot},
    {"memory_budget", test_suite_memory_budget},
    {"cred_rotation", test_suite_cred_rotation},
    {"cred_store", test_suite_cred_store},
    {"gateway", test_suite_gateway},
    {"audio_probe", test_suite_audio_probe},
};

int main(int argc, char **argv)
{
    size_t suite_count = sizeof(k_suites) / sizeof(k_suites[0]);
    const char *only = (argc > 1) ? argv[1] : NULL;
    int ran = 0;

    for (size_t i = 0; i < suite_count; i++) {
        if (only != NULL && strcmp(only, k_suites[i].name) != 0) {
            continue;
        }
        int failures_before = zv_test_failures;
        k_suites[i].run();
        printf("[%s] %s\n", zv_test_failures == failures_before ? "PASS" : "FAIL", k_suites[i].name);
        ran++;
    }

    if (ran == 0) {
        fprintf(stderr, "ERROR: unknown test suite '%s'\n", only);
        return 2;
    }
    printf("%d suite(s), %d check(s), %d failure(s)\n", ran, zv_test_checks, zv_test_failures);
    return zv_test_failures == 0 ? 0 : 1;
}
