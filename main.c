// Simple one-button skate/boot dryer using attiny85. Button press wakes mcu and
// enters input mode. During input mode, zero presses = fan off, one press = run
// for short duration, two = med, three = long. After input mode timeout, fan
// runs for specified period then mcu sleeps.
//
// button is simple tacticle with hacky software debounce on pin 2. fan
// controlled by transistor on pin 1. ui led on pin 0.
//
// CPU is prescaled low and timers/counters used for UI/fan timeouts.
//
// datahseet:
// https://ww1.microchip.com/downloads/en/devicedoc/atmel-2586-avr-8-bit-microcontroller-attiny25-attiny45-attiny85_datasheet.pdf

#include <stdbool.h>
#include <stdint.h>

#include <avr/common.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <util/delay.h>

// cpu prescale
#define PRESCALE 128.0
// delay between ui acknowledgement blinks
#define UI_MS 200
// delay between accepting button presses
#define DEBOUNCE_MS 100
// wait for this many iterations of the main loop before sleeping
#define IDLE_ITERATIONS 255

#define LED_PIN 0
#define FAN_PIN 1

// how many timers to run the ui
#define TIMER_UI_COUNT 1
// fan timer parameters
#define RUN_TIMERS_INIT 20
#define RUN_TIMERS_MULTIPLIER 20
#define RUN_LEVELS 3

typedef enum {
  RUN_NO_STATE = 0, // initial, unknown state
  RUN_OFF = 1,      // fan off
  RUN_SHORT = 2,    // fan running (short duration)
  RUN_MED = 3,      // (med duration)
  RUN_LONG = 4,     // (long duration)
} run_state_t;

volatile uint8_t inactive_iterations = 0;
volatile run_state_t current_run_state = RUN_OFF;
volatile run_state_t desired_run_state = RUN_NO_STATE;
volatile uint8_t run_timers = 0; // remaining timer expirations to run fan
volatile int8_t ui_timers = -1;  // remaining timer expriations to run ui
volatile bool sleeping = false;

// state of ui -- idle, accepting input, or timed out
typedef enum {
  UI_OFF,     // idle
  UI_INPUT,   // accepting input
  UI_TIMEOUT, // finished accepting input, need to handle
} ui_state_t;

volatile ui_state_t ui_state = UI_OFF;
volatile uint8_t buffered_clicks = 0; // unhandled button presses

// utility fns
#define MIN_DELAY_MS 5
static inline void delay_ms(double ms) {
  _delay_ms(ms / PRESCALE > MIN_DELAY_MS ? ms / PRESCALE : MIN_DELAY_MS);
}

static inline void fan_off() { PORTB &= ~_BV(FAN_PIN); }

static inline void fan_on() { PORTB |= _BV(FAN_PIN); }

static inline void led_on() { PORTB |= _BV(LED_PIN); }

static inline void led_off() { PORTB &= ~_BV(LED_PIN); }

void blink(int n) {
  for (int i = 0; i < n; i++) {
    led_off();
    delay_ms(UI_MS);
    led_on();
    delay_ms(UI_MS);
    led_off();
  }
}

void panic() {
  for (;;) {
    blink(1);
  }
}

// Interrupt handlers.
// Timer 1 -- triggered by overflow
ISR(TIMER1_OVF_vect) {
  if (run_timers > 0) {
    run_timers--;
    if (run_timers == 0) {
      // timed out, indicate new desired state.
      desired_run_state = RUN_OFF;
    }
  }
}

// Timer 0 -- triggered by overflow
ISR(TIMER0_OVF_vect) {
  if (ui_timers > 0) {
    ui_timers--;
  }
}

// INT0 -- triggered by button press
ISR(INT0_vect) {
  delay_ms(DEBOUNCE_MS); // gross.
  // on wake from sleep, wait a bit.
  if (!sleeping) {
    buffered_clicks += 1;
  }
}

void init(void) {
  run_timers = 0;
  ui_timers = -1;
  buffered_clicks = 0;
  desired_run_state = RUN_NO_STATE;
  current_run_state = RUN_OFF;

  DDRB = 0;                      // in (including INT0/pin 2), except
  DDRB |= _BV(DDB0) | _BV(DDB1); // out for pin 0 and 1

  // change CPU prescale timer to lowest clock speed (8Mhz / 256)
  CLKPR = _BV(CLKPCE);
  CLKPR = _BV(CLKPS3);

  // Set timer 0 to clk/256 -- 2s per overflow
  TCCR0B = _BV(CS02);
  // Timer 0 overflow
  TIMSK = _BV(TOIE0);
  TCNT0 = 0;

  // Timer 1
  // Timings with prescale settings:
  // 16384 - 2m
  // 8192 - 1m
  // 4096 - 30s
  // 2048 - 15s
  // 1024 - 7s
  // 512 - 3s
  // 256 - 1s
  // set timer clear on compare match
  // set prescale to clk/8192
  TCCR1 |= _BV(CS13) | _BV(CS12) | _BV(CS11);
  // set timer to overflow
  TIMSK |= _BV(TOIE1);
  TCNT1 = 0;

  // enable INT0
  GIMSK |= _BV(INT0);

  // enable interrupts
  SREG |= _BV(7);
  sei();
}

void sleep() {
  // set sleep mode to power-down and enable sleep.
  MCUCR = 0; // enable low-level interrupts.
  MCUCR |= _BV(SM1);
  MCUCR |= _BV(SE);

  sleeping = true;
  sleep_mode();

  // on wake, re-enable falling edge interrupts
  MCUCR |= _BV(ISC01);
  sleeping = false;
}

// Called periodically to monitor and actuate UI interactions.
bool ui_handler() {
  uint8_t local_clicks = buffered_clicks;
  bool timeout = ui_timers == 0;
  uint8_t run_level;
  bool active = false;

  switch (ui_state) {
  case UI_OFF:
    // ui is off.
    if (local_clicks > 0) {
      // there were unhandled clicks, enter input state and start input
      // timers.
      ui_state = UI_INPUT;
      led_on(); // turn on led for input state.
      TCNT0 = 0;
      ui_timers = TIMER_UI_COUNT;
      // active now.
      active = true;
    }
    break;
  case UI_INPUT:
    // in input state.
    // nothing to do, yet.
    if (timeout) {
      ui_state = UI_TIMEOUT;
      ui_timers = -1;
      active = true; // speculatively active; we'll see.
    }
    break;
  case UI_TIMEOUT:
    led_off();
    // we timed out. deal with clicks.
    if (local_clicks > 1) {
      // there was one click to enter this state.
      run_level = local_clicks - 1;

      if (run_level >= RUN_LEVELS) {
        // and cap at RUN_LEVELS.
        run_level = RUN_LEVELS;
      }
      if (run_level > 0) {
        // we're entering a run state.
        desired_run_state = RUN_SHORT + (run_level - 1);
      }
      // acknowledge the input.
      blink(run_level);
      // definitely active.
      active = true;
    } else {
      // there were no inputs, turn off.
      desired_run_state = RUN_OFF;
    }

    // we timed out, so the UI state machine is over.
    ui_state = UI_OFF;
    buffered_clicks = 0;
    break;
  }
  return active;
}

// Called periodically to monitor and actuate run state change requests.
bool run_state_handler() {
  if (desired_run_state == RUN_NO_STATE) {
    // no state change request.
    // active if state is not idle or no state.
    return current_run_state > RUN_OFF;
  }

  current_run_state = desired_run_state;
  desired_run_state = RUN_NO_STATE;
  if (current_run_state == RUN_OFF) {
    // reset timers and turn off, if on
    run_timers = 0;
    fan_off();
    return false; // inactive
  }

  // turn on and start countdown
  fan_on();
  TCNT1 = 0;
  run_timers =
      RUN_TIMERS_INIT + RUN_TIMERS_MULTIPLIER * (current_run_state - RUN_SHORT);
  return true; // active
}

void maybe_sleep(bool active) {
  // if we are in an active state, do nothing.
  if (active) {
    // reset inactive iterations.
    inactive_iterations = 0;
    return;
  }

  inactive_iterations++;
  if (inactive_iterations > IDLE_ITERATIONS) {
    sleep();
    inactive_iterations = 0;
  }
}

void loop() {
  bool active = false;
  active |= run_state_handler();
  active |= ui_handler();
  maybe_sleep(active);
}

int main() {
  init();
  blink(5); // hi mom!

  while (1) {
    loop();
  }

  return 0;
}
