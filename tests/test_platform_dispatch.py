"""Compile the actual startup dispatcher with hardware calls replaced by probes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('gcc'), 'native GCC required')
class PlatformDispatchTests(unittest.TestCase):
    def run_mode(self, definitions, expected):
        source = (ROOT / 'components/zv_platform/src/zv_platform.c').read_text()
        # The dispatcher is the final function; compile its real preprocessor
        # branches rather than duplicating mode selection in the test.
        dispatcher = source[source.index('zv_platform_report_t zv_platform_start(void)'):]
        harness = r'''
#include <assert.h>
#include <string.h>
#include "zv_platform.h"
static char calls[8];
static unsigned count;
static zv_status_t ui_result, action_result;
zv_status_t zv_ui_start(void) { calls[count++]='U'; return ui_result; }
zv_status_t zv_ota_start(void) { calls[count++]='O'; return action_result; }
zv_status_t zv_gateway_start(void) { calls[count++]='G'; return action_result; }
'''
        main = r'''
int main(void) {
    zv_platform_report_t report = zv_platform_start();
    assert(!report.ready);
    assert(report.status == ZV_ERR_HARDWARE_GATE);
    assert(strcmp(calls, EXPECTED_CALLS) == 0);
#if CONFIG_ZV_M4_OTA_MODE
    assert(strstr(report.detail, "M4") != NULL || strstr(report.detail, "signed A/B OTA") != NULL);
    memset(calls, 0, sizeof(calls)); count=0;
    ui_result=ZV_ERR_INVALID_ARG;
    report=zv_platform_start();
    assert(!report.ready && report.status == ZV_ERR_INVALID_ARG);
    assert(strcmp(calls, "U") == 0); /* Failed physical gate cannot start OTA. */
    memset(calls, 0, sizeof(calls)); count=0;
    ui_result=ZV_OK; action_result=ZV_ERR_INVALID_ARG;
    report=zv_platform_start();
    assert(!report.ready && report.status == ZV_ERR_INVALID_ARG);
    assert(strcmp(calls, "UO") == 0);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program, executable = directory / 'dispatch.c', directory / 'dispatch'
            program.write_text(harness + dispatcher + main)
            command = ['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                       '-I', str(ROOT / 'components/zv_platform/include'),
                       '-I', str(ROOT / 'components/zv_core/include'),
                       '-DEXPECTED_CALLS="' + expected + '"']
            command += ['-D' + name + '=' + str(value) for name, value in definitions.items()]
            subprocess.run(command + [str(program), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_ota_dependency_does_not_shadow_touch_and_update(self):
        self.run_mode({'CONFIG_ZV_M4_OTA_MODE': 1, 'CONFIG_ZV_M2_WSS_MODE': 1,
                       'CONFIG_ZV_M0_ACCEPTANCE_MODE': 1}, 'UO')

    def test_voice_mode_retains_gateway_dispatch(self):
        self.run_mode({'CONFIG_ZV_M3A_VOICE_MODE': 1, 'CONFIG_ZV_M2_WSS_MODE': 1}, 'G')

    def test_wss_mode_retains_gateway_dispatch(self):
        self.run_mode({'CONFIG_ZV_M2_WSS_MODE': 1}, 'G')

    def test_default_mode_remains_gated_without_hardware_calls(self):
        self.run_mode({}, '')


if __name__ == '__main__':
    unittest.main()
