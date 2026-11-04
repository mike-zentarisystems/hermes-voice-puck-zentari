#include <assert.h>
#include <string.h>
#include "zv_app_state.h"
#include "zv_event_queue.h"
#include "zv_link_supervisor.h"

static uint32_t fixed_random(void *ctx) { (void)ctx; return 7u; }

static void test_state_machine(void)
{
    zv_app_state_t next = ZV_STATE_BOOT;
    zv_event_t event = {.type = ZV_EVT_BOOT_COMPLETE};
    assert(zv_app_state_next(ZV_STATE_BOOT, &event, &next) == ZV_OK);
    assert(next == ZV_STATE_PROVISIONING);
    event.type = ZV_EVT_PROVISIONING_DONE;
    assert(zv_app_state_next(next, &event, &next) == ZV_OK);
    assert(next == ZV_STATE_CONNECTING);
}

static void test_critical_queue(void)
{
    zv_event_t slots[2];
    zv_event_queue_t queue;
    assert(zv_event_queue_init(&queue, slots, 2) == ZV_OK);
    zv_event_t telemetry = {.type = ZV_EVT_AUDIO_LEVEL};
    zv_event_t critical = {.type = ZV_EVT_ERROR};
    assert(zv_event_queue_push(&queue, &telemetry) == ZV_OK);
    assert(zv_event_queue_push(&queue, &telemetry) == ZV_OK);
    assert(zv_event_queue_push(&queue, &critical) == ZV_OK);
    assert(queue.dropped == 1u);
}

static void test_link_policy(void)
{
    zv_link_supervisor_t link;
    const zv_link_config_t cfg = {.backoff_base_ms=100, .backoff_max_ms=800,
      .breaker={.failure_threshold=2,.open_duration_ms=1000,.half_open_max_trials=1},
      .degraded_after_failures=2};
    bool attempt = false;
    assert(zv_link_init(&link, &cfg, fixed_random, NULL, 0) == ZV_OK);
    assert(zv_link_poll(&link, 0, &attempt) == ZV_OK && attempt);
    assert(zv_link_on_failure(&link, 1) == ZV_OK);
    assert(zv_link_state(&link) == ZV_LINK_OFFLINE);
    assert(zv_link_poll(&link, 1000, &attempt) == ZV_OK && attempt);
    assert(zv_link_on_failure(&link, 1001) == ZV_OK);
    assert(zv_link_state(&link) == ZV_LINK_DEGRADED);
}

int main(void) { test_state_machine(); test_critical_queue(); test_link_policy(); return 0; }
