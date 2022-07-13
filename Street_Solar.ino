////////////////////////////////////////////////////////////////////////
// Street Solar Charger 
// by: Aistheta Gleason
// Charges a 12V lead-acid battery using a 100W solar panel (23V)
// Requirements: Arduino UNO or Arduino Pro Mini, Buck Converter Circuit
// 12 Automotive Battery, 100W (23Voc) Solar Panel
////////////////////////////////////////////////////////////////////////
// Safety Note: Read the README!!! Keep Battery in well ventilated area
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
// License
////////////////////////////////////////////////////////////////////////
// You have an unlimited license to learn electronics and have fun with 
// this project - just be safe, don't burn your house or other peoples' 
// houses down. Feel free to copy, modify, do whatever you want with the 
// code, I'm not responsible for what you do with it.
////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////
// Includes
////////////////////////////////////////////////////////////////////////
// Timer 1 Library
#include <TimerOne.h>

////////////////////////////////////////////////////////////////////////
// Defines
////////////////////////////////////////////////////////////////////////
// Charge voltage (target)
#define VCHARGE 14.0
// SW1 PWM Pin
#define SW1_PWM 6
// Battery Voltage Measure ADC Pin
#define VBAT_ADC A0
// Inductor Voltage Measure ADC Pin
#define VL_ADC A1
// Solar Voltage Measure ADC Pin
#define VSOL_ADC A2
// Number of Integrals to Average Before MPPT
#define NUM_INT 10
// PWM Frequency (30Hz)
#define PWM_FREQ 30
// Base Timer Frequency (100xPWM Frequency)
#define BASE_PER (1/(100*PWM_FREQ))
// Minimum Duty Cycle
#define D_MIN 5
// Maximum Duty Cycle
#define D_MAX 98
// Sleep Time (5m)
#define SLEEP_TIME (5*60)
// ADC COEF (V/code)
#define ADC_COEF (3.3/(2^(11)-1))
// Battery Voltage ADC Gain Coef
#define VBAT_COEF ADC_COEF/(0.15625)
// Inductor Voltage ADC Gain Coef
#define VL_COEF 1/(0.0990991)
// Inductor Voltage ADC Offset
#define VL_OFF -2.5
// Solar Voltage ADC Gain Coef
#define VSOL_COEF ADC_COEF/(0.091639)
// Battery Voltage ADC Macro
#define VBAT_MEAS (analogRead(VBAT_ADC)*VBAT_COEF)
// Inductor Voltage ADC Macro
#define VL_MEAS ((analogRead(VL_ADC)*ADC_COEF + VL_OFF)*VL_COEF)
// Solar Voltage ADC Macro
#define VSOL_MEAS (analogRead(VSOL_ADC)*VSOL_COEF)

////////////////////////////////////////////////////////////////////////
// Global Variables
////////////////////////////////////////////////////////////////////////
// State Variable Type Definition
typedef enum _states {INIT, INTEGRATE, MPPT, DONE} STATES;
// State Variable Definition
volatile STATES cur_state;
// Duty Cycle Variable
volatile unsigned char duty_cycle;
// Solar and Battery Voltage Variables
volatile double v_solar, v_battery; 
// Inductor Current and Previous Voltage Variables
volatile double vl_cur, vl_prev;
// MPPT Power Tracking Variables (for slopes)
volatile double p_cur, p_prev;
// Integral Variable
volatile long int integral;
// Time Tracking Variables (for dt integration)
volatile unsigned long int t_cur, t_prev;
// Number of Integrations
volatile unsigned char num_integrals;
// Average Integral Value (over NUM_INT integrals)
volatile long int integral_avg;
// PWM Count Variable
volatile unsigned char pwm_count;
// New Integral Flag, Timer On Flag, and Duty Cycle Increase Flag
volatile bool new_integral, timer_on, duty_inc;

////////////////////////////////////////////////////////////////////////
// setup() function
// Set's up GPIO, timer, and initial state
////////////////////////////////////////////////////////////////////////
void setup() {
    // Setup SW1 PWM as Output
    pinMode(SW1_PWM, OUTPUT);
    // Turn SW1 Off
    digitalWrite(SW1_PWM, LOW);
    // Setup ADCs as Inputs
    pinMode(VBAT_ADC, INPUT);
    pinMode(VL_ADC, INPUT);
    pinMode(VSOL_ADC, INPUT);
    // Turn off Timer
    timer_on = 0;
    // Set Current State to INIT (timer will change appropriately)
    cur_state = INIT;
    // Initialize Timer to BASE_PER (us)
    Timer1.initialize(BASE_PER*1e6);
    // Attach Timer Intterupt Handler
    Timer1.attachInterrupt(pwm_handler);
}

////////////////////////////////////////////////////////////////////////
// check_battery() function
// Checks the current battery level
// Set's state to done if battery level reaches or excedes charge level
////////////////////////////////////////////////////////////////////////
void check_battery(){
    // Measure Battery Voltage
    v_battery = VBAT_MEAS;
    // If Battery Charged
    if (v_battery >= VCHARGE) {
      timer_on = 0;
      cur_state = DONE;
    }
}

////////////////////////////////////////////////////////////////////////
// check_solar() function
// Reads the solar panel voltage
////////////////////////////////////////////////////////////////////////
void check_solar(){
    // Measure Solar Voltage
    v_solar = VSOL_MEAS;
}

////////////////////////////////////////////////////////////////////////
// loop() function
// Main loop for the program
////////////////////////////////////////////////////////////////////////
void loop() {
    // State Machine
    switch(cur_state){
        ////////////////////////////////////////////////////////////////////////
        // INIT State
        // Reset variables, initialize charger
        ////////////////////////////////////////////////////////////////////////
        case INIT:
            // Reset PWM count
            pwm_count = 0;
            // Reset Number of Integrals
            num_integrals = 0;
            // Reset Integral
            integral = 0;
            // Set new_integral to 0 (Algorithm starts in INTEGRATE after forced init and timer handler called)
            new_integral = 0;
            // Reset Integral Average
            integral_avg = 0;
            // Set Duty Cycle Increase Flag
            // Init to 1, because p_prev = 0 initially, it powers up (increases from 0) so assume increase at first
            duty_inc = 1;
            // Zero Out Power Tracking Variables (will read new cur on first and assume positive)
            p_prev = p_cur = 0;
            // Check Battery Level (Battery Only, Charger Not Running Yet)
            check_battery();
            // Check Solar Level
            check_solar();
            // Set VL prev to start integral
            vl_prev = VL_MEAS;
            // Set Initial Duty Cycle (Vsol*D = Vbat => D = Vbat/Vsol)
            // Will target current battery level then MPPT will nagivate around that
            duty_cycle = (char) (100 * ((double) v_battery / (double) v_solar));
            // Read Current Time, Set Both Previous and Current
            t_prev = t_cur = micros();
            // Set First State to INTEGRATE
            cur_state = INTEGRATE;
            // Turn on Timer
            timer_on = 1;
            break;
        ////////////////////////////////////////////////////////////////////////
        // INTEGRATE State
        // Integrates Inductor voltage to (VL = L*dIL/dt -> IL = int(VL*dt)) to Get Measure of Current (L Fixed)
        // Ideally if you know the value of the inductor, you can more accurately estimate current
        // You can also take a current reading and calibrate (create coef) if you print out the integral values
        ////////////////////////////////////////////////////////////////////////
        case INTEGRATE:
            // Set SW1_PWM High (turn on SW1) if new_integral (just transitioned) then turn SW1 ON
            if(!new_integral) digitalWrite(SW1_PWM, HIGH);
            // Set new_integral flag to 1 (so MPPT can add when transitioned)
            new_integral = 1;
            // Read current time in ticks microseconds
            t_cur = micros();
            // Take VL Current VL Reading
            vl_cur = VL_MEAS;
            // Compute Integral sum(VL*dt)
            if (vl_cur >= vl_prev){
                integral += (vl_prev + (vl_cur - vl_prev)/2.0)*(t_cur - t_prev);
            } else {
                integral += (vl_cur + (vl_prev - vl_cur)/2.0)*(t_cur - t_prev);
            }
            // Set Previous Time to Current
            t_prev = t_cur;
            // Set Previous VL to Current
            vl_prev = vl_cur;
            break;
        ////////////////////////////////////////////////////////////////////////
        // MPPT State
        // Checks Where at on Power Curve and Adjusts Duty Cycle
        // Stears towards duty cycle that achieves maximum power
        ////////////////////////////////////////////////////////////////////////
        case MPPT:
            // Check Battery Level
            check_battery();
            // Check Solar Level
            check_solar();
            // If don't have enough solar to charge battery
            if (v_solar*D_MAX/100.0 < v_battery) {
                timer_on = 0;
                cur_state = DONE;
            }
            // If just transitioned to MPPT
            if (new_integral) {
                // Set SW1_PWM Low (turn SW Off)
                digitalWrite(SW1_PWM, LOW);
                // Add Integral to New Average
                integral_avg += integral;
                // Divide by 2
                integral_avg >>= 1;
                // Increment Integral Count
                num_integrals++;
                // Set New Integral to 0 (prevent re-entry)
                new_integral = 0;
                // Reset Integral Variable for next integration period
                integral = 0;
            }
            // If Have All Integrals (NUM_INT)
            if (num_integrals == NUM_INT) {
                // Check the Battery
                check_battery();
                // Compute Power 
                p_cur = v_battery*integral_avg;
                // If Power Slope Positive (left of peak)
                // Did the Power Increase?
                if (p_cur - p_prev > 0){
                    // Did you Increase the Voltage (duty_cycle)?
                    // Yes then increase again (max power seeking)
                    if(duty_inc){
                        if (++duty_cycle >= D_MAX) duty_cycle = D_MAX;
                        duty_inc = 1;
                    } else {
                        // Else Decrease
                        if (--duty_cycle <= D_MIN) duty_cycle = D_MIN;
                        duty_inc = 0;
                    }
                // If Power Slope Negative (right of peak)
                } else if (p_cur - p_prev < 0){
                    // Did you Increase the Voltage (duty_cycle)?
                    if(duty_inc){
                        // Yes, Then Decrease 
                        if (--duty_cycle <= D_MIN) duty_cycle = D_MIN;
                        duty_inc = 0;
                    } else {
                        // Else increase again
                        if (++duty_cycle >= D_MAX) duty_cycle = D_MAX;
                        duty_inc = 1;
                    }
                }
                // Set Previous Power to Current
                p_prev = p_cur;
                // Reset Number of Integrals
                num_integrals = 0;
            }
            break;
        ////////////////////////////////////////////////////////////////////////
        // DONE State
        // Battery Charged or Solar Dropped Out (not enough sun)
        ////////////////////////////////////////////////////////////////////////
        case DONE:
            // Turn Off Timer
            timer_on = 0;
            // Turn Off SW1 (disconnect solar)
            digitalWrite(SW1_PWM, LOW);
            // Check Battery and Solar
            check_battery();
            check_solar();
            // If Battery Not Charged Anymore and Solar Voltage Good, then start charging
            if ((v_battery < VCHARGE)&&(v_solar*D_MAX/100.0 >= v_battery)) cur_state = INIT;
            // Sleep for SLEEP_TIME
            // Ideally you would put the device to sleep and have some sort of RTC wake up 
            // the system or better yet use an analog comparator that checks the battery 
            // voltage and when it drops below charge level then wake up the system. 
            delay(SLEEP_TIME*1000);
        break;
    }
}

////////////////////////////////////////////////////////////////////////
// PWM Handler Function
// Runs off the base timer frequency, which is 100 times faster
// PWM increments in by +- 1%
// Set's state to INTEGRATE when PWM signal is high
// Set's state to MPPT when PWM signal is low
////////////////////////////////////////////////////////////////////////
void pwm_handler(){
    // If Timer is ON
    if (timer_on) {
        // If PWM Counter less than Duty Cycle (%)
        if(++pwm_count <= duty_cycle){
            // Set State to INTEGRATE
            cur_state = INTEGRATE;
        // If PWM Counter greater than Duty Cycle and less than 100
        } else if ((pwm_count > duty_cycle)&&(pwm_count < 100)) {
            // Set State MPPT
            cur_state = MPPT;
        // If PWM Counter Overflows, reset to 0
        } else if (pwm_count >= 100) pwm_count = 0;
    }
}
