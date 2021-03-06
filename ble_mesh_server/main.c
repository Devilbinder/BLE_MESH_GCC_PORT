/* Copyright (c) 2010 - 2018, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
*
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "boards.h"
#include "simple_hal.h"
#include "log.h"
#include "access_config.h"
#include "simple_on_off_server.h"
#include "light_switch_example_common.h"
#include "mesh_app_utils.h"
#include "net_state.h"
#include "rtt_input.h"
#include "mesh_stack.h"
#include "mesh_softdevice_init.h"
#include "mesh_provisionee.h"
#include "nrf_mesh_config_examples.h"
#include "nrf_mesh_configure.h"
#include "app_timer.h"

#include "nrf_mesh_events.h"
#include "nrf_nvic.h"

#include "nrf_delay.h"

#include "SEGGER_RTT.h"
#include "SEGGER_RTT_Conf.h"


#define RTT_INPUT_POLL_PERIOD_MS (100)
#define LED_PIN_NUMBER           (BSP_LED_0)
#define LED_PIN_MASK             (1u << LED_PIN_NUMBER)
#define LED_BLINK_INTERVAL_MS    (200)
#define LED_BLINK_CNT_START      (2)
#define LED_BLINK_CNT_RESET      (3)
#define LED_BLINK_CNT_PROV       (4)

static simple_on_off_server_t m_server;
static bool                   m_device_provisioned;

static void provisioning_complete_cb(void)
{
    SEGGER_RTT_printf(0,"Successfully provisioned\n");

    dsm_local_unicast_address_t node_address;
    dsm_local_unicast_addresses_get(&node_address);
    SEGGER_RTT_printf(0,"Node Address: 0x%04x \n", node_address.address_start);

    hal_led_mask_set(LEDS_MASK, LED_MASK_STATE_OFF);
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_PROV);
}

static bool on_off_server_get_cb(const simple_on_off_server_t * p_server)
{
    SEGGER_RTT_printf(0,"on_off_server_get_cb");
    return hal_led_pin_get(LED_PIN_NUMBER);
}

static bool on_off_server_set_cb(const simple_on_off_server_t * p_server, bool value)
{
    SEGGER_RTT_printf(0,"Got SET command to %u\n", value);
    hal_led_pin_set(LED_PIN_NUMBER, value);
    return value;
}

static void node_reset(void)
{
    SEGGER_RTT_printf(0,"----- Node reset  -----\n");
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_RESET);
    /* This function may retu3rn if there are ongoing flash operations. */
    mesh_stack_device_reset();
}

static void config_server_evt_cb(const config_server_evt_t * p_evt)
{
    if (p_evt->type == CONFIG_SERVER_EVT_NODE_RESET)
    {
        node_reset();
    }
}

static void button_event_handler(uint32_t button_number)
{
    SEGGER_RTT_printf(0,"Button %u pressed\n", button_number);
    switch (button_number)
    {
        /* Pressing SW1 on the Development Kit will result in LED state to toggle and trigger
        the STATUS message to inform client about the state change. This is a demonstration of
        state change publication due to local event. */
        case 0:
        {
            uint8_t value = !hal_led_pin_get(LED_PIN_NUMBER);
            SEGGER_RTT_printf(0,"User action \n");
            hal_led_pin_set(LED_PIN_NUMBER, value);
            (void)simple_on_off_server_status_publish(&m_server, value);
            break;
        }

        /* Initiate node reset */
        case 3:
        {
            /* Clear all the states to reset the node. */
            mesh_stack_config_clear();
            node_reset();
            break;
        }

        default:
            break;
    }
}

static void app_rtt_input_handler(int key)
{
    if (key >= '0' && key <= '4')
    {
        uint32_t button_number = key - '0';
        button_event_handler(button_number);
    }
}

static void models_init_cb(void)
{
    SEGGER_RTT_printf(0,"Initializing and adding models\n");
    m_server.get_cb = on_off_server_get_cb;
    m_server.set_cb = on_off_server_set_cb;
    ERROR_CHECK(simple_on_off_server_init(&m_server, 0));
    ERROR_CHECK(access_model_subscription_list_alloc(m_server.model_handle));
}

static void mesh_init(void)
{
    uint8_t dev_uuid[NRF_MESH_UUID_SIZE];
    uint8_t node_uuid_prefix[SERVER_NODE_UUID_PREFIX_SIZE] = SERVER_NODE_UUID_PREFIX;

    ERROR_CHECK(mesh_app_uuid_gen(dev_uuid, node_uuid_prefix, SERVER_NODE_UUID_PREFIX_SIZE));
    mesh_stack_init_params_t init_params =
    {
        .core.irq_priority       = NRF_MESH_IRQ_PRIORITY_LOWEST,
        .core.lfclksrc           = DEV_BOARD_LF_CLK_CFG,
        .core.p_uuid             = dev_uuid,
        .models.models_init_cb   = models_init_cb,
        .models.config_server_cb = config_server_evt_cb
    };
    ERROR_CHECK(mesh_stack_init(&init_params, &m_device_provisioned));
}

static void initialize(void)
{
    __LOG_INIT(LOG_SRC_APP | LOG_SRC_ACCESS, LOG_LEVEL_INFO, LOG_CALLBACK_DEFAULT);
    SEGGER_RTT_printf(0,"----- BLE Mesh Light Switch Server Demo -----\n");

    ERROR_CHECK(app_timer_init());
    hal_leds_init();

#if BUTTON_BOARD
    ERROR_CHECK(hal_buttons_init(button_event_handler));
#endif

    nrf_clock_lf_cfg_t lfc_cfg = DEV_BOARD_LF_CLK_CFG;
    ERROR_CHECK(mesh_softdevice_init(lfc_cfg));
    mesh_init();
}

static void start(void)
{
    rtt_input_enable(app_rtt_input_handler, RTT_INPUT_POLL_PERIOD_MS);
    ERROR_CHECK(mesh_stack_start());

    if (!m_device_provisioned)
    {
        static const uint8_t static_auth_data[NRF_MESH_KEY_SIZE] = STATIC_AUTH_DATA;
        mesh_provisionee_start_params_t prov_start_params =
        {
            .p_static_data    = static_auth_data,
            .prov_complete_cb = provisioning_complete_cb,
            .p_device_uri = NULL
        };
        ERROR_CHECK(mesh_provisionee_prov_start(&prov_start_params));
    }

    const uint8_t *p_uuid = nrf_mesh_configure_device_uuid_get();
    UNUSED_VARIABLE(p_uuid);
    __LOG_XB(LOG_SRC_APP, LOG_LEVEL_INFO, "Device UUID ", p_uuid, NRF_MESH_UUID_SIZE);

    hal_led_mask_set(LEDS_MASK, LED_MASK_STATE_OFF);
    hal_led_blink_ms(LEDS_MASK, LED_BLINK_INTERVAL_MS, LED_BLINK_CNT_START);
}

int main(void)
{

    initialize();
    execution_start(start);

    //get_client_packet_bool();


    SEGGER_RTT_printf(0, "ENTER Main LOOP\n");
    for (;;)
    {
    	//SEGGER_RTT_printf(0, "ENTER Main LOOP\n");
        /*if(get_client_packet_bool()){
          SEGGER_RTT_printf(0,"Got packet from client\n");
          uint8_t value = hal_led_pin_get(LED_PIN_NUMBER);
          hal_led_pin_set(LED_PIN_NUMBER, value);
          (void)simple_on_off_server_status_publish(&m_server, value);
          set_client_packet_bool(false);
        }*/

        (void)sd_app_evt_wait();


    }
}
