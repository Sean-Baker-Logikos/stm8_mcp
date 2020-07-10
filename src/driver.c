/**
  ******************************************************************************
  * @file driver.c
  * @brief support functions for the BLDC motor control
  * @author Neidermeier
  * @version
  * @date March-2020
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "stm8s.h"
#include "parameter.h" // app defines


/* Private defines -----------------------------------------------------------*/

//#define PWM_IS_MANUAL
//#define LOWER_ARM_CHOPPING

#define PWM_100PCNT    TIM2_PWM_PD
#define PWM_50PCNT     ( PWM_100PCNT / 2 )
#define PWM_25PCNT     ( PWM_100PCNT / 4 )
#define PWM_0PCNT      0
#define PWM_DC_RAMPUP  PWM_50PCNT // 52 // exp. determined


#ifndef PWM_IS_MANUAL
#define PWM_NOT_MANUAL_DEF  PWM_25PCNT //30 //0x20 // experimentally determined value (using manual adjustment)
#endif



/*
 * These constants are the number of timer counts (TIM3) to achieve a given
 *  commutation step period.
 * See TIM3 setup - base period is 0.000008 Seconds (8 uSecs)
 * For the theoretical 15000 RPM motor:
 *   15000 / 60 = 250 rps
 *   cycles per sec = 250 * 6 = 1500
 *   1 cycle = 1/1500 = .000667 S
 *
 * RPS = (1 / cycle_time * 6)
 */

// the OL comm time is shortened by 1 rammp-unit (e.g. 2 counts @ 0.000008S per count where 8uS is the TIM3 bit-time)
// the ramp shortens the OL comm-time by 1 RU each ramp-step with each ramp step is the TIM1 period of ~1mS
#define BLDC_ONE_RAMP_UNIT          1

// 1 cycle = 6 * 8uS * 512 = 0.024576 S
#define BLDC_OL_TM_LO_SPD         512  // start of ramp

// 1 cycle = 6 * 8uS * 80 = 0.00384 S
#define BLDC_OL_TM_HI_SPD          80  // end of ramp

// 1 cycle = 6 * 8uS * 50 = 0.0024 S
#define BLDC_OL_TM_MANUAL_HI_LIM   64 // 58   // stalls at 56 ... stop at 64? ( 0x40? ) (I want to push the button and see the data at this speed w/o actually chaniging the CT)

// any "speed" setting higher than HI_LIM would be by closed-loop control of
// commutation period (manual speed-control input by adjusting PWM  duty-cycle)
// The control loop will only have precision of 0.000008 S adjustment though (externally triggered comparator would be ideal )

// 1 cycle = 6 * 8uS * 13 = 0.000624 S
// 1 cycle = 6 * 8uS * 14 = 0.000672 S
#define LUDICROUS_SPEED (13) // 15kRPM would be ~13.8 counts


#define THREE_PHASES 3

/* Private types -----------------------------------------------------------*/

// enumerates the PWM state of each channel
typedef enum DC_PWM_STATE
{
    DC_OUTP_OFF,
    DC_PWM_PLUS,
    DC_PWM_MINUS, // complimented i.e. (100% - DC)
    DC_OUTP_HI,
    DC_OUTP_LO,
    DC_OUTP_FLOAT
} DC_PWM_STATE_t;

// enumerate 3 phases
typedef enum THREE_PHASE_CHANNELS
{
    PHASE_A,
    PHASE_B,
    PHASE_C
} THREE_PHASE_CHANNELS_t;

//enumerate available PWM drive modes
typedef enum PWM_MODE
{
    UPPER_ARM,
    LOWER_ARM
    // SYMETRICAL ... upper and lower arms driven (complementary) .. maybe no use for it
} PWM_MODE_t;

//enumerate commutation "sectors" (steps)
typedef enum COMMUTATION_SECTOR
{
    SECTOR_1,
    SECTOR_2,
    SECTOR_3,
    SECTOR_4,
    SECTOR_5,
    SECTOR_6
} COMMUTATION_SECTOR_t;

/* Public variables  ---------------------------------------------------------*/
uint16_t BLDC_OL_comm_tm;   // could be private

uint16_t Manual_uDC;

BLDC_STATE_T BLDC_State;


/* Private variables ---------------------------------------------------------*/

static uint16_t Ramp_Step_Tm; // reduced x2 each time but can't start any slower
/* static */ uint16_t global_uDC;

/* Private function prototypes -----------------------------------------------*/
void bldc_move( COMMUTATION_SECTOR_t );

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  .
  * @par Parameters:
  * None
  * @retval void None
  */
void PWM_Set_DC(uint16_t pwm_dc)
{
    global_uDC = pwm_dc;

    if ( BLDC_OFF == BLDC_State )
    {
//        global_uDC = 0;
    }
}

/*
 * intermediate function for setting PWM with positive or negative polarity
 * Provides an "inverted" (complimentary) duty-cycle if [state0 < 0]
 */
uint16_t _set_output(uint8_t chan, DC_PWM_STATE_t state0)
{
    uint16_t pulse = PWM_0PCNT;

    switch(state0)
    {
    default:
    case DC_OUTP_FLOAT:
    case DC_OUTP_LO:
        pulse = PWM_0PCNT;
        break;
    case DC_OUTP_HI:
        pulse = PWM_100PCNT;
        break;
    case DC_PWM_PLUS:
        pulse = global_uDC;
        break;
    case DC_PWM_MINUS: // complimented i.e. (100% - DC)
        pulse = TIM2_PWM_PD - global_uDC; // inverse pulse
        break;
    }

    return pulse;
}



/* 
 * see Issue #6
 * At the end of driven sectors (2 x 60 degree = 120 degrees driving duration),
 * the PWM would be in an indeterminate state (hi or lo) depending on TIM1 duty-cycle
 * and how the comm. switch timing (TIM3) happens to line up with it (if comm. switch 
 * happends during on or off segment).. 
 * Only by calling TIM1_DeInit() has it been possible to 
 * assert the state of the PWM signal on the sector that is being transitioned ->FLOAT .
 * But this has been a problem in that, the 2 consecutive driving sectors should not
 * really be reconfigured unless there is a way to do it w/o causing a disruption during 
 * the 120 driving semgnet which appears as voltage noise spike on the motor phase output
 * If only I could  get the STM8 TIM setup right).
 * Would need to only assert the phase being transitioned -> FLOAT. 
 */

/**
  * @brief  .
  * @par Parameters:
  * None
  * @retval void None
  *   reference:
  *    http://embedded-lab.com/blog/starting-stm8-microcontrollers/21/
  *
  * - pulse width modulation frequency determined by the value of the TIM1_ARR register 
  * - duty cycle determined by the value of the TIM1_CCRi register
  */
void PWM_set_outputs(DC_PWM_STATE_t state0, DC_PWM_STATE_t state1, DC_PWM_STATE_t state2)
{
/* todo: look into this?:
"For correct operation, preload registers must be enabled when the timer is in PWM mode. This
is not mandatory in one-pulse mode (OPM bit set in TIM1_CR1 register)."
*/
/*
 *  assert some config bits in the Timer1 peripheral (see "TIM1_Deinit()" ) before setting
 * up the individual channels. This is fussy, it can mess up the back-EMF part of the phase voltage!
 */
    TIM1_Cmd(DISABLE); // maybe? (TIM1->CR1)  ........ YES
    TIM1_SetCounter(0); // maybe? YES
    TIM1_CtrlPWMOutputs(DISABLE); // maybe?  (TIM1->BKR) ...... //          definately likes this!!!!!!!!!!!!!!


    if (DC_PWM_PLUS == state0 /* MINUS? */)
    {
        TIM1_CCxCmd(TIM1_CHANNEL_2, ENABLE);
        TIM1_SetCompare2(_set_output(0, state0));
    }
    else
    {
        TIM1_CCxCmd(TIM1_CHANNEL_2, DISABLE);

        if (DC_OUTP_HI == state0)
        {
            GPIOC->ODR |=  (1<<2);  // PC2 set HI
            GPIOC->DDR |=  (1<<2);
            GPIOC->CR1 |=  (1<<2);
        }
        else // LO
        {
            GPIOC->ODR &=  ~(1<<2);  // PC2 set LO
            GPIOC->DDR |=  (1<<2);
            GPIOC->CR1 |=  (1<<2);
        }
    }

    if (DC_PWM_PLUS == state1 /* MINUS? */)
    {
        TIM1_CCxCmd(TIM1_CHANNEL_3, ENABLE);
        TIM1_SetCompare3(_set_output(1, state1));
    }
    else
    {
        TIM1_CCxCmd(TIM1_CHANNEL_3, DISABLE);

        if (DC_OUTP_HI == state1)
        {
            GPIOC->ODR |=  (1<<3);  // PC3 set HI
            GPIOC->DDR |=  (1<<3);
            GPIOC->CR1 |=  (1<<3);
        }
        else
        {
            GPIOC->ODR &=  ~(1<<3);  // PC3 set LO
            GPIOC->DDR |=  (1<<3);
            GPIOC->CR1 |=  (1<<3);
        }
    }

    if (DC_PWM_PLUS == state2 /* MINUS? */)
    {
        TIM1_CCxCmd(TIM1_CHANNEL_4, ENABLE);
        TIM1_SetCompare4(_set_output(2, state2));
    }
    else
    {
        TIM1_CCxCmd(TIM1_CHANNEL_4, DISABLE);

        if (DC_OUTP_HI == state2)
        {
            GPIOC->ODR |=  (1<<4);  // PC4 set HI
            GPIOC->DDR |=  (1<<4);
            GPIOC->CR1 |=  (1<<4);
        }
        else
        {
            GPIOC->ODR &=  ~(1<<4);  // PC4 set LO
            GPIOC->DDR |=  (1<<4);
            GPIOC->CR1 |=  (1<<4);
        }
    }

// counterparts to Disable commands above
    TIM1_Cmd(ENABLE);
    TIM1_CtrlPWMOutputs(ENABLE);
}



/*
 *
 */
void BLDC_Stop()
{
    BLDC_State = BLDC_OFF;
    PWM_Set_DC( 0 );
}

/*
 *
 */
void BLDC_Spd_dec()
{
    if (BLDC_OFF == BLDC_State)
    {
        BLDC_State = BLDC_RAMPUP;
        // BLDC_OL_comm_tm ... init in OFF state to _OL_TM_LO_SPD, don't touch!
    }

    if (BLDC_ON == BLDC_State  && BLDC_OL_comm_tm < 0xFFFF)
    {
        BLDC_OL_comm_tm += 1; // slower
    }
}

/*
 *
 */
void BLDC_Spd_inc()
{
    if (BLDC_OFF == BLDC_State)
    {
        BLDC_State = BLDC_RAMPUP;
        // BLDC_OL_comm_tm ... init in OFF state to _OL_TM_LO_SPD, don't touch!
    }

    if (BLDC_ON == BLDC_State  && BLDC_OL_comm_tm > BLDC_OL_TM_MANUAL_HI_LIM )
    {
        BLDC_OL_comm_tm -= 1; // faster
    }
}


void TIM3_setup(uint16_t u16period); // tmp

/*
 * BLDC Update: handle the BLDC state
 *      Off: nothing
 *      Rampup: get BLDC up to sync speed to est. comm. sync.
 *              Once the HI OL speed (frequency) is reached, then the idle speed
 *              must be established, i.e. controlling PWM DC to ? to achieve 2500RPM
 *              To do this closed loop, will need to internally time between the
 *              A/D or comparator input interrupts and adjust DC using e.g. Proportional
 *              control. When idle speed is reached, can transition to user control i.e. ON State
 *      On:  definition of ON state - user control (button inputs) has been enabled
 *              1) ideally, does nothing - BLDC_Step triggered by A/D comparator event
 *              2) less ideal, has to check A/D or comp. result and do the comm.
 *                 step ... but the resolution will be these discrete steps
 *                 (of TIM1 reference)
 */
void BLDC_Update(void)
{
    switch (BLDC_State)
    {
    default:
    case BLDC_OFF:
        // reset commutation timer and ramp-up counters ready for ramp-up
        BLDC_OL_comm_tm = BLDC_OL_TM_LO_SPD;
        break;

    case BLDC_ON:
        // do ON stuff
#ifdef PWM_IS_MANUAL
// doesn't need to set global uDC every time as it would be set once in the FSM
// transition ramp->on ... but it doesn't hurt to assert it
        PWM_Set_DC( Manual_uDC ) ;  // #ifdef SYMETRIC_PWM ...  (PWM_50PCNT +  Manual_uDC / 2)
#else
//         PWM_Set_DC( PWM_NOT_MANUAL_DEF ) ;
#endif
        break;
    case BLDC_RAMPUP:

        PWM_Set_DC( PWM_DC_RAMPUP ) ;

        if (BLDC_OL_comm_tm > BLDC_OL_TM_HI_SPD) // state-transition trigger?
        {
            BLDC_OL_comm_tm -= BLDC_ONE_RAMP_UNIT;
        }
        else
        {
            // TODO: the actual transition to ON state would be seeing the ramp-to speed
// achieved in closed-loop operation
            BLDC_State = BLDC_ON;
            PWM_Set_DC( PWM_NOT_MANUAL_DEF );
        }
        break;
    }

#if 1 //    ! MANUAL TEST
//  update the timer for the OL commutation switch time
    TIM3_setup(BLDC_OL_comm_tm);
#endif
}

/*
 * TODO: schedule at 30degree intervals? (see TIM3)	????
 Start a short timer on which ISR will then  trigger the A/D with proper timing  .... at 1/4 of the comm. cycle ?
 So TIM3 would not stepp 6 times but 6x4 times? (4 times precision?)
 */
void BLDC_Step(void)
{
    const uint8_t N_CSTEPS = 6;

    static COMMUTATION_SECTOR_t bldc_step = 0;

    bldc_step += 1;
    bldc_step %= N_CSTEPS;

    if (global_uDC > 0)
    {
        /*
            each comm. step, need to shutdown all PWM for the "hold off" period (flyback settling) ???
               PWM_set_outputs(0, 0, 0);
        */
// TODO: set the PWM states (hi, lo plus minus) from look-up tables	???

        bldc_move( bldc_step );
    }
    else // motor drive output has been disabled
    {
        GPIOC->ODR &=  ~(1<<5);
        GPIOC->ODR &=  ~(1<<7);
        GPIOG->ODR &=  ~(1<<1);
        PWM_set_outputs(DC_OUTP_OFF, DC_OUTP_OFF, DC_OUTP_OFF);
    }
}

/*
 * TODO: schedule at 30degree intervals? (see TIM3)
 *
 * BUG? if PWM pulse is on at the step time to transition to floating, the PWM 
 * pulse is not turned off with good timing as the voltage just bleeds off then
 */
void bldc_move(COMMUTATION_SECTOR_t step )
{
/*
    each comm. step, need to shutdown all PWM for the "hold off" period (flyback settling)
*/
//        PWM_set_outputs(0, 0, 0);

// Start a short timer on which ISR will then  trigger the A/D with proper 
//  timing  .... at 1/4 of the comm. cycle ?
// So this TIM3 would not stepp 6 times but 6x4 times? (4 times precision?)
// ....... (yes seems necessary (refer to SiLabs appnote)


// /SD outputs on C5, C7, and G1
    switch( step )
    {
    default:

    case 0: // SECTOR_1 etc.
        GPIOC->ODR |=   (1<<5);      // A+-+
        GPIOC->ODR |=   (1<<7);      // B---
        GPIOG->ODR &=  ~(1<<1);      // C.
        PWM_set_outputs(DC_PWM_PLUS, DC_OUTP_LO, DC_OUTP_FLOAT);
        break;

    case 1:
        GPIOC->ODR |=   (1<<5);	     // A+-+
        GPIOC->ODR &=  ~(1<<7);      // B.
        GPIOG->ODR |=   (1<<1);      // C---
        PWM_set_outputs(DC_PWM_PLUS, DC_OUTP_FLOAT, DC_OUTP_LO);
        break;

    case 2:
        GPIOC->ODR &=  ~(1<<5);      // A.
        GPIOC->ODR |=   (1<<7);      // B+-+
        GPIOG->ODR |=   (1<<1);      // C---
        PWM_set_outputs(DC_OUTP_FLOAT, DC_PWM_PLUS, DC_OUTP_LO);
        break;

    case 3:
        GPIOC->ODR |=   (1<<5);      // A---
        GPIOC->ODR |=   (1<<7);      // B+-+
        GPIOG->ODR &=  ~(1<<1);      // C.
        PWM_set_outputs(DC_OUTP_LO, DC_PWM_PLUS, DC_OUTP_FLOAT);
        break;

    case 4:
        GPIOC->ODR |=   (1<<5);      // A---
        GPIOC->ODR &=  ~(1<<7);      // B.
        GPIOG->ODR |=   (1<<1);      // C+-+
        PWM_set_outputs(DC_OUTP_LO, DC_OUTP_FLOAT, DC_PWM_PLUS);
        break;

    case 5:
        GPIOC->ODR &=  ~(1<<5);      // A.
        GPIOC->ODR |=   (1<<7);      // B---
        GPIOG->ODR |=   (1<<1);      // C+-+
        PWM_set_outputs(DC_OUTP_FLOAT, DC_OUTP_LO, DC_PWM_PLUS);
        break;
    }
}
