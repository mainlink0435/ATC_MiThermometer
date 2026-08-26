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
// "RAM" = retention section: survives deep sleep. Without it these would be
// reset to zero on every wake, losing garage mode and the animation frame.
RAM static volatile u8  g_state = GARAGE_STATE_CLOSED;
RAM static volatile u8  g_active = 0;
RAM static volatile u8  g_period_x100ms = GARAGE_DEFAULT_PERIOD_X100MS;
RAM static u32 g_frame = 0;
RAM static u32 g_last_tick = 0;

// Arrow glyphs for the bottom (small) digits.
#define GLYPH_UP_ARROW   0x33  // "▲" (a,f,b,g)
#define GLYPH_DOWN_ARROW 0xC6  // "▼" (d,e,c,g)

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
// Write the 3 top (big) digit slots and the 2 bottom (small) digit slots.
static void set_top(u8 g5, u8 g4, u8 g3) {
	display_buff[5] = g5;
	display_buff[4] = g4;
	display_buff[3] = g3;
}
static void set_bottom(u8 g1, u8 g0) {
	display_buff[1] = g1;
	display_buff[0] = g0;
}

// Scroll a 3-char window of `tape` (length `len`) through the top row,
// starting at `start`. Out-of-range positions are shown blank.
static void scroll_top(const char *tape, int len, int start) {
	u8 w[3];
	for(int i = 0; i < 3; i++) {
		int idx = start + i;
		w[i] = (idx >= 0 && idx < len) ? glyph(tape[idx]) : 0x00;
	}
	set_top(w[0], w[1], w[2]);
}

// Set smiley + BLE symbol bits in byte 2 (temp symbol left off).
static void set_symbols(u8 smiley, bool ble) {
	display_buff[2] = (smiley & 0x07) | (ble ? 0x10 : 0x00);
}

// ---- renderers ----
// Idle state: the word scrolls leftwards through the top (big) row so it
// stays on one line and reads clearly. The bottom (small) row shows a fixed
// 2-char label (e.g. "UP" / "dn").
static void render_idle(const char *word, int n, u8 smiley, bool ble, u8 bg1, u8 bg0) {
	char tape[16];
	tape[0] = ' ';
	for(int i = 0; i < n; i++) tape[1 + i] = word[i];
	tape[1 + n] = ' ';
	int len = n + 2;
	int start = (int)(g_frame % (u32)(n + 1));
	scroll_top(tape, len, start);
	set_bottom(bg1, bg0);
	set_symbols(smiley, ble);
}

// Transition animation (opening/closing), loops until a new command.
// Phase A: countdown 9..1 on the top row + blinking direction arrows.
// Phase B: segment sweep across the top row (rising / descending).
// Phase C: scrolling word marquee through the top row.
static void render_transition(bool opening, bool ble) {
	const char *word = opening ? "OPEN" : "CLOSED";
	int wlen = opening ? 4 : 6;
	u8 arr = opening ? GLYPH_UP_ARROW : GLYPH_DOWN_ARROW;
	u32 f = g_frame & 0xff;

	if (f < 9) { // countdown 9..1
		u8 n = (u8)(9 - f); // 9..1
		set_top(0x00, 0x00, glyph('0' + n));
		u8 on = (f & 1) ? arr : 0x00; // blink arrows
		set_bottom(on, on);
		set_symbols(0, ble);
		return;
	}
	f -= 9;
	if (f < 4) { // sweep on the top row
		u8 seg;
		if(opening)
			seg = (f == 0) ? 0x80 : (f == 1) ? 0x02 : 0x10; // d, g, a (rising)
		else
			seg = (f == 0) ? 0x10 : (f == 1) ? 0x02 : 0x80; // a, g, d (descending)
		set_top(seg, seg, seg);
		set_bottom(arr, arr);
		set_symbols(0, ble);
		return;
	}
	f -= 4;
	{ // marquee "OPEN"/"CLOSED" through the top row
		char tape[14];
		tape[0] = ' ';
		for(int i = 0; i < wlen; i++) tape[1 + i] = word[i];
		tape[1 + wlen] = ' ';
		int len = wlen + 2;
		int start = (int)(f % (u32)(wlen + 1));
		scroll_top(tape, len, start);
		set_bottom(arr, arr);
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
	case GARAGE_STATE_OPEN:     render_idle("OPEN",   4, 5 /*(^-^)*/, ble, glyph('U'), glyph('P')); break;
	case GARAGE_STATE_CLOSED:   render_idle("CLOSED", 6, 6 /*(-^-)*/, ble, glyph('d'), glyph('n')); break;
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
