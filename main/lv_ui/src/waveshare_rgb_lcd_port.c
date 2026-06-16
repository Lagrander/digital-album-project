/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "waveshare_rgb_lcd_port.h"
#include "i2c_bus.h"

static const char *TAG = "waveshare_rgb_lcd";
static bool i2c_driver_ready = false;

// Ensure I2C driver is installed for backlight control
static void ensure_i2c_ready(void) {
  if (i2c_driver_ready)
    return;
  i2c_config_t i2c_conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = I2C_MASTER_SDA_IO,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_io_num = I2C_MASTER_SCL_IO,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = I2C_MASTER_FREQ_HZ,
  };
  i2c_bus_handle_t bus = i2c_bus_create(I2C_MASTER_NUM, &i2c_conf);
  if (bus == NULL) {
    ESP_LOGE(TAG, "Failed to create I2C bus via i2c_bus_create");
    return;
  }

  /* 上电极早期安装驱动后，给总线电容和外接上拉电阻 5ms
   * 物理上升时间，确保通信前总线电平处于高电平稳定状态 */
  esp_rom_delay_us(5000);

  i2c_driver_ready = true;
}

// VSYNC event callback function
IRAM_ATTR static bool
rgb_lcd_on_vsync_event(esp_lcd_panel_handle_t panel,
                       const esp_lcd_rgb_panel_event_data_t *edata,
                       void *user_ctx) {
  return lvgl_port_notify_rgb_vsync();
}

// Initialize RGB LCD
esp_err_t waveshare_esp32_s3_rgb_lcd_init() {
  wavesahre_rgb_lcd_bl_on(); /* 上电立即点亮背光（维持芯片原生白屏状态），实现无缝视觉过渡
                              */
  ESP_LOGI(TAG, "Install RGB LCD panel driver"); // Log the start of the RGB LCD
                                                 // panel driver installation
  esp_lcd_panel_handle_t panel_handle =
      NULL; // Declare a handle for the LCD panel
  esp_lcd_rgb_panel_config_t panel_config = {
      .clk_src = LCD_CLK_SRC_DEFAULT, // Set the clock source for the panel
      .timings =
          {
              .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ, // Pixel clock frequency
              .h_res = EXAMPLE_LCD_H_RES,            // Horizontal resolution
              .v_res = EXAMPLE_LCD_V_RES,            // Vertical resolution
              .hsync_pulse_width = 4, // Horizontal sync pulse width
              .hsync_back_porch = 8,  // Horizontal back porch
              .hsync_front_porch = 8, // Horizontal front porch
              .vsync_pulse_width = 4, // Vertical sync pulse width
              .vsync_back_porch = 8,  // Vertical back porch
              .vsync_front_porch = 8, // Vertical front porch
              .flags =
                  {
                      .pclk_active_neg = 1, // Active low pixel clock
                  },
          },
      .data_width = EXAMPLE_RGB_DATA_WIDTH,     // Data width for RGB
      .in_color_format = LCD_COLOR_FMT_RGB565,  // Input color format: RGB565
      .out_color_format = LCD_COLOR_FMT_RGB565, // Output color format: RGB565
      .num_fbs = LVGL_PORT_LCD_RGB_BUFFER_NUMS, // Number of frame buffers
      .bounce_buffer_size_px =
          EXAMPLE_RGB_BOUNCE_BUFFER_SIZE, // Bounce buffer size in pixels
      .dma_burst_size = 64,               // DMA burst size in bytes
      .hsync_gpio_num =
          EXAMPLE_LCD_IO_RGB_HSYNC, // GPIO number for horizontal sync
      .vsync_gpio_num =
          EXAMPLE_LCD_IO_RGB_VSYNC,             // GPIO number for vertical sync
      .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,     // GPIO number for data enable
      .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK, // GPIO number for pixel clock
      .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP, // GPIO number for display
      .data_gpio_nums =
          {
              EXAMPLE_LCD_IO_RGB_DATA0,
              EXAMPLE_LCD_IO_RGB_DATA1,
              EXAMPLE_LCD_IO_RGB_DATA2,
              EXAMPLE_LCD_IO_RGB_DATA3,
              EXAMPLE_LCD_IO_RGB_DATA4,
              EXAMPLE_LCD_IO_RGB_DATA5,
              EXAMPLE_LCD_IO_RGB_DATA6,
              EXAMPLE_LCD_IO_RGB_DATA7,
              EXAMPLE_LCD_IO_RGB_DATA8,
              EXAMPLE_LCD_IO_RGB_DATA9,
              EXAMPLE_LCD_IO_RGB_DATA10,
              EXAMPLE_LCD_IO_RGB_DATA11,
              EXAMPLE_LCD_IO_RGB_DATA12,
              EXAMPLE_LCD_IO_RGB_DATA13,
              EXAMPLE_LCD_IO_RGB_DATA14,
              EXAMPLE_LCD_IO_RGB_DATA15,
          },
      .flags =
          {
              .fb_in_psram = 1, // Use PSRAM for framebuffer
          },
  };

  // Create a new RGB panel with the specified configuration
  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

  ESP_LOGI(TAG, "Initialize RGB LCD panel"); // Log the initialization of the
                                             // RGB LCD panel
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle)); // Initialize the LCD panel

  // /* TEST */
  // ESP_LOGI("TEST", "Testing RGB panel with solid color...");
  // // 分配一个全屏大小的buffer（例如 800x480 的 RGB565）
  // size_t buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2; //
  // 2字节/像素 void *solid_color_buf = heap_caps_malloc(buffer_size,
  // MALLOC_CAP_PSRAM); if (solid_color_buf) {
  //     // 填充红色 (0xF800)
  //     for (int i = 0; i < (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES); i++) {
  //         ((uint16_t*)solid_color_buf)[i] = 0xF800;
  //     }
  //     // 绘制全屏红色
  //     esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, EXAMPLE_LCD_H_RES,
  //     EXAMPLE_LCD_V_RES, solid_color_buf); vTaskDelay(pdMS_TO_TICKS(2000));
  //     free(solid_color_buf);
  // }

  // /* Test */
  ESP_ERROR_CHECK(lvgl_port_init(
      panel_handle)); // Initialize LVGL with the panel handle only

  // Register callbacks for RGB panel events
  esp_lcd_rgb_panel_event_callbacks_t cbs = {
      .on_vsync = rgb_lcd_on_vsync_event, // Callback for vertical sync
  };
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(
      panel_handle, &cbs, NULL)); // Register event callbacks

  return ESP_OK; // Return success
}

/******************************* Turn on the screen backlight
 * **************************************/
esp_err_t wavesahre_rgb_lcd_bl_on() {
  ensure_i2c_ready();
  // Configure CH422G to output mode
  uint8_t write_buf = 0x01;
  esp_err_t ret1 = ESP_FAIL;
  esp_err_t ret2 = ESP_FAIL;
  int retry = 0;

  // 自适应重试点亮，确保指令必然送达
  while ((ret1 != ESP_OK || ret2 != ESP_OK) && retry < 10) {
    write_buf = 0x01;
    ret1 = i2c_master_write_to_device(I2C_MASTER_NUM, 0x24, &write_buf, 1,
                                      pdMS_TO_TICKS(10));

    // Pull the backlight pin high to light the screen backlight
    write_buf = 0x1E;
    ret2 = i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
                                      pdMS_TO_TICKS(10));
    if (ret1 != ESP_OK || ret2 != ESP_OK) {
      esp_rom_delay_us(2000); // 抖动发生时延迟 2ms 重试
      retry++;
    }
  }
  return (ret1 == ESP_OK && ret2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/******************************* Turn off the screen backlight
 * **************************************/
esp_err_t wavesahre_rgb_lcd_bl_off() {
  ensure_i2c_ready();
  // Configure CH422G to output mode
  uint8_t write_buf = 0x01;
  esp_err_t ret1 = ESP_FAIL;
  esp_err_t ret2 = ESP_FAIL;
  int retry = 0;

  // 自适应重试关断，确保开机第一微秒哪怕遭遇硬件电平不稳，也能在几毫秒内迅速捕获并强制切断背光！
  while ((ret1 != ESP_OK || ret2 != ESP_OK) && retry < 10) {
    write_buf = 0x01;
    ret1 = i2c_master_write_to_device(I2C_MASTER_NUM, 0x24, &write_buf, 1,
                                      pdMS_TO_TICKS(10));

    // Turn off the screen backlight by pulling the backlight pin low
    write_buf = 0x1A;
    ret2 = i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
                                      pdMS_TO_TICKS(10));
    if (ret1 != ESP_OK || ret2 != ESP_OK) {
      esp_rom_delay_us(2000); // 抖动发生时延迟 2ms 重试
      retry++;
    }
  }

  if (ret1 == ESP_OK && ret2 == ESP_OK) {
    ESP_LOGI(TAG, "Backlight turned off successfully via I2C (retries: %d)",
             retry);
  } else {
    ESP_LOGE(TAG, "Failed to turn off backlight via I2C (ret1: %d, ret2: %d)",
             ret1, ret2);
  }
  return (ret1 == ESP_OK && ret2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

/* ── 软件亮度控制（CH422G 无硬件 PWM，0=关 >0=开）─────────────── */

static uint8_t g_bl_brightness = 100;

void waveshare_rgb_lcd_bl_set_brightness(uint8_t pct) {
  g_bl_brightness = (pct > 100) ? 100 : pct;
  if (g_bl_brightness == 0) {
    wavesahre_rgb_lcd_bl_off();
  } else {
    wavesahre_rgb_lcd_bl_on();
  }
}

uint8_t waveshare_rgb_lcd_bl_get_brightness(void) {
  return g_bl_brightness;
}

/******************************* Example code
 * **************************************/
static void draw_event_cb(lv_event_t *e) // Draw event callback function
{
  lv_obj_draw_part_dsc_t *dsc =
      lv_event_get_draw_part_dsc(e); // Get the draw part descriptor
  if (dsc->part == LV_PART_ITEMS) {  // If drawing chart items
    lv_obj_t *obj =
        lv_event_get_target(e); // Get the target object of the event
    lv_chart_series_t *ser =
        lv_chart_get_series_next(obj, NULL); // Get the series of the chart
    uint32_t cnt =
        lv_chart_get_point_count(obj); // Get the number of points in the chart
    /* Make older values more transparent */
    dsc->rect_dsc->bg_opa =
        (LV_OPA_COVER * dsc->id) / (cnt - 1); // Set opacity based on the index

    /* Make smaller values blue, higher values red  */
    lv_coord_t *x_array =
        lv_chart_get_x_array(obj, ser); // Get the X-axis array
    lv_coord_t *y_array =
        lv_chart_get_y_array(obj, ser); // Get the Y-axis array
    /* dsc->id is the drawing order, but we need the index of the point being
     * drawn dsc->id  */
    uint32_t start_point = lv_chart_get_x_start_point(
        obj, ser); // Get the start point of the chart
    uint32_t p_act = (start_point + dsc->id) %
                     cnt; // Calculate the actual index based on the start point
    lv_opa_t x_opa =
        (x_array[p_act] * LV_OPA_50) / 200; // Calculate X-axis opacity
    lv_opa_t y_opa =
        (y_array[p_act] * LV_OPA_50) / 1000; // Calculate Y-axis opacity

    dsc->rect_dsc->bg_color =
        lv_color_mix(lv_palette_main(LV_PALETTE_RED), // Mix colors
                     lv_palette_main(LV_PALETTE_BLUE), x_opa + y_opa);
  }
}

static void
add_data(lv_timer_t *timer) // Timer callback to add data to the chart
{
  lv_obj_t *chart = timer->user_data; // Get the chart associated with the timer
  lv_chart_set_next_value2(chart, lv_chart_get_series_next(chart, NULL),
                           lv_rand(0, 200),
                           lv_rand(0, 1000)); // Add random data to the chart
}

// This demo UI is adapted from LVGL official example:
// https://docs.lvgl.io/master/examples.html#scatter-chart
void example_lvgl_demo_ui() // LVGL demo UI initialization function
{
  lv_obj_t *scr = lv_scr_act();               // Get the current active screen
  lv_obj_t *chart = lv_chart_create(scr);     // Create a chart object
  lv_obj_set_size(chart, 200, 150);           // Set chart size
  lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0); // Center the chart on the screen
  lv_obj_add_event_cb(chart, draw_event_cb, LV_EVENT_DRAW_PART_BEGIN,
                      NULL); // Add draw event callback
  lv_obj_set_style_line_width(chart, 0, LV_PART_ITEMS); /* Remove chart lines */

  lv_chart_set_type(chart, LV_CHART_TYPE_SCATTER); // Set chart type to scatter

  lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 5, 5, 5, 1, true,
                         30); // Set X-axis ticks
  lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 10, 5, 6, 5, true,
                         50); // Set Y-axis ticks

  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_X, 0,
                     200); // Set X-axis range
  lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                     1000); // Set Y-axis range

  lv_chart_set_point_count(chart, 50); // Set the number of points in the chart

  lv_chart_series_t *ser =
      lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_RED),
                          LV_CHART_AXIS_PRIMARY_Y); // Add a series to the chart
  for (int i = 0; i < 50; i++) { // Add random points to the chart
    lv_chart_set_next_value2(chart, ser, lv_rand(0, 200),
                             lv_rand(0, 1000)); // Set X and Y values
  }

  lv_timer_create(add_data, 100,
                  chart); // Create a timer to add new data every 100ms
}
