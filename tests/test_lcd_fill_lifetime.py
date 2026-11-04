"""Exercise the real fill function with a delayed display DMA completion."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('gcc'), 'native GCC required')
class LcdFillLifetimeTests(unittest.TestCase):
    def test_timeout_keeps_dma_storage_and_blocks_further_writes(self):
        source = (ROOT / 'components/zv_board/src/zv_board.c').read_text()
        function = source[source.index('esp_err_t zv_board_lcd_fill_rect('):
                          source.index('esp_err_t zv_board_lcd_draw_row(')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT };
enum { LCD_WIDTH=360, LCD_HEIGHT=360, LCD_FILL_LINES=16, MALLOC_CAP_DMA=1, pdTRUE=1 };
#define pdMS_TO_TICKS(value) (value)
#define ESP_RETURN_ON_FALSE(condition,error,tag,message) do { if (!(condition)) return error; } while (0)
static void *s_lcd_panel=(void *)1, *s_lcd_trans_done=(void *)2;
static bool s_sprite_dma_fault, pending, timeout, reject;
static unsigned allocations, frees, transfers;
static uint16_t storage[LCD_WIDTH*LCD_FILL_LINES];
void *heap_caps_malloc(size_t size, int flags) {
    assert(!pending && size<=sizeof(storage) && flags==MALLOC_CAP_DMA);
    allocations++; return storage;
}
void checked_free(void *buffer) {
    assert(buffer==storage && !pending); frees++;
}
#define free checked_free
int esp_lcd_panel_draw_bitmap(void *panel,int x0,int y0,int x1,int y1,void *buffer) {
    assert(panel==s_lcd_panel && !pending && buffer==storage);
    assert(x0==0 && x1==2 && y0>=0 && y1<=18 && y1>y0);
    assert(storage[0]==0x3412);
    transfers++; if (reject) return ESP_ERR_INVALID_ARG;
    pending=true; return ESP_OK;
}
int xSemaphoreTake(void *semaphore,int ticks) {
    assert(semaphore==s_lcd_trans_done && ticks==1000 && pending);
    if (timeout) return 0;
    pending=false; return pdTRUE;
}
'''
        main = r'''
int main(void) {
    assert(zv_board_lcd_fill_rect(0,0,2,18,0x1234)==ESP_OK);
    assert(allocations==1 && frees==1 && transfers==2 && !pending);
    reject=true;
    assert(zv_board_lcd_fill_rect(0,0,2,18,0x1234)==ESP_ERR_INVALID_ARG);
    assert(allocations==2 && frees==2 && !pending);
    reject=false; timeout=true;
    assert(zv_board_lcd_fill_rect(0,0,2,18,0x1234)==ESP_ERR_TIMEOUT);
    assert(s_sprite_dma_fault && pending && allocations==3 && frees==2);
    unsigned before=transfers;
    assert(zv_board_lcd_fill_rect(0,0,2,18,0x1234)==ESP_ERR_INVALID_STATE);
    assert(transfers==before && allocations==3 && frees==2);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            program, executable = directory / 'fill.c', directory / 'fill'
            program.write_text(harness + function + main)
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(program), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
