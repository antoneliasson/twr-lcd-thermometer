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

// Rotation support
static twr_lis2dh12_t lis2dh12;
static twr_lis2dh12_alarm_t alarm1;
static twr_dice_t dice;
static twr_module_lcd_rotation_t rotation = TWR_MODULE_LCD_ROTATION_0;

#if CORE_R == 2
static twr_module_lcd_rotation_t face_2_lcd_rotation_lut[7] =
{
    [TWR_DICE_FACE_2] = TWR_MODULE_LCD_ROTATION_270,
    [TWR_DICE_FACE_3] = TWR_MODULE_LCD_ROTATION_180,
    [TWR_DICE_FACE_4] = TWR_MODULE_LCD_ROTATION_0,
    [TWR_DICE_FACE_5] = TWR_MODULE_LCD_ROTATION_90
};
#else
static twr_module_lcd_rotation_t face_2_lcd_rotation_lut[7] =
{
    [TWR_DICE_FACE_2] = TWR_MODULE_LCD_ROTATION_90,
    [TWR_DICE_FACE_3] = TWR_MODULE_LCD_ROTATION_0,
    [TWR_DICE_FACE_4] = TWR_MODULE_LCD_ROTATION_180,
    [TWR_DICE_FACE_5] = TWR_MODULE_LCD_ROTATION_270
};
#endif

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

static void alarm_from_die_face(twr_lis2dh12_alarm_t *alarm, twr_dice_face_t f)
{
    alarm->x_low = false;
    alarm->y_low = false;
    alarm->z_low = false;

    switch (f) {
        case TWR_DICE_FACE_2:
        alarm->x_low = true;
        return;
        case TWR_DICE_FACE_3:
        alarm->y_low = true;
        return;
        case TWR_DICE_FACE_4:
        alarm->y_low = true;
        return;
        case TWR_DICE_FACE_5:
        alarm->x_low = true;
        return;

        case TWR_DICE_FACE_1:
        alarm->z_low = true;
        return;
        case TWR_DICE_FACE_6:
        alarm->z_low = true;
        return;
        case TWR_DICE_FACE_UNKNOWN:
        default:
        application_error(TWR_ERROR_INVALID_PARAMETER);
        return;
    }
}

static void lis2dh12_event_handler(twr_lis2dh12_t *self, twr_lis2dh12_event_t event, void *event_param)
{
    (void) event_param;

    if (event == TWR_LIS2DH12_EVENT_UPDATE)
    {
        twr_lis2dh12_result_g_t result;
        twr_dice_face_t new_face, old_face;

        old_face = twr_dice_get_face(&dice);
        twr_lis2dh12_get_result_g(self, &result);
        twr_dice_feed_vectors(&dice, result.x_axis, result.y_axis, result.z_axis);
        new_face = twr_dice_get_face(&dice);

        twr_log_debug("%s: face: %d->%d (x=%+.03f y=%+.03f z=%+.03f)", __func__, old_face, new_face, result.x_axis, result.y_axis, result.z_axis);

        if (new_face != old_face)
        {
            old_face = new_face;
            alarm_from_die_face(&alarm1, new_face);
            // Set new alarm for when the new orientation is left. This will
            // trigger an immediate second measurement and update event.
            twr_lis2dh12_set_alarm(self, &alarm1);
            twr_log_debug("%s: next alarm when leaving face %d", __func__, new_face);

            if (new_face > TWR_DICE_FACE_1 && new_face < TWR_DICE_FACE_6)
            {
                rotation = face_2_lcd_rotation_lut[new_face];
                twr_scheduler_plan_now(display_update_task);
            }
        }
    }
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

    twr_module_lcd_set_rotation(rotation);

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

    // Initialize Accelerometer. Not setting an update interval disables
    // periodic measurements. Setting an alarm triggers the first
    // measurement. We exploit that an update event is triggered when
    // each measurement is done.
    twr_dice_init(&dice, TWR_DICE_FACE_UNKNOWN);
    twr_lis2dh12_init(&lis2dh12, TWR_I2C_I2C0, 0x19);
    // Low resolution is fine -- we only need to detect the general orientation
    twr_lis2dh12_set_resolution(&lis2dh12, TWR_LIS2DH12_RESOLUTION_8BIT);
    /* The scaling calculation in twr_lis2dh12_set_alarm is only correct in 4G mode
     * so use that until fixed. */
    twr_lis2dh12_set_scale(&lis2dh12, TWR_LIS2DH12_SCALE_4G);
    twr_lis2dh12_set_event_handler(&lis2dh12, lis2dh12_event_handler, NULL);
    alarm1.threshold = 0.5;
    twr_lis2dh12_set_alarm(&lis2dh12, &alarm1);

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
