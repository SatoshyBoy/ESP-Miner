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

#define VOLTAGE_START_THROTTLE 4900
#define VOLTAGE_MIN_THROTTLE 3500
#define VOLTAGE_RANGE (VOLTAGE_START_THROTTLE - VOLTAGE_MIN_THROTTLE)

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

void POWER_MANAGEMENT_task(void * pvParameters)
{

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    power_management->frequency_multiplier = 1;

    char * board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION, "unknown");
    power_management->HAS_POWER_EN =
        (strcmp(board_version, "202") == 1 || strcmp(board_version, "203") == 1 || strcmp(board_version, "204") == 1);
    power_management->HAS_PLUG_SENSE = strcmp(board_version, "204") == 1;
    free(board_version);

    int last_frequency_increase = 0;

    bool read_power = INA260_installed();

    uint16_t frequency_target = nvs_config_get_u16(NVS_CONFIG_ASIC_FREQ, CONFIG_ASIC_FREQUENCY);

    uint16_t auto_fan_speed = nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1);

    // Configure GPIO12 as input(barrel jack) 1 is plugged in
    gpio_config_t barrel_jack_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_12),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&barrel_jack_conf);
    int barrel_jack_plugged_in = gpio_get_level(GPIO_NUM_12);

    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    if (barrel_jack_plugged_in == 1 || !power_management->HAS_PLUG_SENSE) {
        // turn ASIC on
        gpio_set_level(GPIO_NUM_10, 0);
    } else {
        // turn ASIC off
        gpio_set_level(GPIO_NUM_10, 1);
    }

    vTaskDelay(3000 / portTICK_PERIOD_MS);

    while (1) {

        if (read_power == true) {
            power_management->voltage = INA260_read_voltage();
            power_management->power = INA260_read_power() / 1000;
            power_management->current = INA260_read_current();
        }
        power_management->fan_speed = EMC2101_get_fan_speed();

        if (strcmp(GLOBAL_STATE->asic_model, "BM1397") == 0) {

            power_management->chip_temp = EMC2101_get_external_temp();

            // Voltage
            // We'll throttle between 4.9v and 3.5v
            float voltage_multiplier =
                _fbound((power_management->voltage - VOLTAGE_MIN_THROTTLE) * (1 / (float) VOLTAGE_RANGE), 0, 1);

            // Temperature
            float temperature_multiplier = 1;
            float over_temp = -(THROTTLE_TEMP - power_management->chip_temp);
            if (over_temp > 0) {
                temperature_multiplier = (THROTTLE_TEMP_RANGE - over_temp) / THROTTLE_TEMP_RANGE;
            }

            float lowest_multiplier = 1;
            float multipliers[2] = {voltage_multiplier, temperature_multiplier};

            for (int i = 0; i < 2; i++) {
                if (multipliers[i] < lowest_multiplier) {
                    lowest_multiplier = multipliers[i];
                }
            }

            power_management->frequency_multiplier = lowest_multiplier;

            float target_frequency = _fbound(power_management->frequency_multiplier * frequency_target, 0, frequency_target);

            if (target_frequency < 50) {
                // TODO: Turn the chip off
            }

            // chip is coming back from a low/no voltage event
            if (power_management->frequency_value < 50 && target_frequency > 50) {
                // TODO recover gracefully?
                esp_restart();
            }

            if (power_management->frequency_value > target_frequency) {
                power_management->frequency_value = target_frequency;
                last_frequency_increase = 0;
                BM1397_send_hash_frequency(power_management->frequency_value);
                ESP_LOGI(TAG, "target %f, Freq %f, Temp %f, Power %f", target_frequency, power_management->frequency_value,
                         power_management->chip_temp, power_management->power);
            } else {
                if (last_frequency_increase > 120 && power_management->frequency_value != frequency_target) {
                    float add = (target_frequency + power_management->frequency_value) / 2;
                    power_management->frequency_value += _fbound(add, 2, 20);
                    BM1397_send_hash_frequency(power_management->frequency_value);
                    ESP_LOGI(TAG, "target %f, Freq %f, Temp %f, Power %f", target_frequency, power_management->frequency_value,
                             power_management->chip_temp, power_management->power);
                    last_frequency_increase = 60;
                } else {
                    last_frequency_increase++;
                }
            }
        } else if (strcmp(GLOBAL_STATE->asic_model, "BM1366") == 0 || strcmp(GLOBAL_STATE->asic_model, "BM1368") == 0) {
            power_management->chip_temp = EMC2101_get_internal_temp() + 5;

            if (power_management->chip_temp > THROTTLE_TEMP &&
                (power_management->frequency_value > 50 || power_management->voltage > 1000)) {
                ESP_LOGE(TAG, "OVERHEAT");


                if (power_management->HAS_POWER_EN) {
                    gpio_set_level(GPIO_NUM_10, 1);
                } else {
                    nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, 990);
                    nvs_config_set_u16(NVS_CONFIG_ASIC_FREQ, 50);
                    nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, 100);
                    nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, 0);
                    exit(EXIT_FAILURE);
                }

            }
        }

        if (auto_fan_speed == 1) {
            automatic_fan_speed(power_management->chip_temp);
        } else {
            EMC2101_set_fan_speed((float) nvs_config_get_u16(NVS_CONFIG_FAN_SPEED, 100) / 100);
        }
        // ESP_LOGI(TAG, "target %f, Freq %f, Volt %f, Power %f", target_frequency, power_management->frequency_value,
        // power_management->voltage, power_management->power);

        // Read the state of GPIO12
        if (power_management->HAS_PLUG_SENSE) {
            int gpio12_state = gpio_get_level(GPIO_NUM_12);
            if (gpio12_state == 0) {
                // turn ASIC off
                gpio_set_level(GPIO_NUM_10, 1);
            }
        }

        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }
}

// Set the fan speed between 20% min and 100% max based on chip temperature as input.
// The fan speed increases from 20% to 100% proportionally to the temperature increase from 50 and THROTTLE_TEMP
static void automatic_fan_speed(float chip_temp)
{
    double result = 0.0;
    double min_temp = 50.0;
    double min_fan_speed = 20.0;

    if (chip_temp < min_temp) {
        result = min_fan_speed;
    } else if (chip_temp >= THROTTLE_TEMP) {
        result = 100;
    } else {
        double temp_range = THROTTLE_TEMP - min_temp;
        double fan_range = 100 - min_fan_speed;
        result = ((chip_temp - min_temp) / temp_range) * fan_range + min_fan_speed;
    }

    EMC2101_set_fan_speed((float) result / 100);
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
void POWER_MANAGEMENT_task_V2(void * pvParameters){

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
        power_multiplier = power_multiplier +  power_error*0.008;
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
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
}