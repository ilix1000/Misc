/*
 * emu_gb_impl.h - Game Boy front-end, compiled twice:
 *   emu_gb.c  with Peanut-GB          (original Game Boy, most accurate)
 *   emu_gbc.c with Peanut-GB's Color branch (Game Boy Color games)
 * The including file defines GB_CGB, GB_STATE_MAGIC and GB_RUN_DECL.
 */
typedef struct {
    uint8_t *rom;
    size_t rom_size;
    uint8_t *cart_ram;
    size_t cart_ram_size;
    bool ram_dirty;
    uint16_t *fb;
    uint16_t pal[3][4];    /* [OBJ0, OBJ1, BG][shade], ready for the panel */
    uint16_t pal565[3][4]; /* same, unswapped, for blending */
    int ox, oy, dw, dh;    /* where the 160x144 picture goes on screen */
} gb_priv_t;

extern const uint32_t gb_palettes[GB_PALETTE_COUNT][3][4];

static gb_priv_t s;
static uint8_t s_line_dst[LCD_HEIGHT_GB];

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    return addr < s.rom_size ? s.rom[addr] : 0xFF;
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    return addr < s.cart_ram_size ? s.cart_ram[addr] : 0xFF;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    if (addr < s.cart_ram_size && s.cart_ram[addr] != val) {
        s.cart_ram[addr] = val;
        s.ram_dirty = true;
    }
}

static void gb_err(struct gb_s *gb, const enum gb_error_e e, const uint16_t addr)
{
    /* Bad opcode / bus access: keep running, most games recover. */
}

#define BLEND(a, b) ((uint16_t)((((a) & 0xF7DE) >> 1) + (((b) & 0xF7DE) >> 1)))

static void IRAM_ATTR draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    if (line >= LCD_HEIGHT_GB) return;
    int y = s_line_dst[line];
    if (y == 0xFF) return;
    uint16_t *dst = s.fb + (s.oy + y) * LCD_WIDTH + s.ox;
#if GB_CGB
    if (gb->cgb.cgbMode) {
        /* Color games: pixels index the 64 colours the game set up (32 BG,
         * 32 sprite), stored as 15-bit RGB. Convert those once per line. */
        uint16_t cp[64], cp565[64];
        for (int i = 0; i < 64; i++) {
            uint16_t f = gb->cgb.fixPalette[i];
            uint16_t r = (f >> 10) & 31, g = (f >> 5) & 31, b = f & 31;
            cp565[i] = (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
            cp[i] = SWAP16(cp565[i]);
        }
        if (s.dw == 160) {
            for (int x = 0; x < 160; x++) dst[x] = cp[pixels[x] & 0x3f];
        } else {
            for (int x = 0; x < s.dw; x++) {
                int f = x * 5 / 4;
                uint16_t a = cp565[pixels[f] & 0x3f];
                uint16_t v = (x * 5) % 4 && f < 159 ? BLEND(a, cp565[pixels[f + 1] & 0x3f]) : a;
                dst[x] = SWAP16(v);
            }
        }
        return;
    }
#endif
    if (s.dw == 160) {
        for (int x = 0; x < 160; x++) {
            uint8_t p = pixels[x];
            int sub = (p >> 4) & 3;
            if (sub > 2) sub = 2;
            dst[x] = s.pal[sub][p & 3];
        }
        return;
    }
    /* portrait: 160 -> 128 columns; every 4 output pixels cover 5 input
     * pixels, so blend neighbours instead of dropping a column. */
    uint16_t c[160];
    for (int x = 0; x < 160; x++) {
        uint8_t p = pixels[x];
        int sub = (p >> 4) & 3;
        if (sub > 2) sub = 2;
        c[x] = s.pal565[sub][p & 3];
    }
    for (int x = 0; x < s.dw; x++) {
        int f = x * 5 / 4;
        uint16_t v = (x * 5) % 4 && f < 159 ? BLEND(c[f], c[f + 1]) : c[f];
        dst[x] = SWAP16(v);
    }
}

static void set_layout(void)
{
    /* Landscape: squeeze 144 lines into 128. Portrait: scale to 128 wide and
     * keep the aspect ratio, centred with black bars. */
    s.dw = LCD_WIDTH < 160 ? LCD_WIDTH : 160;
    s.dh = LCD_WIDTH < 160 ? LCD_HEIGHT_GB * s.dw / 160 : LCD_HEIGHT;
    if (s.dh > LCD_HEIGHT) s.dh = LCD_HEIGHT;
    s.ox = (LCD_WIDTH - s.dw) / 2;
    s.oy = (LCD_HEIGHT - s.dh) / 2;
    for (int i = 0; i < LCD_HEIGHT_GB; i++) s_line_dst[i] = 0xFF;
    for (int y = 0; y < s.dh; y++) s_line_dst[y * LCD_HEIGHT_GB / s.dh] = y;
}

static void clear_borders(uint16_t *fb)
{
    if (s.oy > 0) {
        gfx_rect(fb, 0, 0, LCD_WIDTH, s.oy, C_BLACK);
        gfx_rect(fb, 0, s.oy + s.dh, LCD_WIDTH, LCD_HEIGHT - s.oy - s.dh, C_BLACK);
    }
    if (s.ox > 0) {
        gfx_rect(fb, 0, 0, s.ox, LCD_HEIGHT, C_BLACK);
        gfx_rect(fb, s.ox + s.dw, 0, LCD_WIDTH - s.ox - s.dw, LCD_HEIGHT, C_BLACK);
    }
}

static void load_palette(int idx)
{
    if (idx < 0 || idx >= GB_PALETTE_COUNT) idx = 0;
    for (int sub = 0; sub < 3; sub++)
        for (int i = 0; i < 4; i++) {
            uint32_t c = gb_palettes[idx][sub][i];
            s.pal565[sub][i] = RGB565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            s.pal[sub][i] = SWAP16(s.pal565[sub][i]);
        }
}

static void save_sram(const char *rom_path)
{
    if (!s.cart_ram_size || !s.ram_dirty) return;
    char path[300];
    save_path(path, sizeof(path), rom_path, "gb", ".sav");
    if (file_save(path, s.cart_ram, s.cart_ram_size)) s.ram_dirty = false;
}

/* Save states are the whole emulator struct + cart RAM. Function pointers are
 * restored from the live struct so they stay valid across firmware builds. */
static bool state_save(struct gb_s *gb, const char *rom_path)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "gb", ".state");
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t magic = GB_STATE_MAGIC;
    bool ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(gb, sizeof(*gb), 1, f) == 1;
    if (ok && s.cart_ram_size) ok = fwrite(s.cart_ram, s.cart_ram_size, 1, f) == 1;
    fclose(f);
    return ok;
}

static bool state_load(struct gb_s *gb, const char *rom_path)
{
    char path[300];
    save_path(path, sizeof(path), rom_path, "gb", ".state");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct gb_s *tmp = heap_caps_malloc(sizeof(*tmp), MALLOC_CAP_SPIRAM);
    uint32_t magic = 0;
    bool ok = tmp && fread(&magic, 4, 1, f) == 1 && magic == GB_STATE_MAGIC &&
              fread(tmp, sizeof(*tmp), 1, f) == 1;
    if (ok && s.cart_ram_size) ok = fread(s.cart_ram, s.cart_ram_size, 1, f) == 1;
    fclose(f);
    if (ok) {
        tmp->gb_rom_read = gb->gb_rom_read;
        tmp->gb_cart_ram_read = gb->gb_cart_ram_read;
        tmp->gb_cart_ram_write = gb->gb_cart_ram_write;
        tmp->gb_error = gb->gb_error;
        tmp->gb_serial_tx = gb->gb_serial_tx;
        tmp->gb_serial_rx = gb->gb_serial_rx;
        tmp->gb_bootrom_read = gb->gb_bootrom_read;
        tmp->display.lcd_draw_line = gb->display.lcd_draw_line;
        tmp->direct.priv = gb->direct.priv;
        memcpy(gb, tmp, sizeof(*gb));
        s.ram_dirty = true;
    }
    free(tmp);
    return ok;
}

/* Takes ownership of rom. */
GB_RUN_DECL(const char *path, uint8_t *rom, size_t rom_size)
{
    memset(&s, 0, sizeof(s));
    s.rom = rom;
    s.rom_size = rom_size;

    /* fast internal RAM if Wi-Fi keeps enough of it, else PSRAM */
    struct gb_s *gb = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > sizeof(struct gb_s) + 48 * 1024)
        gb = heap_caps_malloc(sizeof(struct gb_s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gb) gb = heap_caps_malloc(sizeof(struct gb_s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gb) { free(s.rom); message_box("Out of memory", NULL, 0); return; }

    enum gb_init_error_e err = gb_init(gb, rom_read, cart_ram_read, cart_ram_write, gb_err, &s);
    if (err != GB_INIT_NO_ERROR) {
        free(gb);
        free(s.rom);
        message_box(err == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Unsupported cartridge" : "Bad ROM checksum",
                    NULL, 0);
        return;
    }

    size_t ram_size = gb_cart_ram_size(rom, rom_size);
    if (ram_size > 0) {
        s.cart_ram = heap_caps_calloc(1, ram_size, MALLOC_CAP_SPIRAM);
        s.cart_ram_size = s.cart_ram ? ram_size : 0;
        char sp[300];
        save_path(sp, sizeof(sp), path, "gb", ".sav");
        if (s.cart_ram) file_read_into(sp, s.cart_ram, s.cart_ram_size);
    }

    gb_init_lcd(gb, draw_line);
    load_palette(g_settings.gb_palette);
    set_layout();

    char title[17];
    gb_get_rom_name(gb, title);

    pacer_t pacer;
    pacer_start(&pacer);
    int64_t sram_t = esp_timer_get_time();
#if GB_CGB
    int64_t rtc_t = sram_t;
#endif
    bool skip = false;

    for (;;) {
        uint32_t keys = input_poll();
        if (bridge_quit_requested()) break;   /* web UI: exit / switch game */
        if (input_pressed() & BTN_MENU) {
            ingame_action_t a = ingame_menu(title[0] ? title : "Game Boy", true);
            if (a == MENU_QUIT) break;
            if (a == MENU_RESET) gb_reset(gb);
            if (a == MENU_SAVE_STATE)
                message_box(state_save(gb, path) ? "State saved" : "Save failed", NULL, 700);
            if (a == MENU_LOAD_STATE)
                message_box(state_load(gb, path) ? "State loaded" : "No saved state", NULL, 700);
            load_palette(g_settings.gb_palette);
            pacer_start(&pacer);
            continue;
        }

        uint8_t pad = 0xFF; /* active low */
        if (keys & BTN_A) pad &= ~JOYPAD_A;
        if (keys & BTN_B) pad &= ~JOYPAD_B;
        if (keys & BTN_START) pad &= ~JOYPAD_START;
        if (keys & BTN_SELECT) pad &= ~JOYPAD_SELECT;
        if (keys & BTN_UP) pad &= ~JOYPAD_UP;
        if (keys & BTN_DOWN) pad &= ~JOYPAD_DOWN;
        if (keys & BTN_LEFT) pad &= ~JOYPAD_LEFT;
        if (keys & BTN_RIGHT) pad &= ~JOYPAD_RIGHT;
        gb->direct.joypad = pad;

        s.fb = display_fb();
        clear_borders(s.fb);
        gb_run_frame(gb);
        if (!skip) {
            if (g_settings.show_fps) draw_fps(s.fb, pacer.fps);
            display_present();
        }

        int64_t now = esp_timer_get_time();
#if GB_CGB
        /* this core needs the cartridge clock (MBC3 RTC) ticked by hand */
        while (now - rtc_t >= 1000000) { gb_tick_rtc(gb); rtc_t += 1000000; }
#endif
        if (s.ram_dirty && now - sram_t >= 30000000) { save_sram(path); sram_t = now; }

        skip = pacer_wait(&pacer, GB_FRAME_US) && !skip; /* never skip 2 in a row */
    }

    save_sram(path);
    display_wait();
    free(s.cart_ram);
    free(gb);
    free(s.rom);
}
