#include "tl_common.h"
#include "app_config.h"
#include "app.h"
#include "lcd.h"
#include "ble.h"
#include "drivers.h"
#include "garage_door.h"

#if (DEV_SERVICES & SERVICE_SCREEN) && (DEVICE_TYPE == DEVICE_LYWSD03MMC)

/*
 * LYWSD03MMC display_buff layout (see lcd_lywsd03mmc.c):
 *   bytes 5,4,3 = big-number digits (left..right)
 *   byte  2     = smiley(bits0-2) | BLE(bit4) | temp symbol(bits5-7)
 *   bytes 1,0   = small-number digits (left..right)
 *
 * 7-seg segment bits (same encoding as display_numbers[]):
 *   bit0 = b (top-right)   bit1 = g (middle)     bit2 = c (bottom-right)
 *   bit3 = dp              bit4 = a (top)        bit5 = f (top-left)
 *   bit6 = e (bottom-left) bit7 = d (bottom)
 */

// ---- state ----
static volatile u8  g_state = GARAGE_STATE_CLOSED;
static volatile u8  g_active = 0;
static volatile u8  g_period_x100ms = GARAGE_DEFAULT_PERIOD_X100MS;
static u32 g_frame = 0;
static u32 g_last_tick = 0;

// ---- glyph lookup (7-seg) ----
static u8 glyph(char c) {
	switch(c) {
	case ' ': return 0x00;
	case '0': case 'O': case 'o': return 0xf5;
	case '1': case 'I': return 0x05;
	case '2': return 0xd3;
	case '3': return 0x97;
	case '4': return 0x27;
	case '5': case 'S': case 's': return 0xb6;
	case '6': case 'G': case 'g': return 0xf6;
	case '7': return 0x15;
	case '8': return 0xf7;
	case '9': return 0xb7;
	case 'A': case 'a': case 'H': case 'h': return 0x77; // "A"
	case 'b': return 0xe6;
	case 'C': case 'c': return 0xf0;
	case 'd': return 0xc7;
	case 'E': case 'e': return 0xf2;
	case 'F': case 'f': return 0x72;
	case 'L': case 'l': return 0xe0;
	case 'n': return 0x56;
	case 'i': return 0x02;
	case 'P': case 'p': return 0x73;
	case 'r': return 0x42;
	case 't': return 0xe2;
	case 'U': case 'u': return 0xe1;
	case 'V': case 'v': case 'y': return 0xa7; // "y"/down-arrow
	case '^': return 0x63; // up chevron
	default: return 0x00;
	}
}

// ---- helpers ----
// Write the 5 digit slots with the segment byte for each of 5 chars.
static void set_text(u8 g0, u8 g1, u8 g2, u8 g3, u8 g4) {
	display_buff[5] = g0;
	display_buff[4] = g1;
	display_buff[3] = g2;
	display_buff[1] = g3;
	display_buff[0] = g4;
}

// Draw a 5-char window of `tape` (length `len`) starting at `start`.
// Out-of-range positions are shown blank.
static void draw_tape(const char *tape, int len, int start) {
	u8 w[5];
	for(int i = 0; i < 5; i++) {
		int idx = start + i;
		w[i] = (idx >= 0 && idx < len) ? glyph(tape[idx]) : 0x00;
	}
	set_text(w[0], w[1], w[2], w[3], w[4]);
}

// Set smiley + BLE symbol bits in byte 2 (temp symbol left off).
static void set_symbols(u8 smiley, bool ble) {
	display_buff[2] = (smiley & 0x07) | (ble ? 0x10 : 0x00);
}

// ---- renderers ----
// Idle marquee: the word scrolls leftwards through the 5 slots, looped.
static void render_idle(const char *word, int n, u8 smiley, bool ble) {
	char tape[16];
	tape[0] = tape[1] = ' ';
	for(int i = 0; i < n; i++) tape[2 + i] = word[i];
	tape[2 + n] = ' ';
	tape[2 + n + 1] = ' ';
	int len = n + 4;
	int start = (int)(g_frame % (u32)(n + 1));
	draw_tape(tape, len, start);
	set_symbols(smiley, ble);
}

// Transition animation (opening/closing), loops until a new command.
// Phase A: countdown 9..1 on a big digit with direction arrows.
// Phase B: segment sweep across all digits (rising / descending).
// Phase C: scrolling word marquee.
static void render_transition(bool opening, bool ble) {
	const char *word = opening ? "OPENING" : "CLOSING";
	int wlen = 7;
	u8 arr = opening ? 0x77 /*A*/ : 0xa7 /*y*/; // direction glyph
	u32 f = g_frame & 0xff;

	if (f < 9) { // countdown
		u8 n = (u8)(9 - f); // 9..1
		display_buff[5] = 0x00;
		display_buff[4] = 0x00;
		display_buff[3] = glyph('0' + n);
		display_buff[1] = arr;
		display_buff[0] = arr;
		set_symbols(0, ble);
		return;
	}
	f -= 9;
	if (f < 4) { // sweep: bar across all digits, bottom->top (open) / top->bottom (close)
		u8 seg;
		if(opening)
			seg = (f == 0) ? 0x80 : (f == 1) ? 0x02 : 0x10; // d, g, a
		else
			seg = (f == 0) ? 0x10 : (f == 1) ? 0x02 : 0x80; // a, g, d
		display_buff[5] = display_buff[4] = display_buff[3] = seg;
		display_buff[1] = display_buff[0] = arr;
		set_symbols(0, ble);
		return;
	}
	f -= 4;
	{ // marquee
		char tape[14];
		tape[0] = tape[1] = ' ';
		for(int i = 0; i < wlen; i++) tape[2 + i] = word[i];
		tape[2 + wlen] = ' ';
		tape[2 + wlen + 1] = ' ';
		int len = wlen + 4;
		int start = (int)(f % (u32)(wlen + 1));
		draw_tape(tape, len, start);
		set_symbols(0, ble);
	}
}

// ---- public API ----
void garage_set_state(u8 state) {
	if (state == GARAGE_STATE_OFF) {
		g_active = 0;
		g_frame = 0;
		lcd_flg.update = 1;
	} else {
		g_state = state & 0x03;
		g_active = 1;
		g_frame = 0;
		g_last_tick = clock_time();
		lcd_flg.update = 1;
	}
}

void garage_set_period(u8 period_x100ms) {
	g_period_x100ms = period_x100ms ? period_x100ms : GARAGE_DEFAULT_PERIOD_X100MS;
}

u8 garage_is_active(void) { return g_active; }
u8 garage_get_state(void) { return g_active ? g_state : GARAGE_STATE_OFF; }

void garage_task(u32 now) {
	if (!g_active) return;
	u32 period = (u32)g_period_x100ms * (100 * CLOCK_16M_SYS_TIMER_CLK_1MS);
	if (now - g_last_tick >= period) {
		g_last_tick = now;
		g_frame++;
		lcd_flg.update = 1;
	}
}

void garage_render(void) {
	if (!g_active) return;
	bool ble = wrk.ble_connected != 0;
	switch(g_state) {
	case GARAGE_STATE_OPEN:     render_idle("OPEN",   4, 5 /*(^-^)*/, ble); break;
	case GARAGE_STATE_CLOSED:   render_idle("CLOSED", 6, 6 /*(-^-)*/, ble); break;
	case GARAGE_STATE_OPENING:  render_transition(1, ble); break;
	case GARAGE_STATE_CLOSING:  render_transition(0, ble); break;
	default: break;
	}
}

#else // (DEV_SERVICES & SERVICE_SCREEN) && (DEVICE_TYPE == DEVICE_LYWSD03MMC)

// No-op stubs for other device builds so symbols always exist.
void garage_set_state(u8 state) { (void)state; }
void garage_set_period(u8 period_x100ms) { (void)period_x100ms; }
u8 garage_is_active(void) { return 0; }
u8 garage_get_state(void) { return GARAGE_STATE_OFF; }
void garage_task(u32 now) { (void)now; }
void garage_render(void) { }

#endif // (DEV_SERVICES & SERVICE_SCREEN) && (DEVICE_TYPE == DEVICE_LYWSD03MMC)
