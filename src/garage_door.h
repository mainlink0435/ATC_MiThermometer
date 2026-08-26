#ifndef _GARAGE_DOOR_H_
#define _GARAGE_DOOR_H_

/*
 * Garage door display mode for ATC_MiThermometer (LYWSD03MMC).
 *
 * A host (e.g. Home Assistant via ESPHome BLE client) connects over BLE and
 * writes the command [CMD_ID_GARAGE][state] to the RxTx characteristic
 * (service 0x1F10, char 0x1F1F). The firmware then renders baked-in
 * animations on the LCD for the given state:
 *
 *   state 0   = closed
 *   state 1   = open
 *   state 2   = opening  (transition animation until told otherwise)
 *   state 3   = closing  (transition animation until told otherwise)
 *   state 0xFF = exit garage mode, return to normal temp/hum display
 *
 * Optional 3rd byte: [CMD_ID_GARAGE][state][period] where period is the
 * animation frame period in 0.1s units (0 = default 4 -> 0.4s).
 */

enum {
	GARAGE_STATE_CLOSED = 0,
	GARAGE_STATE_OPEN,
	GARAGE_STATE_OPENING,
	GARAGE_STATE_CLOSING,
	GARAGE_STATE_OFF = 0xFF
} GARAGE_STATES;

#define GARAGE_DEFAULT_PERIOD_X100MS	4	// 0.4s

// Set the garage door state (0..3) and activate the display mode.
// Pass GARAGE_STATE_OFF (0xFF) to deactivate and return to normal display.
void garage_set_state(u8 state);
// Optional: override the animation frame period in 0.1s units (0 = default).
void garage_set_period(u8 period_x100ms);
u8  garage_is_active(void);
u8  garage_get_state(void);

// Advance the animation timer. Call from the main loop when garage mode is active.
void garage_task(u32 now);
// Render the current animation frame into display_buff. Replaces lcd().
void garage_render(void);

#endif // _GARAGE_DOOR_H_
