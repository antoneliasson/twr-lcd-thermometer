#include <application.h>

// Maximum age of received measurements that are considered valid and should be displayed
const unsigned int STALE_MEASUREMENT_THRESHOLD = 60 * 60 * 1000;

#define BATTERY_PUBLISH_INTERVAL (60 * 60 * 1000)
#define TEMPERATURE_PUBLISH_INTERVAL (15 * 60 * 1000)
#define TEMPERATURE_PUBLISH_VALUE_CHANGE 1.0f
#define TEMPERATURE_MEASURE_INTERVAL (5 * 1000)

typedef struct
{
    uint8_t channel;
    float value;
    twr_tick_t next_pub;
} event_param_t;

// Thermometer instance
twr_tmp112_t tmp112;
event_param_t temperature_event_param;

static twr_scheduler_task_id_t display_update_task;
static twr_gfx_t *gfx;

struct display_data
{
    float in_temp;
    twr_tick_t in_temp_last_timestamp;
    float out_temp;
    twr_tick_t out_temp_last_timestamp;
};

struct display_data display_data = {.in_temp = NAN, .out_temp = NAN};

extern void application_error(twr_error_t code);
static void radio_update_sensor(uint64_t *id, const char *topic, void *value, void *param);

enum
{
    SUB_CLIMATE_INFO,
};

/* Must apparently have the format "update/-/xyz...".
 * Cannot be too long. Around 32 character subtopic seems ok but not much longer. */
static const twr_radio_sub_t subs[] = {
    /* {"update/-/notif/state", TWR_RADIO_SUB_PT_BOOL, mailbox_notification_update, NULL}, */
    {"update/-/climate/info", TWR_RADIO_SUB_PT_STRING, radio_update_sensor, (void *)SUB_CLIMATE_INFO},
};

static void radio_update_sensor(uint64_t *id, const char *topic, void *value, void *param)
{
    (void)id;
    int sub = (int)param;
    char *val = value;
    float unused;

    twr_log_info("%s: topic: %s=%s", __func__, topic, val);

    switch (sub)
    {
    case SUB_CLIMATE_INFO:
    {
        // currently has an unused item just to ensure that I get the
        // stringification and destringification right
        int items = sscanf(val, "%f;%f", &display_data.out_temp, &unused);
        if (items != 2)
        {
            twr_log_warning("%s: expected 2 items, got %d", __func__, items);
        }
        display_data.out_temp_last_timestamp = twr_tick_get();
        break;
    }
    default:
        application_error(TWR_ERROR_INVALID_PARAMETER);
        break;
    }

    twr_scheduler_plan_now(display_update_task);
}

void tmp112_event_handler(twr_tmp112_t *self, twr_tmp112_event_t event, void *event_param)
{
    event_param_t *param = (event_param_t *)event_param;

    if (event == TWR_TMP112_EVENT_UPDATE)
    {
        float value;

        if (twr_tmp112_get_temperature_celsius(self, &value))
        {
            if ((fabsf(value - param->value) >= TEMPERATURE_PUBLISH_VALUE_CHANGE) || (param->next_pub < twr_scheduler_get_spin_tick()))
            {
                twr_log_debug("APP: temperature: %.2f °C", value);

                twr_radio_pub_temperature(param->channel, &value);
                param->value = value;
                param->next_pub = twr_scheduler_get_spin_tick() + TEMPERATURE_PUBLISH_INTERVAL;

                display_data.in_temp = value;
                display_data.in_temp_last_timestamp = twr_tick_get();
                twr_scheduler_plan_now(display_update_task);
            }
        }
    }
}

static void draw_lcd_weather_page(void)
{
    twr_gfx_clear(gfx);

    twr_gfx_set_font(gfx, &twr_font_ubuntu_15);
    twr_gfx_printf(gfx, 0, 8, true, "Inne");
    twr_gfx_set_font(gfx, &twr_font_ubuntu_33);
    if (!isnan(display_data.in_temp) && twr_tick_get() - display_data.in_temp_last_timestamp < STALE_MEASUREMENT_THRESHOLD)
        twr_gfx_printf(gfx, 12, 24, true, "%.1f °C", display_data.in_temp);

    twr_gfx_draw_line(gfx, 8, 64, 120, 64, true);

    twr_gfx_set_font(gfx, &twr_font_ubuntu_15);
    twr_gfx_printf(gfx, 0, 72, true, "Ute");
    twr_gfx_set_font(gfx, &twr_font_ubuntu_33);
    if (!isnan(display_data.out_temp) && twr_tick_get() - display_data.out_temp_last_timestamp < STALE_MEASUREMENT_THRESHOLD)
        twr_gfx_printf(gfx, 12, 88, true, "%.1f °C", display_data.out_temp);
}

static void display_update(void *param)
{
    (void)param;
    // twr_log_debug("%s enter", __func__);
    twr_system_pll_enable();
    if (!twr_module_lcd_is_ready())
    {
        twr_log_debug("%s not ready", __func__);
        twr_scheduler_plan_current_from_now(10);
    }
    else
    {
        draw_lcd_weather_page();
        twr_gfx_update(gfx);
    }
    twr_system_pll_disable();
    // twr_log_debug("%s leave", __func__);
}

void application_init(void)
{
    twr_log_init(TWR_LOG_LEVEL_DUMP, TWR_LOG_TIMESTAMP_ABS);

    twr_module_lcd_init();
    gfx = twr_module_lcd_get_gfx();

    display_update_task = twr_scheduler_register(display_update, NULL, 0);

    // Initialize thermometer on core module
    temperature_event_param.channel = TWR_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_ALTERNATE;
    twr_tmp112_init(&tmp112, TWR_I2C_I2C0, TWR_TAG_TEMPERATURE_I2C_ADDRESS_ALTERNATE);
    twr_tmp112_set_event_handler(&tmp112, tmp112_event_handler, &temperature_event_param);
    twr_tmp112_set_update_interval(&tmp112, TEMPERATURE_MEASURE_INTERVAL);

    twr_radio_init(TWR_RADIO_MODE_NODE_SLEEPING);
    twr_radio_set_subs((twr_radio_sub_t *) subs, sizeof(subs)/sizeof(twr_radio_sub_t));
    twr_radio_set_rx_timeout_for_sleeping_node(400);
    twr_radio_pairing_request("lcd-thermostat", FW_VERSION); // Called lcd-thermostat for now
}
