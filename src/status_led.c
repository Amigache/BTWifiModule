/**
 * @file status_led.c
 * @brief On-board RGB status LED driver (RadioMaster Wireless Module only).
 *
 * The LED is a WS2812-compatible TX1812MJA5-1515 driven from GPIO18 through
 * R15 (300R). The frame is generated with the RMT peripheral: three bytes in
 * GRB order, MSB first, 0.4/0.9 us for a zero bit and 0.8/0.5 us for a one bit.
 */

#include "status_led.h"

#include "board.h"

#if defined(HAS_STATUS_LED)

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define LED_TAG "STATUS_LED"

/** RMT resolution: one tick is 0.1 us, fine enough for the WS2812 timings. */
#define LED_RESOLUTION_HZ 10000000

/** Level used for a lit channel (0-255). Kept low: the LED is very bright. */
#define LED_LEVEL 48

/** Colour selected from the link state; also used as index into led_rgb. */
typedef enum {
  LED_STATE_IDLE = 0,  //< powered, no radio and no BLE peer: red
  LED_STATE_RADIO,     //< radio UART active: green
  LED_STATE_BT,        //< BLE peer connected: blue
  LED_STATE_BOTH,      //< radio and BLE at once: cyan
} led_state_t;

/** RGB levels for each state. */
static const uint8_t led_rgb[4][3] = {
    [LED_STATE_IDLE] = {LED_LEVEL, 0, 0},
    [LED_STATE_RADIO] = {0, LED_LEVEL, 0},
    [LED_STATE_BT] = {0, 0, LED_LEVEL},
    [LED_STATE_BOTH] = {0, LED_LEVEL, LED_LEVEL},
};

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static bool s_ready;
static int s_state = -1;

/**
 * @brief Push one colour to the LED.
 *
 * @param rgb Red, green and blue levels (0-255), in that order.
 */
static void led_write_color(const uint8_t *rgb) {
  /* WS2812 expects the channels in GRB order. */
  uint8_t grb[3] = {rgb[1], rgb[0], rgb[2]};
  rmt_transmit_config_t tx_config = {.loop_count = 0};

  if (rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &tx_config) != ESP_OK) {
    return;
  }
  /* The payload above is on the stack, so wait for the transaction to end. */
  rmt_tx_wait_all_done(s_channel, 20);
}

void status_led_init(void) {
  if (s_ready) {
    return;
  }

  rmt_tx_channel_config_t channel_config = {
      .gpio_num = STATUS_LED_GPIO,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = LED_RESOLUTION_HZ,
      .mem_block_symbols = 64,
      .trans_queue_depth = 2,
  };
  if (rmt_new_tx_channel(&channel_config, &s_channel) != ESP_OK) {
    ESP_LOGE(LED_TAG, "RMT channel allocation failed");
    return;
  }

  rmt_bytes_encoder_config_t encoder_config = {
      .bit0 = {.level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 9},
      .bit1 = {.level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 5},
      .flags.msb_first = 1,
  };
  if (rmt_new_bytes_encoder(&encoder_config, &s_encoder) != ESP_OK) {
    ESP_LOGE(LED_TAG, "RMT encoder allocation failed");
    rmt_del_channel(s_channel);
    s_channel = NULL;
    return;
  }

  if (rmt_enable(s_channel) != ESP_OK) {
    ESP_LOGE(LED_TAG, "RMT channel enable failed");
    rmt_del_encoder(s_encoder);
    rmt_del_channel(s_channel);
    s_encoder = NULL;
    s_channel = NULL;
    return;
  }

  s_ready = true;
  status_led_update(false, false);
}

void status_led_update(bool radio_active, bool bt_connected) {
  if (!s_ready) {
    return;
  }

  led_state_t state;
  if (radio_active && bt_connected) {
    state = LED_STATE_BOTH;
  } else if (radio_active) {
    state = LED_STATE_RADIO;
  } else if (bt_connected) {
    state = LED_STATE_BT;
  } else {
    state = LED_STATE_IDLE;
  }

  if ((int)state == s_state) {
    return;
  }
  s_state = (int)state;
  led_write_color(led_rgb[state]);
}

#else /* boards without a status LED */

void status_led_init(void) {}

void status_led_update(bool radio_active, bool bt_connected) {
  (void)radio_active;
  (void)bt_connected;
}

#endif
