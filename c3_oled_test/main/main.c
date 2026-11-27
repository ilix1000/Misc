/* Minimal OLED test for the C3: SSD1306 128x64, SCL=GPIO5, SDA=GPIO6, 0x3C.
 * If this shows a border + "OLED OK" + moving bar, the display driver works. */
#include <string.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"

#define SCL 5
#define SDA 6
#define ADDR 0x3C
#define W 128
#define H 64
#define COL_OFFSET 0   /* 0 = SSD1306, 2 = SH1106 */

static i2c_master_dev_handle_t dev;
static u8g2_t u8;

static void cmds(const uint8_t* c, size_t n){ uint8_t b[40]; b[0]=0; memcpy(b+1,c,n); i2c_master_transmit(dev,b,n+1,100); }

static bool oled_init(void){
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = { .i2c_port=0,.sda_io_num=SDA,.scl_io_num=SCL,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true };
    if(i2c_new_master_bus(&bc,&bus)!=ESP_OK) return false;
    i2c_device_config_t dc = { .dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=ADDR,.scl_speed_hz=400000 };
    if(i2c_master_bus_add_device(bus,&dc,&dev)!=ESP_OK) return false;
    static const uint8_t init[]={0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,0x20,0x02,0xA1,0xC8,0xDA,0x12,0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF};
    cmds(init,sizeof init);
    return true;
}
static uint8_t memcb(u8x8_t* x,uint8_t m,uint8_t a,void* p){ (void)a;(void)p; static const u8x8_display_info_t di={.tile_width=W/8,.tile_height=H/8,.pixel_width=W,.pixel_height=H}; if(m==U8X8_MSG_DISPLAY_SETUP_MEMORY) u8x8_d_helper_display_setup_memory(x,&di); return 1; }
static void flush(void){
    const uint8_t* fb=u8g2_GetBufferPtr(&u8);
    for(int pg=0;pg<H/8;pg++){ uint8_t sp[]={(uint8_t)(0xB0|pg),(uint8_t)(COL_OFFSET&0x0F),(uint8_t)(0x10|(COL_OFFSET>>4))}; cmds(sp,sizeof sp); uint8_t b[1+W]; b[0]=0x40; memcpy(b+1,fb+pg*W,W); i2c_master_transmit(dev,b,sizeof b,100); }
}
void app_main(void){
    bool ok = oled_init();
    uint8_t th; u8g2_SetupDisplay(&u8,memcb,u8x8_cad_empty,u8x8_byte_empty,u8x8_dummy_cb);
    u8g2_SetupBuffer(&u8,u8g2_m_16_8_f(&th),th,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
    u8g2_InitDisplay(&u8); u8g2_SetFontPosTop(&u8);
    int x=0,dir=1;
    while(1){
        u8g2_ClearBuffer(&u8);
        u8g2_DrawFrame(&u8,0,0,W,H);
        u8g2_SetFont(&u8,u8g2_font_7x13B_tr);
        u8g2_DrawStr(&u8,24,6,"OLED OK");
        u8g2_SetFont(&u8,u8g2_font_5x7_tr);
        u8g2_DrawStr(&u8,10,24,ok?"init acked":"init FAIL");
        u8g2_DrawStr(&u8,10,34,"SCL=5 SDA=6 0x3C");
        u8g2_DrawBox(&u8,4+x,46,16,10);
        flush();
        x+=dir*4; if(x>W-24||x<0) dir=-dir;
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
