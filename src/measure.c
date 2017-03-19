#include "console.h"
#include "board.h"

void Output_Measurements(
        CONSOLE_OUTPUT_T *console_output, 
        BMS_INPUT_T* bms_input, 
        BMS_STATE_T* bms_state
) {

    char tempstr[20];

    if(console_output->measure_on) {
        if(console_output->measure_temp) {
            Board_Println("Not implemented yet!");
        }

        if(console_output->measure_voltage) {
            uint32_t i, j, idx;
            idx = 0;
            for (i = 0; i < bms_state->pack_config->num_modules; i++) {
                for (j = 0; j < bms_state->pack_config->module_cell_count[i]; j++) {
                    utoa(bms_input->pack_status->cell_voltages_mV[idx], tempstr, 10);
                    Board_Print_BLOCKING(tempstr);
                    Board_Print_BLOCKING(",");
                    idx++;
                }
                Board_Println_BLOCKING("\n----");
            }
        }

        if(console_output->measure_packcurrent) {
            Board_Println("Not implemented yet!");
        }

        if(console_output->measure_packvoltage) {
            Board_Println("Not implemented yet!");
        }
    }
}
