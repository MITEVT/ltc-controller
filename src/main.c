#include <string.h>
#include <stdlib.h>
#include "board.h"
#include "ssm.h"
#include "sysinit.h"
#include "console.h"
#include "eeprom_config.h"
#include "config.h"
#include "ltc6804.h"
#include "error_handler.h"

#define LED0 2, 8
#define LED1 2, 10

#define BAL_SW 1, 2
#define IOCON_BAL_SW IOCON_PIO1_2
#define CHRG_SW 1, 2
#define IOCON_CHRG_SW IOCON_PIO1_2
#define DISCHRG_SW 1, 2
#define IOCON_DISCHRG_SW IOCON_PIO1_2

#define EEPROM_BAUD 600000
#define EEPROM_CS_PIN 1, 7

#define Hertz2Ticks(freq) SystemCoreClock / freq
#define LTC_CELL_VOLTAGE_FREQ 10

volatile uint32_t msTicks;

static char str[10];

// memory allocation for BMS_OUTPUT_T
static bool balance_reqs[MAX_NUM_MODULES*MAX_CELLS_PER_MODULE];
static BMS_CHARGE_REQ_T charge_req;
static BMS_OUTPUT_T bms_output;

// memory allocation for BMS_INPUT_T
static BMS_PACK_STATUS_T pack_status;
static BMS_INPUT_T bms_input;

// memory allocation for BMS_STATE_T
static BMS_CHARGER_STATUS_T charger_status;
static uint32_t cell_voltages[MAX_NUM_MODULES*MAX_CELLS_PER_MODULE];
static uint8_t num_cells_in_modules[MAX_NUM_MODULES];
static PACK_CONFIG_T pack_config;
static BMS_STATE_T bms_state;

// memory allocation for LTC6804
// [TODO] remove magic numbers
static LTC6804_CONFIG_T ltc6804_config;
static LTC6804_STATE_T ltc6804_state;
static Chip_SSP_DATA_SETUP_T ltc6804_xf_setup;
static uint8_t ltc6804_tx_buf[4+15*6];
static uint8_t ltc6804_rx_buf[4+15*6];
static uint8_t ltc6804_cfg[6];
static uint16_t ltc6804_bal_list[15];
static LTC6804_ADC_RES_T ltc6804_adc_res;

// memory for console
static microrl_t rl;
static CONSOLE_OUTPUT_T console_output;

/****************************
 *          IRQ Vectors
 ****************************/

void SysTick_Handler(void) {
	msTicks++;
}

static bool ltc6804_get_cell_voltages; // [TODO] put in right place

void TIMER32_0_IRQHandler(void) {
    if (Chip_TIMER_MatchPending(LPC_TIMER32_0, 0)) {
        Chip_TIMER_ClearMatch(LPC_TIMER32_0, 0);
        // Do something

        ltc6804_get_cell_voltages = true;
    }
}

/****************************
 *          HELPERS
 ****************************/

// [TODO] Remove
static void delay(uint32_t dlyTicks) {
	uint32_t curTicks = msTicks;
	while ((msTicks - curTicks) < dlyTicks);
}

/****************************
 *       INITIALIZERS
 ****************************/

static void Init_Core(void) {
	SysTick_Config (TicksPerMS);
}

static void Init_GPIO(void) {
    // [TODO] verify that pins don't collide
    //  move pin selections to preprocesser defines
	Chip_GPIO_Init(LPC_GPIO);
	Chip_GPIO_WriteDirBit(LPC_GPIO, LED0, true);
    Chip_GPIO_WriteDirBit(LPC_GPIO, LED1, true);

    Chip_GPIO_WriteDirBit(LPC_GPIO, BAL_SW, false);
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_BAL_SW, IOCON_MODE_PULLUP);
    Chip_GPIO_WriteDirBit(LPC_GPIO, CHRG_SW, false);
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_CHRG_SW, IOCON_MODE_PULLUP);
    Chip_GPIO_WriteDirBit(LPC_GPIO, DISCHRG_SW, false);
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_DISCHRG_SW, IOCON_MODE_PULLUP);
    
    //SSP for EEPROM
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO2_2, (IOCON_FUNC2 | IOCON_MODE_INACT));    /* MISO1 */ 
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO2_3, (IOCON_FUNC2 | IOCON_MODE_INACT));    /* MOSI1 */
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO2_1, (IOCON_FUNC2 | IOCON_MODE_INACT));    /* SCK1 */

    //SSP for LTC6804
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_8, (IOCON_FUNC1 | IOCON_MODE_INACT));    /* MISO0 */ 
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_9, (IOCON_FUNC1 | IOCON_MODE_INACT));    /* MOSI0 */
    Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_6, (IOCON_FUNC2 | IOCON_MODE_INACT));    /* SCK0 */
    Chip_IOCON_PinLocSel(LPC_IOCON, IOCON_SCKLOC_PIO0_6);
}

void Init_Timers(void) {
    // Timer 32_0 initialization
    Chip_TIMER_Init(LPC_TIMER32_0);
    Chip_TIMER_Reset(LPC_TIMER32_0);
    Chip_TIMER_MatchEnableInt(LPC_TIMER32_0, 0);
    Chip_TIMER_SetMatch(LPC_TIMER32_0, 0, Hertz2Ticks(LTC_CELL_VOLTAGE_FREQ));
    Chip_TIMER_ResetOnMatchEnable(LPC_TIMER32_0, 0);

}

void Enable_Timers(void) {
    NVIC_ClearPendingIRQ(TIMER_32_0_IRQn);
    NVIC_EnableIRQ(TIMER_32_0_IRQn);
    Chip_TIMER_Enable(LPC_TIMER32_0);
}


void Init_BMS_Structs(void) {
    bms_output.charge_req = &charge_req;
    bms_output.close_contactors = false;
    bms_output.balance_req = balance_reqs;
    memset(balance_reqs, 0, sizeof(balance_reqs[0])*MAX_NUM_MODULES*MAX_CELLS_PER_MODULE);
    bms_output.read_eeprom_packconfig = false;
    bms_output.check_packconfig_with_ltc = false;

    charge_req.charger_on = 0;
    charge_req.charge_current_mA = 0;
    charge_req.charge_voltage_mV = 0;

    bms_state.charger_status = &charger_status;
    bms_state.pack_config = &pack_config;
    bms_state.curr_mode = BMS_SSM_MODE_INIT;
    bms_state.init_state = BMS_INIT_OFF;
    bms_state.charge_state = BMS_CHARGE_OFF;
    bms_state.discharge_state = BMS_DISCHARGE_OFF;
    bms_state.error_code = BMS_NO_ERROR;

    charger_status.connected = false;
    charger_status.error = false;

    pack_config.num_cells_in_modules = num_cells_in_modules;
    pack_config.cell_min_mV = 0;
    pack_config.cell_max_mV = 0;
    pack_config.cell_capacity_cAh = 0;
    pack_config.num_modules = 0;
    pack_config.cell_charge_c_rating_cC = 0;
    pack_config.bal_on_thresh_mV = 0;
    pack_config.bal_off_thresh_mV = 0;
    pack_config.pack_cells_p = 0;
    pack_config.cv_min_current_mA = 0;
    pack_config.cv_min_current_ms = 0;
    pack_config.cc_cell_voltage_mV = 0;

    pack_config.cell_discharge_c_rating_cC = 0; // at 27 degrees C
    pack_config.max_cell_temp_C = 0;

    //assign bms_inputs
    bms_input.hard_reset_line = false;
    bms_input.mode_request = BMS_SSM_MODE_STANDBY;
    bms_input.balance_mV = 0; // console request balance to mV
    bms_input.contactors_closed = false;
    bms_input.msTicks = msTicks;
    bms_input.pack_status = &pack_status;
    bms_input.eeprom_packconfig_read_done = false;
    bms_input.ltc_packconfig_check_done = false;
    bms_input.eeprom_read_error = false;
    bms_input.ltc_error = LTC_NO_ERROR; //[TODO] change to the type provided by the library

    memset(cell_voltages, 0, sizeof(cell_voltages));
    pack_status.cell_voltage_mV = cell_voltages;
    pack_status.pack_cell_max_mV = 0;
    pack_status.pack_cell_min_mV = 0xFFFFFFFF; // [TODO] this is a bodge fix
    pack_status.pack_current_mA = 0;
    pack_status.pack_voltage_mV = 0;
    pack_status.precharge_voltage = 0;
    pack_status.max_cell_temp_C = 0;
    pack_status.error = 0;

}

void Init_LTC6804(void) {
    // [TODO] For now do all LTC6804 Init here, 100k Baud, Port 0_7 for CS
    
    ltc6804_config.pSSP = LPC_SSP0;
    ltc6804_config.baud = 500000;
    ltc6804_config.cs_gpio = 0;
    ltc6804_config.cs_pin = 2;

    ltc6804_config.num_modules = pack_config.num_modules;
    ltc6804_config.module_cell_count = num_cells_in_modules;

    ltc6804_config.min_cell_mV = pack_config.cell_min_mV;
    ltc6804_config.max_cell_mV = pack_config.cell_max_mV;

    ltc6804_config.adc_mode = LTC6804_ADC_MODE_NORMAL;
    
    ltc6804_state.xf = &ltc6804_xf_setup;
    ltc6804_state.tx_buf = ltc6804_tx_buf;
    ltc6804_state.rx_buf = ltc6804_rx_buf;
    ltc6804_state.cfg = ltc6804_cfg;
    ltc6804_state.bal_list = ltc6804_bal_list;

    ltc6804_adc_res.cell_voltages_mV = pack_status.cell_voltage_mV;

    LTC6804_Init(&ltc6804_config, &ltc6804_state, msTicks);
}

void Process_Input(BMS_INPUT_T* bms_input) {
    // Read current mode request
    // Override Console Mode Request
    // Read pack status
    // Read hardware signal inputs
    // update and other fields in msTicks in &input

    if (console_output.valid_mode_request) {
        bms_input->mode_request = console_output.mode_request;
        bms_input->balance_mV = console_output.balance_mV;
    } else {
        bms_input->mode_request = BMS_SSM_MODE_STANDBY; // [TODO] Change this
    }

    // if (Chip_GPIO_GetPinState(LPC_GPIO, BAL_SW)) {
    //     bms_input->mode_request = BMS_SSM_MODE_BALANCE;
    //     bms_input->balance_mV = 3300;
    // } else if (Chip_GPIO_GetPinState(LPC_GPIO, CHRG_SW)) {
    //     bms_input->mode_request = BMS_SSM_MODE_CHARGE;
    // } else if (Chip_GPIO_GetPinState(LPC_GPIO, DISCHRG_SW)) {
    //     bms_input->mode_request = BMS_SSM_MODE_DISCHARGE;
    // } else {
    //     bms_input->mode_request = BMS_SSM_MODE_STANDBY;
    // }


    // [TODO] Check if-statement logic
    if (ltc6804_get_cell_voltages) {
        LTC6804_STATUS_T res = LTC6804_GetCellVoltages(&ltc6804_config, &ltc6804_state, &ltc6804_adc_res, msTicks);
        if (res == LTC6804_FAIL) Board_Println("LTC6804_GetCellVol FAIL");
        if (res == LTC6804_PEC_ERROR) {
            Board_Println("LTC6804_GetCellVol PEC_ERROR");
            Error_Assert(ERROR_LTC6804_PEC,bms_input->msTicks);
        } 
        else {
            Error_Pass(ERROR_LTC6804_PEC);
        }
        if (res == LTC6804_SPI_ERROR) Board_Println("LTC6804_GetCellVol SPI_ERROR");
        if (res == LTC6804_PASS) {
            pack_status.pack_cell_min_mV = ltc6804_adc_res.pack_cell_min_mV;
            pack_status.pack_cell_max_mV = ltc6804_adc_res.pack_cell_max_mV;
            LTC6804_ClearCellVoltages(&ltc6804_config, &ltc6804_state, msTicks);
            ltc6804_get_cell_voltages = false;
        }
    }

    bms_input->msTicks = msTicks;
}

void Process_Output(BMS_INPUT_T* bms_input, BMS_OUTPUT_T* bms_output) {
    // If SSM changed state, output appropriate visual indicators
    // Carry out appropriate hardware output requests (CAN messages, charger requests, etc.)
    // Board_Println_BLOCKING("Process output");
    if (bms_output->read_eeprom_packconfig){
        bms_input->eeprom_packconfig_read_done = EEPROM_Load_PackConfig(&pack_config);
        Charge_Config(&pack_config);
        Discharge_Config(&pack_config);
    }
    else if (bms_output->check_packconfig_with_ltc) {
        bms_input->ltc_packconfig_check_done = 
            EEPROM_Check_PackConfig_With_LTC(&pack_config);
        Init_LTC6804();
        Board_Print("Initializing LTC6804. Verifying..");
        if (!LTC6804_VerifyCFG(&ltc6804_config, &ltc6804_state, msTicks)) {
            Board_Print(".FAIL. ");
            bms_input->ltc_packconfig_check_done = false;
        } else {
            Board_Print(".PASS. ");
        }

        LTC6804_STATUS_T res;
        Board_Print("CVST..");
        while((res = LTC6804_CVST(&ltc6804_config, &ltc6804_state, msTicks)) != LTC6804_PASS) {
            if (res == LTC6804_FAIL) {
                Board_Print(".FAIL (");

                int i;
                for (i = 0; i < 12; i++) {
                    itoa(ltc6804_state.rx_buf[i], str, 16);
                    Board_Print(str);
                    Board_Print(", ");
                }
                Board_Println(")");
                bms_input->ltc_packconfig_check_done = false;
                break;
            } else if (res == LTC6804_SPI_ERROR) {
                Board_Println(".SPI_ERROR");
                bms_input->ltc_packconfig_check_done = false;
                break;
            } else if (res == LTC6804_PEC_ERROR) {
                Board_Println(".PEC_ERROR");
                bms_input->ltc_packconfig_check_done = false;
                break;
            }
        } 
        if (res == LTC6804_PASS) Board_Println(".PASS");
        Enable_Timers(); // [TODO] Put in right place
    }

    // [TODO] Think about what happens on sleep and only updating on change
    // [TODO] If statement sucks
    if (bms_state.curr_mode != BMS_SSM_MODE_INIT) {
        LTC6804_STATUS_T res;
        res = LTC6804_UpdateBalanceStates(&ltc6804_config, &ltc6804_state, bms_output->balance_req, msTicks);
        if (res == LTC6804_SPI_ERROR) {
            Board_Println("SetBalanceStates SPI_ERROR");
        }
    }

}

// [TODO] Turn off and move command prompt when others are typing....bitch
//      Board Print should tell microrl to gtfo
void Process_Keyboard(void) {
    uint32_t readln = Board_Read(str,50);
    uint32_t i;
    for(i = 0; i < readln; i++) {
        microrl_insert_char(&rl, str[i]);
    }
}


int main(void) {

    Init_Core();
    Init_GPIO();
    Init_Timers(); // [TODO] Think about proper place to put this
    ltc6804_get_cell_voltages = false; // [TODO] Same as above
    EEPROM_init(LPC_SSP0, EEPROM_BAUD, EEPROM_CS_PIN);

    Init_BMS_Structs();
    Board_UART_Init(UART_BAUD);

    Board_Println("Started Up");    
    

    //setup readline
    microrl_init(&rl, Board_Print);
    microrl_set_execute_callback(&rl, executerl);
    console_init(&bms_input, &bms_state, &console_output);

    SSM_Init(&bms_input, &bms_state, &bms_output);

    uint32_t last_count = msTicks;

	while(1) {
        Process_Keyboard(); //do this if you want to add the command line
        Process_Input(&bms_input);
        SSM_Step(&bms_input, &bms_state, &bms_output); 
        Process_Output(&bms_input, &bms_output);
        Error_Handle(bms_input.msTicks);
        
        // Testing Code
        bms_input.contactors_closed = bms_output.close_contactors; // [DEBUG] For testing purposes

        // LED Heartbeat
        if (msTicks - last_count > 1000) {
            last_count = msTicks;
            Chip_GPIO_SetPinState(LPC_GPIO, LED0, 1 - Chip_GPIO_GetPinState(LPC_GPIO, LED0));     
        }
    }

	return 0;
}

