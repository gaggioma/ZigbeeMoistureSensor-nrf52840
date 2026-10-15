/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *
 * @brief Zigbee application template.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

//GPIO
#include <zephyr/drivers/gpio.h>

//ADC
#include <zephyr/drivers/adc.h>

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_app_utils.h>
#include <zb_nrf_platform.h>
#include <zcl/zb_zcl_analog_input.h>

//Basic cluster
#include "zb_basic_attr.h"

//moisture sensor cluster
#include "zb_moisture_sensor.h"
//Battery power cluster
#include "zb_power.h"

//Used for power_down_unused_ram()
#include <ram_pwrdn.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

//Used in Cluster attribute
#define MANUFACTOR_CODE ZB_ZCL_NON_MANUFACTURER_SPECIFIC

/* Device endpoint, used to receive ZCL commands. */
#define APP_POWER_EP 1
#define APP_MOISTURE_SENSOR_ENDPOINT 2

//Battery enable
static const struct gpio_dt_spec enable_voltage_read = GPIO_DT_SPEC_GET(DT_ALIAS(batteryenable), gpios);

//Led
static const struct gpio_dt_spec led_blue =  GPIO_DT_SPEC_GET(DT_ALIAS(ledblue), gpios);
static const struct gpio_dt_spec led_green =  GPIO_DT_SPEC_GET(DT_ALIAS(ledgreen), gpios);
static const struct gpio_dt_spec led_red =  GPIO_DT_SPEC_GET(DT_ALIAS(ledred), gpios);

//ADC init. Configure multiple channel.
//0 - for battery
//1 - read moisture sensor voltage
//Here an example for multiple chanel config:https://github.com/nrfconnect/sdk-zephyr/blob/v3.5.99-ncs1-1/samples/drivers/adc/src/main.c
#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

#define DT_SPEC_AND_COMMA(node_id, prop, idx) \
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

/* Data of ADC io-channels specified in devicetree. */
static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)
};

//ADC buffer Define and initialize a sequence to store samples captured by the ADC.
#define ADC_SAMPLES  100

//timer callback battery
static struct k_timer timer_interrupt_battery;
static struct k_work timer_callback_work_battery;

//timer callback moisture
static struct k_timer timer_interrupt_moisture;
static struct k_work timer_callback_work_moisture;

//prototypes for helpers
static int set_active_gpio(const struct gpio_dt_spec* gpio);
static int set_inactive_gpio(const struct gpio_dt_spec* gpio);
static int toggle_gpio(const struct gpio_dt_spec* gpio);


/////////////////////////////START ZIGBEE ENDPOINTS/////////////////////////////////////////

////////////BASIC and IDENTIFIES attributes. Add the same into every EP.
struct zb_device_ctx {
	zb_zcl_basic_attrs_ext_t basic_attr;
	zb_zcl_identify_attrs_t identify_attr;
};

//Storage for basic and identify attributes
static struct zb_device_ctx device_basic;

//BASIC attribute list
ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST_EXT
(
	basic_attr_list,
	&device_basic.basic_attr.zcl_version,
	&device_basic.basic_attr.app_version,
	&device_basic.basic_attr.stack_version,
	&device_basic.basic_attr.hw_version,
	device_basic.basic_attr.mf_name,
	device_basic.basic_attr.model_id,
	device_basic.basic_attr.date_code,
	&device_basic.basic_attr.power_source,
	device_basic.basic_attr.location_id,
	&device_basic.basic_attr.ph_env,
	device_basic.basic_attr.sw_ver
);

//IDENTIFY attribute list
ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	identify_attr_list,
	&device_basic.identify_attr.identify_time);

//Define moisture sensor attributes and attributes list

//////MOISTURE SENSOR/////////////////////////

//Define this to avoid compilation error
void zb_zcl_analog_input_init_server(void){

}

//moisture attributes
struct moisture_attributes_s {
    char description[50];
    zb_single_t max_present_value;
    zb_single_t min_present_value;
    zb_bool_t   out_of_service;
    zb_single_t present_value;     // The actual sensor value
    zb_uint8_t  reliability;
    zb_single_t resolution;
    zb_uint8_t  status_flags;
    zb_uint16_t engineering_units; // Used to explicitly declare unit type  SET THE MEASUREMENT UNIT (0x0005 = Volts, 0x00E1 = Millivolts, 0x001D: Percent (%))
    zb_uint32_t app_type;
};

static struct moisture_attributes_s moisture_attributes;

//Attribute list
ZB_ZCL_DECLARE_ANALOG_INPUT_ATTRIB_LIST	(	
	moisture_attr_list,
 	moisture_attributes.description,
 	&moisture_attributes.max_present_value,
 	&moisture_attributes.min_present_value,
 	&moisture_attributes.out_of_service,
 	&moisture_attributes.present_value,
 	&moisture_attributes.reliability,
 	&moisture_attributes.resolution,
 	&moisture_attributes.status_flags,
 	&moisture_attributes.engineering_units,
 	&moisture_attributes.app_type 
);	

//Pack all clusters into clusters list
ZB_DECLARE_MOISTURE_CLUSTER_LIST(
	moisture_cluster_list,
	basic_attr_list,
	identify_attr_list,
	moisture_attr_list
);

//Create EP
ZB_DECLARE_MOISTURE_EP(
	moisture_ep,
	APP_MOISTURE_SENSOR_ENDPOINT,
	moisture_cluster_list
);
/////////END MOISTURE SENSOR/////////////////

/////////////BATTERY EP///////////////////

//attributes
struct app_power_config_ctx_t {
    zb_uint8_t voltage; // Attribute 3.3.2.2.3.1
    zb_uint8_t size; // Attribute 3.3.2.2.4.2
    zb_uint8_t quantity; // Attribute 3.3.2.2.4.4
    zb_uint8_t rated_voltage; // Attribute 3.3.2.2.4.5
    zb_uint8_t alarm_mask; // Attribute 3.3.2.2.4.6
    zb_uint8_t voltage_min_threshold; // Attribute 3.3.2.2.4.7
    zb_uint8_t percent_remaining; // Attribute 3.3.2.2.3.1
    zb_uint8_t voltage_threshold_1; // Attribute 3.3.2.2.4.8
    zb_uint8_t voltage_threshold_2; // Attribute 3.3.2.2.4.8
    zb_uint8_t voltage_threshold_3; // Attribute 3.3.2.2.4.8
    zb_uint8_t percent_min_threshold; // Attribute 3.3.2.2.4.9
    zb_uint8_t percent_threshold_1; // Attribute 3.3.2.2.4.10
    zb_uint8_t percent_threshold_2; // Attribute 3.3.2.2.4.10
    zb_uint8_t percent_threshold_3; // Attribute 3.3.2.2.4.10
    zb_uint32_t alarm_state; // Attribute 3.3.2.2.4.11
};

struct app_power_config_ctx_t power_attr;

//Set power cluster attribute list
//In power cluster you can define this or leave blank. Mandatory for compilation
#define bat_num
ZB_ZCL_DECLARE_POWER_CONFIG_BATTERY_ATTRIB_LIST_EXT(
	power_attr_list,
	&power_attr.voltage,
	&power_attr.size,
	&power_attr.quantity,
	&power_attr.rated_voltage,
	&power_attr.alarm_mask,
	&power_attr.voltage_min_threshold,
	&power_attr.percent_remaining,
	&power_attr.voltage_threshold_1,
	&power_attr.voltage_threshold_2,
	&power_attr.voltage_threshold_3,
	&power_attr.percent_min_threshold,
	&power_attr.percent_threshold_1,
	&power_attr.percent_threshold_2,
	&power_attr.percent_threshold_3,
	&power_attr.alarm_state
);

//Pack all clusters into cluster list
ZB_DECLARE_POWER_CLUSTER_LIST(
	power_cluster_list,
	basic_attr_list,				  
	identify_attr_list,
	power_attr_list
);

//Define EP
ZB_DECLARE_POWER_EP(
	power_ep,
	APP_POWER_EP,
	power_cluster_list
);

////////////////////END BATTERY EP battery

//Pack all EP into device
ZBOSS_DECLARE_DEVICE_CTX_2_EP(
	zboss_app_ctx,
	power_ep,
	moisture_ep
);

/////////////////////////////END ZIGBEE ENDPOINTS/////////////////////////////////////////

static void app_basic_clusters_attr_init(void)
{
	//-----------------Basic attribute init---------------------
	device_basic.basic_attr.zcl_version   = ZB_ZCL_VERSION;
	device_basic.basic_attr.power_source  = ZB_ZCL_BASIC_POWER_SOURCE_UNKNOWN;
    device_basic.basic_attr.power_source  = BASIC_POWER_SOURCE;
    device_basic.basic_attr.stack_version = BASIC_STACK_VERSION;
    device_basic.basic_attr.hw_version    = BASIC_HW_VERSION;

    // Use ZB_ZCL_SET_STRING_VAL to set strings, because the first byte should
    // contain string length without trailing zero.
    //
    // For example "test" string wil be encoded as:
    //   [(0x4), 't', 'e', 's', 't']

    ZB_ZCL_SET_STRING_VAL(device_basic.basic_attr.mf_name,
                          BASIC_MANUF_NAME,
                          ZB_ZCL_STRING_CONST_SIZE(BASIC_MANUF_NAME));

    ZB_ZCL_SET_STRING_VAL(device_basic.basic_attr.model_id,
                          BASIC_MODEL_ID,
                          ZB_ZCL_STRING_CONST_SIZE(BASIC_MODEL_ID));

    ZB_ZCL_SET_STRING_VAL(device_basic.basic_attr.date_code,
                          BASIC_DATE_CODE,
                          ZB_ZCL_STRING_CONST_SIZE(BASIC_DATE_CODE));


    ZB_ZCL_SET_STRING_VAL(device_basic.basic_attr.location_id,
                          BASIC_LOCATION_DESC,
                          ZB_ZCL_STRING_CONST_SIZE(BASIC_LOCATION_DESC));


    device_basic.basic_attr.ph_env = BASIC_PH_ENV;

	// Identify cluster attributes data.
	device_basic.identify_attr.identify_time = ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;
	//------------------End Basic--------------------------------
}

static void app_moisture_cluster_attr_init(void){

	ZB_ZCL_SET_STRING_VAL(
		moisture_attributes.description,
        "sensor_voltage",
        ZB_ZCL_STRING_CONST_SIZE("sensor_voltage")
	);

	moisture_attributes.max_present_value = 0;
 	moisture_attributes.min_present_value = 0;
 	moisture_attributes.out_of_service = ZB_ZCL_ANALOG_INPUT_OUT_OF_SERVICE_DEFAULT_VALUE;
 	moisture_attributes.present_value = 0;
 	moisture_attributes.reliability = ZB_ZCL_ANALOG_INPUT_RELIABILITY_DEFAULT_VALUE;
 	moisture_attributes.resolution = 0.1;
 	moisture_attributes.status_flags = ZB_ZCL_ANALOG_INPUT_STATUS_FLAG_DEFAULT_VALUE;
 	moisture_attributes.engineering_units = 0x00E1; //mV
 	moisture_attributes.app_type = ZB_ZCL_AI_APP_TYPE_HUMIDITY;

}

static void app_power_cluster_attr_init(void){

	//Power attr
	power_attr.voltage               = ZB_ZCL_POWER_CONFIG_BATTERY_VOLTAGE_INVALID;
	power_attr.size                  = ZB_ZCL_POWER_CONFIG_BATTERY_SIZE_OTHER;
	power_attr.quantity              = 1; //1 battery
	power_attr.rated_voltage         = 42; //4.2V
	power_attr.alarm_mask            = ZB_ZCL_POWER_CONFIG_MAINS_ALARM_MASK_DEFAULT_VALUE;
	power_attr.voltage_min_threshold = ZB_ZCL_POWER_CONFIG_MAINS_VOLTAGE_MIN_THRESHOLD_DEFAULT_VALUE;
	power_attr.percent_remaining     = ZB_ZCL_POWER_CONFIG_BATTERY_REMAINING_UNKNOWN;
	power_attr.voltage_threshold_1   = ZB_ZCL_POWER_CONFIG_BATTERY_VOLTAGE_THRESHOLD1_DEFAULT_VALUE;
	power_attr.voltage_threshold_2   = ZB_ZCL_POWER_CONFIG_BATTERY_VOLTAGE_THRESHOLD2_DEFAULT_VALUE;
	power_attr.voltage_threshold_3   = ZB_ZCL_POWER_CONFIG_BATTERY_VOLTAGE_THRESHOLD3_DEFAULT_VALUE;
	power_attr.percent_min_threshold = ZB_ZCL_POWER_CONFIG_BATTERY_PERCENTAGE_MIN_THRESHOLD_DEFAULT_VALUE;
	power_attr.percent_threshold_1   = ZB_ZCL_POWER_CONFIG_BATTERY_PERCENTAGE_THRESHOLD1_DEFAULT_VALUE;
	power_attr.percent_threshold_2   = ZB_ZCL_POWER_CONFIG_BATTERY_PERCENTAGE_THRESHOLD2_DEFAULT_VALUE;
	power_attr.percent_threshold_3   = ZB_ZCL_POWER_CONFIG_BATTERY_PERCENTAGE_THRESHOLD3_DEFAULT_VALUE;
	power_attr.alarm_state           = ZB_ZCL_POWER_CONFIG_BATTERY_ALARM_STATE_DEFAULT_VALUE;
}

static void configureReporting(zb_uint8_t ep_id, zb_uint16_t cluster_id, zb_uint16_t attr_id){

	zb_ret_t ret_code;

	zb_zcl_reporting_info_t rep_config;
	memset(&rep_config, 0, sizeof(rep_config));

	rep_config.direction             = ZB_ZCL_CONFIGURE_REPORTING_SEND_REPORT;
	rep_config.ep                    = ep_id;
	rep_config.cluster_id            = cluster_id;
	rep_config.cluster_role          = ZB_ZCL_CLUSTER_SERVER_ROLE;
	rep_config.manuf_code 		    = MANUFACTOR_CODE;
	rep_config.attr_id               = attr_id;
	rep_config.u.send_info.min_interval = ZB_ZCL_MIN_REPORTING_INTERVAL_DEFAULT;   // 10 seconds min
	rep_config.u.send_info.max_interval = ZB_ZCL_MAX_REPORTING_INTERVAL_DEFAULT;  //If Maximum reporting interval is set to value 0xFFFF,reporting is not needed for current attribute.
	rep_config.u.send_info.delta.u8 = 0x00; // Value change threshold
	rep_config.u.send_info.reported_value.u8 = 0;
	
	// Inject configuration first
	ret_code = zb_zcl_put_reporting_info(&rep_config, ZB_TRUE);
	if(ret_code != RET_OK){
		LOG_ERR("error configureReporting ep: %d, error: %d", ep_id, ret_code);
	}else{
		LOG_INF("configureReporting ep: %d:", ep_id);
	};
} 

static void startReporting(zb_uint8_t ep_id, zb_uint16_t cluster_id, zb_uint16_t attr_id){

	zb_ret_t result;

	//percentage
	result = zb_zcl_start_attr_reporting_manuf(
        ep_id,
        cluster_id,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
        attr_id,
		MANUFACTOR_CODE
    );

	if (result != RET_OK) {
    	// RET_NOT_FOUND means reporting context or attribute entry is missing
    	// RET_INVALID_PARAMETER means endpoint/cluster role mismatches
    	LOG_ERR("Start reporting: error %d", result); 
	}else{
		LOG_INF("Start reporting ok"); 
	}
}

static void reportMoisture(float value_to_set){

    // Use the ZBOSS ZCL common macro to update the attribute table
    zb_zcl_status_t response = zb_zcl_set_attr_val(
        APP_MOISTURE_SENSOR_ENDPOINT,                                   // Target Endpoint ID
        ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,             // Cluster ID (Binary Input)
        ZB_ZCL_CLUSTER_SERVER_ROLE,                 // Cluster Role (Server)
        ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,  // Attribute ID (PresentValue)
         (zb_uint8_t *)&value_to_set,               // Pointer to the new value
        ZB_FALSE                                    // check_access flag (ZB_FALSE bypasses read-only constraints locally)
    );
	if(response == ZB_ZCL_STATUS_READ_ONLY ){
		LOG_INF("reportMoisture ZB_ZCL_STATUS_READ_ONLY");
	}else if(response == ZB_ZCL_STATUS_SUCCESS ){
		LOG_INF("reportMoisture ZB_ZCL_STATUS_SUCCESS");
	}else if(response == ZB_ZCL_STATUS_INVALID_VALUE ){
		LOG_INF("reportMoisture ZB_ZCL_STATUS_INVALID_VALUE");
	};
}

void reportBattery(float new_voltage, float max_value) {

    // Safely update the underlying structure value
	float perc = (new_voltage/max_value)*100.0f;
	zb_uint8_t perc_val = (zb_uint8_t)(perc * 2.0f);
	zb_uint8_t volt_to_set = (zb_uint8_t)(new_voltage * 10.0f);
	
	LOG_INF("update_battery_voltage v: %f -> %u", (double)new_voltage, volt_to_set);
	LOG_INF("update_battery_voltage perc: %f -> %u", (double)perc, perc_val);

	//Perc
	zb_zcl_status_t battery_perc_rep_status = zb_zcl_set_attr_val(
        APP_POWER_EP,
        ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
        (zb_uint8_t *)&perc_val, //The pointer MUST be 8bit
        ZB_FALSE
    );
	if(battery_perc_rep_status == ZB_ZCL_STATUS_READ_ONLY ){
		LOG_INF("battery_perc_rep_status ZB_ZCL_STATUS_READ_ONLY");
	}else if(battery_perc_rep_status == ZB_ZCL_STATUS_SUCCESS ){
		LOG_INF("battery_perc_rep_status ZB_ZCL_STATUS_SUCCESS");
	}else if(battery_perc_rep_status == ZB_ZCL_STATUS_INVALID_VALUE ){
		LOG_INF("battery_perc_rep_status ZB_ZCL_STATUS_INVALID_VALUE");
	};

	//Voltage
	zb_zcl_status_t battery_volt_rep_status = zb_zcl_set_attr_val(
        APP_POWER_EP,
        ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
        (zb_uint8_t *)&volt_to_set, //The pointer MUST be 8bit
        ZB_FALSE
    );

	if(battery_volt_rep_status == ZB_ZCL_STATUS_READ_ONLY ){
		LOG_INF("battery_volt_rep_status ZB_ZCL_STATUS_READ_ONLY");
	}else if(battery_volt_rep_status == ZB_ZCL_STATUS_SUCCESS ){
		LOG_INF("battery_volt_rep_status ZB_ZCL_STATUS_SUCCESS");
	}else if(battery_volt_rep_status == ZB_ZCL_STATUS_INVALID_VALUE ){
		LOG_INF("battery_volt_rep_status ZB_ZCL_STATUS_INVALID_VALUE");
	}
}

/**@brief Function to toggle the identify LED
 *
 * @param  bufid  Unused parameter, required by ZBOSS scheduler API.
 */
static void toggle_identify_led(zb_bufid_t bufid)
{
	//static int blink_status;
	LOG_INF("toggle_identify_led: %u", bufid);
	toggle_gpio(&led_blue);
	ZB_SCHEDULE_APP_ALARM(toggle_identify_led, bufid, ZB_MILLISECONDS_TO_BEACON_INTERVAL(100));
}

/**@brief Function to handle identify notification events on the first endpoint.
 *
 * @param  bufid  Unused parameter, required by ZBOSS scheduler API.
 */
static void identify_cb(zb_bufid_t bufid)
{
	zb_ret_t zb_err_code;
	LOG_INF("identify_cb bufid: %u", bufid);

	if (bufid) {
		/* Schedule a self-scheduling function that will toggle the LED. */
		ZB_SCHEDULE_APP_CALLBACK(toggle_identify_led, bufid);
		LOG_INF("Schedule a self-scheduling ep_id: %u", bufid);
	} else {
		/* Cancel the toggling function alarm and turn off LED. */
		zb_err_code = ZB_SCHEDULE_APP_ALARM_CANCEL(toggle_identify_led, ZB_ALARM_ANY_PARAM);
		ZVUNUSED(zb_err_code);

		/* Update network status/idenitfication LED. */
		if (ZB_JOINED()) {
			LOG_INF("Joined ep_id: %u", bufid);
			set_inactive_gpio(&led_green);
			set_inactive_gpio(&led_red);
			set_inactive_gpio(&led_blue);			
		} else {
			LOG_INF("Not joined ep_id: %u", bufid);
			set_inactive_gpio(&led_green);
			set_active_gpio(&led_red);
			set_inactive_gpio(&led_blue);			

		}
	}
}

/**@brief Starts identifying the device.
 *
 * @param  bufid  Unused parameter, required by ZBOSS scheduler API.
 */
/*static void start_identifying(zb_bufid_t bufid)
{
	ZVUNUSED(bufid);

	if (ZB_JOINED()) {
		// Check if endpoint is in identifying mode,
		 // if not put desired endpoint in identifying mode.
		 //
		if (device_basic.identify_attr.identify_time ==
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE) {

			zb_ret_t zb_err_code = zb_bdb_finding_binding_target(
				APP_MOISTURE_SENSOR_ENDPOINT);

			if (zb_err_code == RET_OK) {
				LOG_INF("Enter identify mode");
			} else if (zb_err_code == RET_INVALID_STATE) {
				LOG_WRN("RET_INVALID_STATE - Cannot enter identify mode");
			} else {
				ZB_ERROR_CHECK(zb_err_code);
			}
		} else {
			LOG_INF("Cancel identify mode");
			zb_bdb_finding_binding_target_cancel();
		}
	} else {
		LOG_WRN("Device not in a network - cannot enter identify mode");
	}
}*/

/**@brief Zigbee stack event handler.
 *
 * @param[in]   bufid   Reference to the Zigbee stack buffer
 *                      used to pass signal.
 */
void zboss_signal_handler(zb_bufid_t bufid)
{
	/* 1. Retrieve the signal headers from the buffer ID */
    zb_zdo_app_signal_hdr_t  *sig_header = NULL;
    zb_zdo_app_signal_type_t  sig_type   = zb_get_app_signal(bufid, &sig_header);
    zb_ret_t                  status     = ZB_GET_APP_SIGNAL_STATUS(bufid);

	set_inactive_gpio(&led_green);
	set_inactive_gpio(&led_blue);

	//Manage message
	/* 2. Custom Application Event Handling */
    switch (sig_type)
    {
        case ZB_BDB_SIGNAL_DEVICE_FIRST_START:
			LOG_INF("ZB_BDB_SIGNAL_DEVICE_FIRST_START OK");
			break;

        case ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (status == RET_OK) {
                // Device successfully initialized or joined network from reboot
				LOG_INF("ZB_BDB_SIGNAL_DEVICE_REBOOT OK");
				set_inactive_gpio(&led_red);
            } else {
                // Initial startup network join failed
				set_active_gpio(&led_red);
            }
            break;

        case ZB_BDB_SIGNAL_STEERING:
            if (status == RET_OK) {
                // Network steering process succeeded (e.g. end device pairing ok)
				LOG_INF("ZB_BDB_SIGNAL_STEERING OK");
				set_inactive_gpio(&led_red);
            } else {
                // Rejoin failed
				set_active_gpio(&led_red);
            }
            break;

        case ZB_ZDO_SIGNAL_LEAVE:
            // Device has left the network
			//Try a rejoin procedure
			//sys_reboot(SYS_REBOOT_WARM);
			LOG_INF("ZB_ZDO_SIGNAL_LEAVE OK");
			bdb_start_top_level_commissioning(ZB_BDB_NETWORK_STEERING);
            break;

        default:
            /* Don't explicitly block unhandled events */
            break;
    }

	/* No application-specific behavior is required.
	 * Call default signal handler.
	 */
	ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));

	/* All callbacks should either reuse or free passed buffers.
	 * If bufid == 0, the buffer is invalid (not passed).
	 */
	if (bufid) {
		zb_buf_free(bufid);
	}
}

/*All binary_input go here*/
int configure_gpio_in(const struct gpio_dt_spec* gpio){

	if (!gpio_is_ready_dt(gpio)) {
		return 0;
	}
	gpio_pin_configure_dt(gpio, GPIO_INPUT);

	return 1;
}

/*All output gpios go here*/
int configure_gpio_out(const struct gpio_dt_spec* gpio){
	
	if (!gpio_is_ready_dt(gpio)) {
		return 0;
	}
	gpio_pin_configure_dt(gpio, GPIO_OUTPUT);
	
	return 1;	
}

int set_active_gpio(const struct gpio_dt_spec* gpio){
	gpio_pin_set_dt(gpio, 1);
	return 1;
}

int set_inactive_gpio(const struct gpio_dt_spec* gpio){
	gpio_pin_set_dt(gpio, 0);
	return 1;
}

int toggle_gpio(const struct gpio_dt_spec* gpio){
	 gpio_pin_toggle_dt(gpio);
	 return 1;
}

/*Get effective battery value. ADC measure then divider voltage*/
float getBatteryValue(float adc_value){
  float r1 = 27*10^3;
  float r2 = 100*10^3;
  float alpha = r2/(r1+r2); 
  return ( (float)adc_value / alpha );
}

int measureSensor(char* channel_type){

	int channel = 0;
	if(strcmp(channel_type, "battery") == 0){
		channel = 0;
	}else if(strcmp(channel_type, "moisture") == 0){
		channel = 1;
	};

	LOG_INF("channel_type: %s -> channel: %d", channel_type, channel);

	//err var
	int err;

	//Enable voltage divider
	set_active_gpio(&enable_voltage_read);

	//Stabilize
	k_msleep(500);

	struct adc_sequence_options options = {
		// Number of extra samples *beyond* the first one
		.extra_samplings = ADC_SAMPLES - 1,
		//Interval between consecutive samplings (in microseconds), 0 means sample as fast as possible, without involving any timer.
		.interval_us = 0,
		.callback = NULL,
		.user_data = NULL,
	};

	//Buffer in which store sampples
	int16_t adc_samples_buffer[ADC_SAMPLES];
	struct adc_sequence sequence = {
		.options = &options,
		.buffer = adc_samples_buffer,
		// buffer size in bytes, not number of samples
		.buffer_size = sizeof(adc_samples_buffer),
		//Optional
		//.calibrate = true,
	};

	if (!adc_is_ready_dt(&adc_channels[channel])) {
		LOG_ERR("ADC controller device %s not ready\n", adc_channels[channel].dev->name);
		return 0;
	}else{
		LOG_INF("ADC controller device %s ready\n", adc_channels[channel].dev->name);
	}

	err = adc_channel_setup_dt(&adc_channels[channel]);
	if (err < 0) {
		LOG_ERR("Could not setup channel #%d (%d)\n", 0, err);
		return 0;
	}

	err = adc_sequence_init_dt(&adc_channels[channel], &sequence);
	if (err < 0) {
		LOG_ERR("Could not initalize sequence");
		return 0;
	}

	//Read values
	err = adc_read(adc_channels[channel].dev, &sequence);
	if (err < 0) {
		LOG_ERR("Could not read (%d)", err);
		return 0;
	} 

	//Disable voltage divider
	set_inactive_gpio(&enable_voltage_read);
	
	//Show values
	float meanVolt = 0;
	for (int i = 0; i < ADC_SAMPLES; i++) {
		int32_t val_mv = adc_samples_buffer[i];
        
        int err = adc_raw_to_millivolts_dt(&adc_channels[channel], &val_mv);
        
        if (err < 0) {
            LOG_ERR("Sample %d: Raw = %d, mV = Not available\n", i, adc_samples_buffer[i]);
        } else {
            //LOG_INF("Sample %d: Raw = %d, mV = %d mV\n", i, adc_samples_buffer[i], val_mv);
			meanVolt += (int)val_mv;
        }

    }

	meanVolt = meanVolt / ADC_SAMPLES;
    LOG_INF("Measured mV = %d mV", (int)meanVolt);

	if(channel == 0){
		float battery_voltage = getBatteryValue(meanVolt);
		LOG_INF("Battery mV = %d mV", (int)battery_voltage);
		reportBattery((float)(battery_voltage/1000.0f), 4.2f); //in volt
	}else if (channel == 1)
	{
		LOG_INF("Moisture mV = %d mV", (int)meanVolt);
		reportMoisture(meanVolt);
	}	

	return 1;
}

////Define timer callback form battery//////
void battery_callback(struct k_timer *timer_id)
{
	k_work_submit(&timer_callback_work_battery);
}

//This execute the routine
void battery_callback_work_handler(struct k_work *work){
	LOG_INF("timer wake up for measureBattery()");
	measureSensor("battery");
	k_msleep(500);
}

void battery_timer_callback_init(){
	//Initialize the k_work item used to send zigbee update in gpio interrupt context.
	//This allow execution of routine into callback context (gpio or timer)
	k_work_init(&timer_callback_work_battery, battery_callback_work_handler);

	//Timer init
	k_timer_init(&timer_interrupt_battery, battery_callback, NULL);
	//it means that timer expires first time after Nsec, after that expires every Nsec
	k_timer_start(&timer_interrupt_battery, K_HOURS(24), K_HOURS(24));  
}
///////End battery timer////


////Define timer callback form moisture//////
void moisture_callback(struct k_timer *timer_id)
{
	k_work_submit(&timer_callback_work_moisture);
}

//This execute the routine
void moisture_callback_work_handler(struct k_work *work){
	LOG_INF("timer wake up for measureMoisture");
	measureSensor("moisture");
	k_msleep(500);
}

void moisture_timer_callback_init(){
	//Initialize the k_work item used to send zigbee update in gpio interrupt context.
	//This allow execution of routine into callback context (gpio or timer)
	k_work_init(&timer_callback_work_moisture, moisture_callback_work_handler);

	//Timer init
	k_timer_init(&timer_interrupt_moisture, moisture_callback, NULL);
	//it means that timer expires first time after Nsec, after that expires every Nsec
	k_timer_start(&timer_interrupt_moisture, K_MINUTES(10), K_MINUTES(20));
	//k_timer_start(&timer_interrupt_moisture, K_SECONDS(10), K_SECONDS(20)); 
}
///////End moisture timer////

int main(void)
{
	LOG_INF("Starting Zigbee application template example");

	//Reset led.
	configure_gpio_out(&led_blue);
	set_inactive_gpio(&led_blue);
	configure_gpio_out(&led_green);
	set_inactive_gpio(&led_green);
	configure_gpio_out(&led_red);
	set_inactive_gpio(&led_red);

	//Config read battery gpio	
	configure_gpio_out(&enable_voltage_read);
	set_inactive_gpio(&enable_voltage_read);

	/* Register device context (endpoints). */
	ZB_AF_REGISTER_DEVICE_CTX(&zboss_app_ctx);

	//Init attribute
	app_basic_clusters_attr_init();
	app_moisture_cluster_attr_init();
	app_power_cluster_attr_init();

	/* Register handlers to identify notifications */
	ZB_AF_SET_IDENTIFY_NOTIFICATION_HANDLER(APP_POWER_EP, identify_cb);
	ZB_AF_SET_IDENTIFY_NOTIFICATION_HANDLER(APP_MOISTURE_SENSOR_ENDPOINT, identify_cb);

	//turn rx off when deep sleep
	if(IS_ENABLED(CONFIG_PM_DEVICE)){
		LOG_INF("Enabled zigbee_configure_sleepy_behavior");
		zigbee_configure_sleepy_behavior(true);
	}
	
	/* Power off unused sections of RAM to lower device power consumption. */
	if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY)) {
		LOG_INF("Enabled power_down_unused_ram");
		power_down_unused_ram();
	}

	/* Start Zigbee default thread */
	zigbee_enable();

	LOG_INF("Zigbee application template started");

	//Configure and start reporting for battery VOLTAGE
	configureReporting(APP_POWER_EP, ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID);
	startReporting(APP_POWER_EP, ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID);

	//Configure and start reporting for battery PERCENTAGE
	configureReporting(APP_POWER_EP, ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);
	startReporting(APP_POWER_EP, ZB_ZCL_CLUSTER_ID_POWER_CONFIG, ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);

	//Configure and start reporting for moisture sensor
	configureReporting(APP_MOISTURE_SENSOR_ENDPOINT, ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID);
	startReporting(APP_MOISTURE_SENSOR_ENDPOINT, ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID);

	while(1){
		if(ZB_JOINED()){
			LOG_INF("Zigbee join ok");

			//Init timer
			battery_timer_callback_init();
			moisture_timer_callback_init();

			//Read and report state for the first time
			measureSensor("battery"); //Battery
			measureSensor("moisture"); //Moisture

			k_sleep(K_FOREVER);
		}
		
	}

	return 0;
}
