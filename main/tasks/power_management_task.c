#include "global_state.h"
#include <string.h>
#include "esp_log.h"
#include "mining.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bm1397.h"
#include "EMC2101.h"
#include "INA260.h"
#include "math.h"
#include "serial.h"
#include "adc.h"
#include "nvs_config.h"

#define POLL_RATE 100
#define MAX_TEMP 90.0
#define THROTTLE_TEMP 80.0
#define THROTTLE_TEMP_RANGE (MAX_TEMP - THROTTLE_TEMP)

#define MIN_VOLTAGE             5000

#define ASIC_MODEL              CONFIG_ASIC_MODEL
#define MAX_CURRENT             CONFIG_ASIC_CURRENT
          

static const char * TAG = "power_management";

static float _fbound(float value, float lower_bound, float upper_bound)
{
	if (value < lower_bound)
		return lower_bound;
	if (value > upper_bound)
		return upper_bound;

	return value;
}

/************************************************************************************************************
 *  @brief Sample the sensor in a regular interval. It also apply a recursive average filter 
 * 
 *  @param [in] pvParameters
 * 
 *  @return none
************************************************************************************************************/
void Sensor_task(void * pvParameters)
{

    GlobalState *GLOBAL_STATE = (GlobalState*)pvParameters;
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    const float alpha = 0.95;
    const float beta  = 0.05;

    //initialize the power management variables
    power_management->voltage   = INA260_read_voltage();
    power_management->power     = INA260_read_power() / 1000;
    power_management->current   = INA260_read_current();
    power_management->fan_speed = EMC2101_get_fan_speed();
    power_management->chip_temp = EMC2101_get_external_temp();
    power_management->vcore     = ADC_get_vcore();
    power_management->efficiency = 0;

    while (1)
    {    
        power_management->voltage   = alpha*power_management->voltage     +   beta*INA260_read_voltage();
        power_management->power     = alpha*power_management->power       +   beta*INA260_read_power() / 1000;
        power_management->current   = alpha*power_management->current     +   beta*INA260_read_current();
        power_management->fan_speed = alpha*power_management->fan_speed   +   beta*EMC2101_get_fan_speed();
        power_management->chip_temp = alpha*power_management->chip_temp   +   beta*EMC2101_get_external_temp();
        power_management->vcore     = alpha*power_management->vcore       +   beta*ADC_get_vcore();
        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }

}

/************************************************************************************************************
 *  @brief Main power task. This task manage the BM1397 to keep its power consumption, clock and temperature 
 *         with the limits
 * 
 *  @param [in] pvParameters
 * 
 *  @return none
************************************************************************************************************/
void POWER_MANAGEMENT_task(void * pvParameters){

    GlobalState *GLOBAL_STATE = (GlobalState*)pvParameters;
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;

    float maxFreq = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);


    float voltage_multiplier = 0;
    float current_multiplier = ((float)power_management->frequency_value)/maxFreq; //get the multiplier value that matches to the initial value
    float power_multiplier = ((float)power_management->frequency_value)/maxFreq; //get the multiplier value that matches to the initial value

    float current_error = 0;
    float current_error_old = 0;
    float current_integral = 0;
    float current_setpoint;

    float voltage_error = 0;
    float last_frequency_increase = 0;
    float target_frequency;

    power_management->power_setpoint = nvs_config_get_u16(NVS_CONFIG_ASIC_MAXPOWER, CONFIG_ASIC_POWER)/1000.0;
    float power_error;

    float temp_setpoint = MAX_TEMP;
    float temp_error;
    float temp_error_old = 0;
    float temp_MV = 0;


    if(strcmp(ASIC_MODEL, "BM1397") != 0){
        ESP_LOGE(TAG, "Power task wasn't desing for the current BM ASIC");
        while(1);
    }
    
    while(1)
    {
        
        //Closed loop control
        // voltage_error = MIN_VOLTAGE - power_management->voltage;
        // voltage_multiplier = voltage_multiplier + voltage_error*0.00001;
        // voltage_multiplier = _fbound(voltage_multiplier, 0, 1);

        if (power_management->chip_temp > temp_setpoint )
        {
            ESP_LOGE(TAG, "Over Temperature %0.2fc", power_management->chip_temp);
        }

        temp_error = temp_setpoint - power_management->chip_temp;
        temp_MV = temp_MV + (temp_error-temp_error_old)*2 + temp_error*0.01;
        temp_error_old = temp_error;
        temp_MV = _fbound(temp_MV, -power_management->power_setpoint, 0);

        power_error = (power_management->power_setpoint + temp_MV) - power_management->power;
        power_multiplier = power_multiplier +  power_error*0.002;
        power_multiplier = _fbound(power_multiplier, 0, 1);

        //Closed-cascade loop control
        // current_error = MAX_CURRENT - power_management->current;
        // current_multiplier = current_multiplier +  current_error*0.00001;
        // current_multiplier = _fbound(current_multiplier, 0, 1);


        power_management->frequency_multiplier = power_multiplier;

        target_frequency = _fbound(power_management->frequency_multiplier * maxFreq, 25, maxFreq);

        power_management->frequency_value = target_frequency;

        if (fabs(last_frequency_increase - target_frequency) > 2)
        {
            last_frequency_increase = target_frequency;
            ESP_LOGI(TAG, "target %f, Freq %f, Temp %f, Power %f", target_frequency, power_management->frequency_value, power_management->chip_temp, power_management->power);
            BM1397_send_hash_frequency(target_frequency);
            
        }

        //ESP_LOGI(TAG, "target %f, Freq %f, Volt %f, Power %f", target_frequency, power_management->frequency_value, power_management->voltage, power_management->power);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}