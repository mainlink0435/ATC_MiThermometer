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
RAM static u8  g_settled = 0;   // 1 = idle state settled to static text
RAM static u8  g_saved_adv = 0; // advertising interval saved when garage mode started
RAM static u8  g_adv_mode = 0;  // 0 = saved (normal), 1 = fast (animating), 2 = settled (1s)

// Advertising intervals (in 0.625 ms units) used while garage mode is active:
//  - fast: during animation, so the device wakes ~2x/sec for smooth motion
//  - settled: after the animation settles, so the ESP32 finds it faster on the
//    next command (small battery cost while garage mode is on, ~9 months).
// The saved interval is restored on exit (0xFF) or reboot.
#define GARAGE_ADV_FAST     8   // 8 * 62.5 = 0.5 s
#define GARAGE_ADV_SETTLED 16   // 16 * 62.5 = 1.0 s
// Idle-state marquee word lengths (frames until settling to static text).
#define GARAGE_CLOSED_WLEN   6
#define GARAGE_OPEN_WLEN     4

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
	case 'n': return 0x46; // lowercase n, no top bar
	case 'i': return 0x02;
	case 'P': case 'p': return 0x73;
	case 'r': return 0x42;
	case 't': return 0xe2;
	case 'U': case 'u': return 0xe5; // proper U (b,c,d,e,f)
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
// Entry-from-right marquee: the word scrolls leftwards through the top row
// once (start 0..n+1), then the caller settles to static text.
static void marquee_top(const char *word, int n, u32 frame) {
	char tape[16];
	tape[0] = tape[1] = ' ';
	for(int i = 0; i < n; i++) tape[2 + i] = word[i];
	tape[2 + n] = ' ';
	tape[2 + n + 1] = ' ';
	int len = n + 4;
	int start = (int)(frame % (u32)(n + 2));
	scroll_top(tape, len, start);
}

// Continuous rising/descending bar sweep on the top row.
static void sweep_top(bool opening, u32 frame) {
	u8 seg;
	switch(frame % 3) {
	case 0: seg = opening ? 0x80 : 0x10; break; // bottom / top
	case 1: seg = 0x02; break;                   // middle
	default: seg = opening ? 0x10 : 0x80; break; // top / bottom
	}
	set_top(seg, seg, seg);
}

// ---- advertising helpers ----
// While garage mode is active we use a faster advertising interval so the
// ESP32 discovers the thermometer quickly: FAST (0.5s) during animation and
// SETTLED (1s) once the display settles. The saved interval is restored on
// exit (0xFF) or reboot.
static void adv_set(u8 interval) {
	if (cfg.advertising_interval == interval) return;
	cfg.advertising_interval = interval;
	test_config();
	ev_adv_timeout(0, 0, 0);
}
static void adv_fast_on(void) { // start of an animation
	if (g_adv_mode == 1) return;
	if (g_adv_mode == 0) g_saved_adv = cfg.advertising_interval;
	adv_set(GARAGE_ADV_FAST);
	g_adv_mode = 1;
}
static void adv_settled(void) { // after the animation settles
	if (g_adv_mode != 1) return;
	adv_set(GARAGE_ADV_SETTLED);
	g_adv_mode = 2;
}
static void adv_restore(void) { // exit garage mode
	if (g_adv_mode == 0) return;
	adv_set(g_saved_adv);
	g_adv_mode = 0;
}

// ---- public API ----
void garage_set_state(u8 state) {
	if (state == GARAGE_STATE_OFF) {
		adv_restore();
		g_active = 0;
		g_settled = 0;
		g_frame = 0;
		lcd_flg.update = 1;
	} else if (state <= GARAGE_STATE_ERROR) {
		g_state = state;
		g_active = 1;
		g_settled = 0;
		g_frame = 0;
		g_last_tick = clock_time();
		adv_fast_on();
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
	// Idle states settle to static text after one marquee pass.
	if (!g_settled && (g_state == GARAGE_STATE_CLOSED || g_state == GARAGE_STATE_OPEN)) {
		u32 wlen = (g_state == GARAGE_STATE_CLOSED) ? GARAGE_CLOSED_WLEN : GARAGE_OPEN_WLEN;
		if (g_frame >= wlen + 2) {
			g_settled = 1;
			adv_settled();
			lcd_flg.update = 1;
		}
	}
}

void garage_render(void) {
	if (!g_active) return;
	bool ble = wrk.ble_connected != 0;
	switch(g_state) {
	case GARAGE_STATE_CLOSED:
		if (g_settled) {
			set_top(glyph('C'), glyph('L'), glyph('S'));
			set_bottom(glyph('d'), glyph('n'));
		} else {
			marquee_top("CLOSED", GARAGE_CLOSED_WLEN, g_frame);
			set_bottom(glyph('d'), glyph('n'));
		}
		break;
	case GARAGE_STATE_OPEN:
		if (g_settled) {
			set_top(glyph('O'), glyph('P'), glyph('n'));
			set_bottom(glyph('U'), glyph('P'));
		} else {
			marquee_top("OPEn", GARAGE_OPEN_WLEN, g_frame);
			set_bottom(glyph('U'), glyph('P'));
		}
		break;
	case GARAGE_STATE_OPENING:
		sweep_top(1, g_frame);
		set_bottom(glyph('U'), glyph('P'));
		break;
	case GARAGE_STATE_CLOSING:
		sweep_top(0, g_frame);
		set_bottom(glyph('d'), glyph('n'));
		break;
	case GARAGE_STATE_ERROR: // "Err" blinking forever
		if (g_frame & 1)
			set_top(glyph('E'), glyph('r'), glyph('r'));
		else
			set_top(0, 0, 0);
		set_bottom(0, 0);
		break;
	default: break;
	}
	set_symbols(0, ble); // smiley off, keep BLE symbol
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
