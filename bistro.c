/*
 * "Bistro" firmware
 * This code runs on a single-channel or dual-channel driver (FET+7135)
 * with an attiny25/45/85 MCU and a capacitor to measure offtime (OTC).
 *
 * Copyright (C) 2015 Selene Scriven
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * ATTINY25/45/85 Diagram
 *           ----
 *         -|1  8|- VCC
 *     OTC -|2  7|- Voltage ADC
 *  Star 3 -|3  6|- PWM (FET, optional)
 *     GND -|4  5|- PWM (1x7135)
 *           ----
 *
 * FUSES
 *      I use these fuse settings on attiny25
 *      Low:  0xd2
 *      High: 0xde
 *      Ext:  0xff
 *
 *      For more details on these settings:
 *      http://www.engbedded.com/cgi-bin/fcx.cgi?P_PREV=ATtiny25&P=ATtiny25&M_LOW_0x3F=0x12&M_HIGH_0x07=0x06&M_HIGH_0x20=0x00&B_SPIEN=P&B_SUT0=P&B_CKSEL3=P&B_CKSEL2=P&B_CKSEL0=P&B_BODLEVEL0=P&V_LOW=E2&V_HIGH=DE&V_EXTENDED=FF
 *
 * STARS
 *      Star 3 = unused
 *
 * CALIBRATION
 *
 *   To find out what values to use, flash the driver with battcheck.hex
 *   and hook the light up to each voltage you need a value for.  This is
 *   much more reliable than attempting to calculate the values from a
 *   theoretical formula.
 *
 *   Same for off-time capacitor values.  Measure, don't guess.
 */

#include "driver_config_bistro.h"

/*
 * =========================================================================
 */

// Ignore a spurious warning, we did the cast on purpose
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

#include <avr/pgmspace.h>
//#include <avr/io.h>
//#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
//#include <avr/power.h>
#include <string.h>

#define OWN_DELAY           // Don't use stock delay functions.
#define USE_DELAY_MS        // Also use _delay_ms()
#define USE_DELAY_S         // Also use _delay_s()
#include "tk-delay.h"

#include "tk-voltage.h"

#ifdef RANDOM_STROBE
#include "tk-random.h"
#endif

/*
 * global variables
 */

// Config option variables
#define USE_FIRSTBOOT
#ifdef USE_FIRSTBOOT
#define FIRSTBOOT 0b01010101
uint8_t g_u8firstboot = FIRSTBOOT;  // detect initial boot or factory reset
#endif
uint8_t g_u8modegroup = 5;     // which mode group (set above in #defines)
uint8_t enable_moon = 1;   // Should we add moon to the set of g_u8modes?
uint8_t reverse_modes = 0; // flip the mode order?
uint8_t g_u8memory = 0;        // mode g_u8memory, or not (set via soldered star)
#ifdef OFFTIM3
uint8_t g_u8offtim3 = 1;       // enable medium-press?
#endif
#ifdef TEMPERATURE_MON
uint8_t g_u8maxtemp = 79;      // temperature step-down threshold
#endif
uint8_t muggle_mode = 0;   // simple mode designed for muggles
// Other state variables
uint8_t g_u8mode_override = 0; // do we need to enter a special mode?
uint8_t g_u8mode_idx = 0;      // current or last-used mode number
uint8_t g_u8eepos = 0;
// counter for entering config mode
// (needs to be remembered while off, but only for up to half a second)
uint8_t g_u8fast_presses __attribute__ ((section (".noinit")));

// total length of current mode group's array
#ifdef OFFTIM3
uint8_t g_u8mode_cnt;
#endif
// number of regular non-hidden g_u8modes in current mode group
uint8_t g_u8solid_modes;
// number of hidden g_u8modes in the current mode group
// (hardcoded because both groups have the same hidden g_u8modes)
//uint8_t hidden_modes = NUM_HIDDEN;  // this is never used


PROGMEM const uint8_t hiddenmodes[] = { HIDDENMODES };
// default values calculated by group_calc.py
// Each group must be 8 values long, but can be cut short with a zero.
#define NUM_MODEGROUPS 9  // don't count muggle mode
PROGMEM const uint8_t modegroups[] = {
    64,  0,  0,  0,  0,  0,  0,  0,
    11, 64,  0,  0,  0,  0,  0,  0,
    11, 35, 64,  0,  0,  0,  0,  0,
    11, 26, 46, 64,  0,  0,  0,  0,
    11, 23, 36, 50, 64,  0,  0,  0,
    11, 20, 31, 41, 53, 64,  0,  0,
    29, 64,POLICE_STROBE,0,0,0,0,0,  // 7: special group A
    BIKING_STROBE,BATTCHECK,11,29,64,0,0,0,  // 8: special group B
    9, 18, 29, 46, 64,  0,  0,  0,  // 9: special group C
    11, 29, 50,  0,                  // muggle mode, exception to "must be 8 bytes long"
};
//uint8_t g_u8modes[] = { 1,2,3,4,5,6,7,8,9, HIDDENMODES };  // make sure this is long enough...
uint8_t g_u8modes[9 + sizeof(hiddenmodes)];  // make sure this is long enough...

// Modes (gets set when the light starts up based on saved config values)
PROGMEM const uint8_t ramp_7135[] = { RAMP_7135 };
PROGMEM const uint8_t ramp_FET[]  = { RAMP_FET };

void save_mode() {  // save the current mode index (with wear leveling)
    uint8_t oldpos=g_u8eepos;

    g_u8eepos = (g_u8eepos+1) & ((EEPSIZE/2)-1);  // wear leveling, use next cell

    eeprom_write_byte((uint8_t *)(g_u8eepos), g_u8mode_idx);  // save current state
    eeprom_write_byte((uint8_t *)(oldpos), 0xff);     // erase old state
}

#define OPT_firstboot (EEPSIZE-1)
#define OPT_modegroup (EEPSIZE-2)
#define OPT_memory (EEPSIZE-3)
#define OPT_offtim3 (EEPSIZE-4)
#define OPT_maxtemp (EEPSIZE-5)
#define OPT_mode_override (EEPSIZE-6)
#define OPT_moon (EEPSIZE-7)
#define OPT_revmodes (EEPSIZE-8)
#define OPT_muggle (EEPSIZE-9)
void save_state() {  // central method for writing complete state
    save_mode();
#ifdef USE_FIRSTBOOT
    eeprom_write_byte((uint8_t *)OPT_firstboot, g_u8firstboot);
#endif
    eeprom_write_byte((uint8_t *)OPT_modegroup, g_u8modegroup);
    eeprom_write_byte((uint8_t *)OPT_memory, g_u8memory);
#ifdef OFFTIM3
    eeprom_write_byte((uint8_t *)OPT_offtim3, g_u8offtim3);
#endif
#ifdef TEMPERATURE_MON
    eeprom_write_byte((uint8_t *)OPT_maxtemp, g_u8maxtemp);
#endif
    eeprom_write_byte((uint8_t *)OPT_mode_override, g_u8mode_override);
    eeprom_write_byte((uint8_t *)OPT_moon, enable_moon);
    eeprom_write_byte((uint8_t *)OPT_revmodes, reverse_modes);
    eeprom_write_byte((uint8_t *)OPT_muggle, muggle_mode);
}

#ifndef USE_FIRSTBOOT
static inline void reset_state() {
    g_u8mode_idx = 0;
    g_u8modegroup = 5;
    save_state();
}
#endif

void restore_state() {
    uint8_t eep;

#ifdef USE_FIRSTBOOT
    // check if this is the first time we have powered on
    eep = eeprom_read_byte((uint8_t *)OPT_firstboot);
    if (eep != FIRSTBOOT) {
        // not much to do; the defaults should already be set
        // while defining the variables above
        save_state();
        return;
    }
#else
    uint8_t first = 1;
#endif

    // find the mode index data
    for(g_u8eepos=0; g_u8eepos<(EEPSIZE/2); g_u8eepos++) {
        eep = eeprom_read_byte((const uint8_t *)g_u8eepos);
        if (eep != 0xff) {
            g_u8mode_idx = eep;
#ifndef USE_FIRSTBOOT
            first = 0;
#endif
            break;
        }
    }
#ifndef USE_FIRSTBOOT
    // if no g_u8mode_idx was found, assume this is the first boot
    if (first) {
        reset_state();
        return;
    }
#endif

    // load other config values
    g_u8modegroup = eeprom_read_byte((uint8_t *)OPT_modegroup);
    g_u8memory    = eeprom_read_byte((uint8_t *)OPT_memory);
#ifdef OFFTIM3
    g_u8offtim3   = eeprom_read_byte((uint8_t *)OPT_offtim3);
#endif
#ifdef TEMPERATURE_MON
    g_u8maxtemp   = eeprom_read_byte((uint8_t *)OPT_maxtemp);
#endif
    g_u8mode_override = eeprom_read_byte((uint8_t *)OPT_mode_override);
    enable_moon   = eeprom_read_byte((uint8_t *)OPT_moon);
    reverse_modes = eeprom_read_byte((uint8_t *)OPT_revmodes);
    muggle_mode   = eeprom_read_byte((uint8_t *)OPT_muggle);

    // unnecessary, save_state handles wrap-around
    // (and we don't really care about it skipping cell 0 once in a while)
    //else g_u8eepos=0;

#ifndef USE_FIRSTBOOT
    if (g_u8modegroup >= NUM_MODEGROUPS) reset_state();
#endif
}

static inline void next_mode() {
    g_u8mode_idx += 1;
    if (g_u8mode_idx >= g_u8solid_modes) {
        // Wrap around, skipping the hidden g_u8modes
        // (note: this also applies when going "forward" from any hidden mode)
        // FIXME? Allow this to cycle through hidden g_u8modes?
        g_u8mode_idx = 0;
    }
}

#ifdef OFFTIM3
static inline void prev_mode() {
    // simple mode has no reverse
    if (muggle_mode) {
        return next_mode();
    }

    if (g_u8mode_idx == g_u8solid_modes) {
        // If we hit the end of the hidden g_u8modes, go back to moon
        g_u8mode_idx = 0;
    } else if (g_u8mode_idx > 0) {
        // Regular mode: is between 1 and TOTAL_MODES
        g_u8mode_idx -= 1;
    } else {
        // Otherwise, wrap around (this allows entering hidden g_u8modes)
        g_u8mode_idx = g_u8mode_cnt - 1;
    }
}
#endif

void count_modes() {
    /*
     * Determine how many solid and hidden g_u8modes we have.
     *
     * (this matters because we have more than one set of g_u8modes to choose
     *  from, so we need to count at runtime)
     */
    // copy config to local vars to avoid accidentally overwriting them in muggle mode
    // (also, it seems to reduce overall program size)
    uint8_t my_modegroup = g_u8modegroup;
    uint8_t my_enable_moon = enable_moon;
    uint8_t my_reverse_modes = reverse_modes;

    // override config if we're in simple mode
    if (muggle_mode) {
        my_modegroup = NUM_MODEGROUPS;
        my_enable_moon = 0;
        my_reverse_modes = 0;
    }

    uint8_t *dest;
    const uint8_t *src = modegroups + (my_modegroup<<3);
    dest = g_u8modes;

    // add moon mode (or not) if config says to add it
    if (my_enable_moon) {
        g_u8modes[0] = 1;
        dest ++;
    }

    // Figure out how many g_u8modes are in this group
    //g_u8solid_modes = g_u8modegroup + 1;  // Assume group N has N g_u8modes
    // No, how about actually counting the g_u8modes instead?
    // (in case anyone changes the mode groups above so they don't form a triangle)
    for(g_u8solid_modes=0;
            (g_u8solid_modes<8) && pgm_read_byte(src);
            g_u8solid_modes++, src++ )
    {
        *dest++ = pgm_read_byte(src);
    }

    // add regular g_u8modes
    //memcpy_P(dest, src, g_u8solid_modes);  // was already copied above
    // add hidden g_u8modes
    //memcpy_P(dest + g_u8solid_modes, hiddenmodes, sizeof(hiddenmodes));
    // smaller than memcpy_p()
    for( src=hiddenmodes; src<hiddenmodes+sizeof(hiddenmodes); src++ )
    {
        *dest++ = pgm_read_byte(src);
    }
    // final count
#ifdef OFFTIM3
    g_u8mode_cnt = g_u8solid_modes + sizeof(hiddenmodes);
#endif
    if (my_reverse_modes) {
        // TODO: yuck, isn't there a better way to do this?
        int8_t i;
        src += g_u8solid_modes;
        dest = g_u8modes;
        for(i=0; i<g_u8solid_modes; i++) {
            src --;
            *dest = pgm_read_byte(src);
            dest ++;
        }
        if (my_enable_moon) {
            *dest = 1;
        }
        g_u8mode_cnt --;  // get rid of last hidden mode, since it's a duplicate turbo
    }
    if (my_enable_moon) {
        g_u8mode_cnt ++;
        g_u8solid_modes ++;
    }
}

#ifdef ALT_PWM_LVL
static inline void set_output(uint8_t pwm1, uint8_t pwm2) {
#else
static inline void set_output(uint8_t pwm1) {
#endif
    /* This is no longer needed since we always use PHASE mode.
    // Need PHASE to properly turn off the light
    if ((pwm1==0) && (pwm2==0)) {
        TCCR0A = PHASE;
    }
    */
    PWM_LVL = pwm1;
#ifdef ALT_PWM_LVL
    ALT_PWM_LVL = pwm2;
#endif
}

void set_level(uint8_t level) {
    if (level == 0) {
        set_output(0,0);
    } else {
        level -= 1;
        set_output(pgm_read_byte(ramp_FET  + level),
                   pgm_read_byte(ramp_7135 + level));
    }
}

void set_mode(uint8_t mode) {
#ifdef SOFT_START
    static uint8_t actual_level = 0u;
    uint8_t target_level = mode;
    int8_t shift_amount;
    int8_t diff;
    do {
        diff = target_level - actual_level;
        shift_amount = (diff >> 2) | (diff!=0);
        actual_level += shift_amount;
        set_level(actual_level);
        //_delay_ms(RAMP_SIZE/20);  // slow ramp
        _delay_ms(RAMP_SIZE/4);  // fast ramp
    } while (target_level != actual_level);
#else
#define set_mode set_level
    //set_level(mode);
#endif  // SOFT_START
}

void blink(uint8_t val, uint16_t speed)
{
    for (; val>0; val--)
    {
        set_level(BLINK_BRIGHTNESS);
        _delay_ms(speed);
        set_level(0);
        _delay_ms(speed);
        _delay_ms(speed);
    }
}

static inline void strobe(uint8_t ontime, uint8_t offtime) {
    uint8_t i;
    for(i=0; i<8; i++) {
        set_level(RAMP_SIZE);
        _delay_ms(ontime);
        set_level(0);
        _delay_ms(offtime);
    }
}

#ifdef SOS
static inline void SOS_mode() {
#define SOS_SPEED 200
    blink(3, SOS_SPEED);
    _delay_ms(SOS_SPEED*5);
    blink(3, SOS_SPEED*5/2);
    //_delay_ms(SOS_SPEED);
    blink(3, SOS_SPEED);
    _delay_s();
    _delay_s();
}
#endif

void toggle(uint8_t *var, uint8_t num) {
    // Used for config mode
    // Changes the value of a config option, waits for the user to "save"
    // by turning the light off, then changes the value back in case they
    // didn't save.  Can be used repeatedly on different options, allowing
    // the user to change and save only one at a time.
    blink(num, BLINK_SPEED/8);  // indicate which option number this is
    *var ^= 1;
    save_state();
    // "buzz" for a while to indicate the active toggle window
    blink(32, 500/32);
    /*
    for(uint8_t i=0; i<32; i++) {
        set_level(BLINK_BRIGHTNESS * 3 / 4);
        _delay_ms(20);
        set_level(0);
        _delay_ms(20);
    }
    */
    // if the user didn't click, reset the value and return
    *var ^= 1;
    save_state();
    _delay_s();
}

#ifdef TEMPERATURE_MON
uint8_t get_temp() {
    ADC_on_temperature();
    // average a few values; temperature is noisy
    uint16_t temp = 0;
    uint8_t i;
    get_temperature();
    for(i=0; i<16; i++) {
        temp += get_voltage();
        _delay_ms(5);
    }
    temp >>= 4;
    return temp;
}
#endif  // TEMPERATURE_MON

#ifdef OFFTIM3
static inline uint8_t read_otc() {
    // Read and return the off-time cap value
    // Start up ADC for capacitor pin
    // disable digital input on ADC pin to reduce power consumption
    DIDR0 |= (1 << CAP_DIDR);
    // 1.1v reference, left-adjust, ADC3/PB3
    ADMUX  = (1 << V_REF) | (1 << ADLAR) | CAP_CHANNEL;
    // enable, start, prescale
    ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;

    // Wait for completion
    while (ADCSRA & (1 << ADSC));
    // Start again as datasheet says first result is unreliable
    ADCSRA |= (1 << ADSC);
    // Wait for completion
    while (ADCSRA & (1 << ADSC));

    // ADCH should have the value we wanted
    return ADCH;
}
#endif

int main(void)
{
    // check the OTC immediately before it has a chance to charge or discharge
#ifdef OFFTIM3
    uint8_t cap_val = read_otc();  // save it for later
#endif

    // Set PWM pin to output
    DDRB |= (1 << PWM_PIN);     // enable main channel
#ifdef ALT_PWM_PIN
    DDRB |= (1 << ALT_PWM_PIN); // enable second channel
#endif

    // Set timer to do PWM for correct output pin and set prescaler timing
    //TCCR0A = 0x23; // phase corrected PWM is 0x21 for PB1, fast-PWM is 0x23
    //TCCR0B = 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)
    TCCR0A = PHASE;
    // Set timer to do PWM for correct output pin and set prescaler timing
    TCCR0B = 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)

    // Read config values and saved state
    restore_state();

    // Enable the current mode group
    count_modes();


    // TODO: Enable this?  (might prevent some corner cases, but requires extra room)
    // g_u8memory decayed, reset it
    // (should happen on med/long press instead
    //  because mem decay is *much* slower when the OTC is charged
    //  so let's not wait until it decays to reset it)
    //if (g_u8fast_presses > 0x20) { g_u8fast_presses = 0; }

    // check button press time, unless the mode is overridden
    if (! g_u8mode_override) {
#ifdef OFFTIM3
        if (cap_val > CAP_SHORT) {
#else
        if (g_u8fast_presses < 0x20) {
#endif
            // Indicates they did a short press, go to the next mode
            // We don't care what the g_u8fast_presses value is as long as it's over 15
            g_u8fast_presses = (g_u8fast_presses+1) & 0x1f;
            next_mode(); // Will handle wrap arounds
#ifdef OFFTIM3
        } else if (cap_val > CAP_MED) {
            // User did a medium press, go back one mode
            g_u8fast_presses = 0;
            if (g_u8offtim3) {
                prev_mode();  // Will handle "negative" g_u8modes and wrap-arounds
            } else {
                next_mode();  // disabled-med-press acts like short-press
                // (except that g_u8fast_presses isn't reliable then)
            }
#endif
        } else {
            // Long press, keep the same mode
            // ... or reset to the first mode
            g_u8fast_presses = 0;
            if (muggle_mode  || (! g_u8memory)) {
                // Reset to the first mode
                g_u8mode_idx = 0;
            }
        }
    }
    save_mode();

#ifdef CAP_PIN
    // Charge up the capacitor by setting CAP_PIN to output
    DDRB  |= (1 << CAP_PIN);    // Output
    PORTB |= (1 << CAP_PIN);    // High
#endif

    // Turn features on or off as needed
#ifdef VOLTAGE_MON
    ADC_on();
#else
    ADC_off();
#endif

    uint8_t output;
    uint8_t actual_level;
#ifdef TEMPERATURE_MON
    uint8_t overheat_count = 0;
#endif
#ifdef VOLTAGE_MON
    uint8_t lowbatt_cnt = 0;
    uint8_t i = 0;
    uint8_t voltage;
    // Make sure voltage reading is running for later
    ADCSRA |= (1 << ADSC);
#endif
    //output = pgm_read_byte(g_u8modes + g_u8mode_idx);
    output = g_u8modes[g_u8mode_idx];
    actual_level = output;
    // handle mode overrides, like mode group selection and temperature calibration
    if (g_u8mode_override) {
        // do nothing; mode is already set
        //g_u8mode_idx = g_u8mode_override;
        g_u8fast_presses = 0;
        output = g_u8mode_idx;
    }
    while(1) {
        if (g_u8fast_presses > 0x0f) {  // Config mode
            _delay_s();       // wait for user to stop fast-pressing button
            g_u8fast_presses = 0; // exit this mode after one use
            g_u8mode_idx = 0;

            // Enter or leave "muggle mode"?
            toggle(&muggle_mode, 1);
            if (muggle_mode) {
                continue;
            };  // don't offer other options in muggle mode

            toggle(&g_u8memory, 2);

            toggle(&enable_moon, 3);

            toggle(&reverse_modes, 4);

            // Enter the mode group selection mode?
            g_u8mode_idx = GROUP_SELECT_MODE;
            toggle(&g_u8mode_override, 5);
            g_u8mode_idx = 0;

#ifdef OFFTIM3
            toggle(&g_u8offtim3, 6);
#endif

#ifdef TEMPERATURE_MON
            // Enter temperature calibration mode?
            g_u8mode_idx = TEMP_CAL_MODE;
            toggle(&g_u8mode_override, 7);
            g_u8mode_idx = 0;
#endif

#ifdef USE_FIRSTBOOT
            toggle(&g_u8firstboot, 8);
#endif

            //output = pgm_read_byte(g_u8modes + g_u8mode_idx);
            output = g_u8modes[g_u8mode_idx];
            actual_level = output;
        }
#ifdef STROBE
        else if (output == STROBE) {
            // 10Hz tactical strobe
            strobe(33,67);
        }
#endif // ifdef STROBE
#ifdef POLICE_STROBE
        else if (output == POLICE_STROBE) {
            // police-like strobe
            //for(i=0;i<8;i++) {
            strobe(20,40);
            //}
            //for(i=0;i<8;i++) {
            strobe(40,80);
            //}
        }
#endif // ifdef POLICE_STROBE
#ifdef RANDOM_STROBE
        else if (output == RANDOM_STROBE) {
            // pseudo-random strobe
            uint8_t ms = 34 + (pgm_rand() & 0x3f);
            strobe(ms, ms);
            //strobe(ms, ms);
        }
#endif // ifdef RANDOM_STROBE
#ifdef BIKING_STROBE
        else if (output == BIKING_STROBE) {
            // 2-level stutter beacon for biking and such
#ifdef FULL_BIKING_STROBE
            // normal version
            for(i=0; i<4; i++) {
                set_output(255,0);
                _delay_ms(5);
                set_output(0,255);
                _delay_ms(65);
            }
            _delay_ms(720);
#else
            // small/minimal version
            set_output(255,0);
            _delay_ms(10);
            set_output(0,255);
            _delay_s();
#endif
        }
#endif  // ifdef BIKING_STROBE
#ifdef SOS
        else if (output == SOS) {
            SOS_mode();
        }
#endif // ifdef SOS
#ifdef RAMP
        else if (output == RAMP) {
            int8_t r;
            // simple ramping test
            for(r=1; r<=RAMP_SIZE; r++) {
                set_level(r);
                _delay_ms(25);
            }
            for(r=RAMP_SIZE; r>0; r--) {
                set_level(r);
                _delay_ms(25);
            }
        }
#endif  // ifdef RAMP
#ifdef BATTCHECK
        else if (output == BATTCHECK) {
#ifdef BATTCHECK_VpT
            // blink out volts and tenths
            _delay_ms(100);
            uint8_t result = battcheck();
            blink(result >> 5, BLINK_SPEED/8);
            _delay_ms(BLINK_SPEED);
            blink(1,5);
            _delay_ms(BLINK_SPEED*3/2);
            blink(result & 0b00011111, BLINK_SPEED/8);
#else  // ifdef BATTCHECK_VpT
            // blink zero to five times to show voltage
            // (~0%, ~25%, ~50%, ~75%, ~100%, >100%)
            blink(battcheck(), BLINK_SPEED/8);
#endif  // ifdef BATTCHECK_VpT
            // wait between readouts
            _delay_s();
            _delay_s();
        }
#endif // ifdef BATTCHECK
        else if (output == GROUP_SELECT_MODE) {
            // exit this mode after one use
            g_u8mode_idx = 0;
            g_u8mode_override = 0;

            for(i=0; i<NUM_MODEGROUPS; i++) {
                g_u8modegroup = i;
                save_state();

                blink(1, BLINK_SPEED/3);
            }
            _delay_s();
            _delay_s();
        }
#ifdef TEMP_CAL_MODE
        else if (output == TEMP_CAL_MODE) {
            // make sure we don't stay in this mode after button press
            g_u8mode_idx = 0;
            g_u8mode_override = 0;

            // Allow the user to turn off thermal regulation if they want
            g_u8maxtemp = 255;
            save_state();
            set_mode(RAMP_SIZE/4);  // start somewhat dim during turn-off-regulation mode
            _delay_s();
            _delay_s();

            // run at highest output level, to generate heat
            set_mode(RAMP_SIZE);

            // measure, save, wait...  repeat
            while(1) {
                g_u8maxtemp = get_temp();
                save_state();
                _delay_s();
                _delay_s();
            }
        }
#endif  // TEMP_CAL_MODE
        else {  // Regular non-hidden solid mode
            set_mode(actual_level);
#ifdef TEMPERATURE_MON
            uint8_t temp = get_temp();

            // step down? (or step back up?)
            if (temp >= g_u8maxtemp) {
                overheat_count ++;
                // reduce noise, and limit the lowest step-down level
                if ((overheat_count > 15) && (actual_level > (RAMP_SIZE/8))) {
                    actual_level --;
                    //_delay_ms(5000);  // don't ramp down too fast
                    overheat_count = 0;  // don't ramp down too fast
                }
            } else {
                // if we're not overheated, ramp up to the user-requested level
                overheat_count = 0;
                if ((temp < g_u8maxtemp - 2) && (actual_level < output)) {
                    actual_level ++;
                }
            }
            set_mode(actual_level);

            ADC_on();  // return to voltage mode
#endif
            // Otherwise, just sleep.
            _delay_ms(500);

            // If we got this far, the user has stopped fast-pressing.
            // So, don't enter config mode.
            //g_u8fast_presses = 0;
        }
        g_u8fast_presses = 0;
#ifdef VOLTAGE_MON
        if (ADCSRA & (1 << ADIF)) {  // if a voltage reading is ready
            voltage = ADCH;  // get the waiting value
            // See if voltage is lower than what we were looking for
            if (voltage < ADC_LOW) {
                lowbatt_cnt ++;
            } else {
                lowbatt_cnt = 0;
            }
            // See if it's been low for a while, and maybe step down
            if (lowbatt_cnt >= 8) {
                // DEBUG: blink on step-down:
                //set_level(0);  _delay_ms(100);

                if (actual_level > RAMP_SIZE) {  // hidden / blinky g_u8modes
                    // step down from blinky g_u8modes to medium
                    actual_level = RAMP_SIZE / 2;
                } else if (actual_level > 1) {  // regular solid mode
                    // step down from solid g_u8modes somewhat gradually
                    // drop by 25% each time
                    actual_level = (actual_level >> 2) + (actual_level >> 1);
                    // drop by 50% each time
                    //actual_level = (actual_level >> 1);
                } else { // Already at the lowest mode
                    //g_u8mode_idx = 0;  // unnecessary; we never leave this clause
                    //actual_level = 0;  // unnecessary; we never leave this clause
                    // Turn off the light
                    set_level(0);
                    // Power down as many components as possible
                    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
                    sleep_mode();
                }
                set_mode(actual_level);
                output = actual_level;
                //save_mode();  // we didn't actually change the mode
                lowbatt_cnt = 0;
                // Wait before lowering the level again
                //_delay_ms(250);
                _delay_s();
            }

            // Make sure conversion is running for next time through
            ADCSRA |= (1 << ADSC);
        }
#endif  // ifdef VOLTAGE_MON
    }

    //return 0; // Standard Return Code
}
