#include "global_state.h"
#include "work_queue.h"
#include "serial.h"
#include "bm1397.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_config.h"
#include "utils.h"
#include "esp_system.h"

const char *TAG = "asic_result";

void ASIC_efficiency(GlobalState * GLOBAL_STATE)
{
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    static u_int16_t samples = 0;

    // Moving average filter
    if (module->current_hashrate > 0.1) {
        power_management->efficiency = (samples * power_management->efficiency +
                                        (GLOBAL_STATE->POWER_MANAGEMENT_MODULE.power / (module->current_hashrate / 1000.0))) /
                                       (samples + 1);

        if (samples < 100)
            samples++;
    }
}

void ASIC_result_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    SERIAL_clear_buffer();

    char *user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, STRATUM_USER);

    while (1)
    {

        task_result *asic_result = (*GLOBAL_STATE->ASIC_functions.receive_result_fn)(GLOBAL_STATE);

        if(asic_result == NULL){
            GLOBAL_STATE->asic_result_null++;
            if (GLOBAL_STATE->asic_result_null > 10)
            {
                esp_restart();
            }
            ESP_LOGW(TAG, "BM returned NULL");
            continue;
        }
        GLOBAL_STATE->asic_result_null = 0;

        uint8_t job_id = asic_result->job_id;

        if (GLOBAL_STATE->valid_jobs[job_id] == 0)
        {
            ESP_LOGE(TAG, "Invalid job nonce found, id=%d", job_id);
        }

        // check the nonce difficulty
        double nonce_diff = test_nonce_value(
            GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id],
            asic_result->nonce,
            asic_result->rolled_version);

        if(nonce_diff > 0.1)
        ESP_LOGI(TAG, "Nonce difficulty %.2f of %ld.", nonce_diff, GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->pool_diff);
        else
        ESP_LOGW(TAG, "Nonce difficulty %.2f of %ld.", nonce_diff, GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->pool_diff);

        if (nonce_diff > GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->pool_diff)
        {
            SYSTEM_notify_found_nonce(
                &GLOBAL_STATE->SYSTEM_MODULE,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->pool_diff,
                nonce_diff,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->target,
                GLOBAL_STATE->POWER_MANAGEMENT_MODULE.power
                );

            ASIC_efficiency(GLOBAL_STATE);

            STRATUM_V1_submit_share(
                GLOBAL_STATE->sock,
                user,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->jobid,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->extranonce2,
                GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->ntime,
                asic_result->nonce,
                asic_result->rolled_version ^ GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->version);
        }
    }
}
