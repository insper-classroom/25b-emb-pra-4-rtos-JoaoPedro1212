#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pins.h"
#include "ssd1306.h"

#define PIN_TRIG 12
#define PIN_ECHO 13
#define LED_ACTIVE_LOW 1

static ssd1306_t disp;

static volatile bool measuring = false;
static volatile bool have_result = false;
static volatile absolute_time_t t_rise, t_fall;

static inline void led_write(int r, int g, int b) {
#if LED_ACTIVE_LOW
    gpio_put(LED_PIN_R, r ? 0 : 1);
    gpio_put(LED_PIN_G, g ? 0 : 1);
    gpio_put(LED_PIN_B, b ? 0 : 1);
#else
    gpio_put(LED_PIN_R, r ? 1 : 0);
    gpio_put(LED_PIN_G, g ? 1 : 0);
    gpio_put(LED_PIN_B, b ? 1 : 0);
#endif
}
static inline void led_red(void){    led_write(1,0,0); }
static inline void led_green(void){  led_write(0,1,0); }
static inline void led_yellow(void){ led_write(1,1,0); }

static void echo_isr(uint gpio, uint32_t events) {
    if (gpio != PIN_ECHO) return;
    if (events & GPIO_IRQ_EDGE_RISE) { if (!measuring) { measuring = true; have_result = false; t_rise = get_absolute_time(); } }
    if (events & GPIO_IRQ_EDGE_FALL) { if (measuring) { t_fall = get_absolute_time(); measuring = false; have_result = true; } }
}

static void oled_init(void) {
    i2c_init(i2c1, 400000);
    gpio_set_function(2, GPIO_FUNC_I2C);
    gpio_set_function(3, GPIO_FUNC_I2C);
    gpio_pull_up(2);
    gpio_pull_up(3);
    disp.external_vcc = false;
    ssd1306_init(&disp, 128, 32, 0x3C, i2c1);
    ssd1306_clear(&disp);
    ssd1306_show(&disp);
    gpio_init(SSD1306_PIN_LITE);
    gpio_set_dir(SSD1306_PIN_LITE, GPIO_OUT);
    gpio_put(SSD1306_PIN_LITE, 0);
}

static void hw_init(void) {
    gpio_init(PIN_TRIG); gpio_set_dir(PIN_TRIG, GPIO_OUT); gpio_put(PIN_TRIG, 0);
    gpio_init(PIN_ECHO); gpio_set_dir(PIN_ECHO, GPIO_IN);
    gpio_set_irq_enabled_with_callback(PIN_ECHO, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, &echo_isr);
    gpio_init(LED_PIN_R); gpio_set_dir(LED_PIN_R, GPIO_OUT);
    gpio_init(LED_PIN_G); gpio_set_dir(LED_PIN_G, GPIO_OUT);
    gpio_init(LED_PIN_B); gpio_set_dir(LED_PIN_B, GPIO_OUT);
#if LED_ACTIVE_LOW
    gpio_put(LED_PIN_R, 1); gpio_put(LED_PIN_G, 1); gpio_put(LED_PIN_B, 1);
#else
    gpio_put(LED_PIN_R, 0); gpio_put(LED_PIN_G, 0); gpio_put(LED_PIN_B, 0);
#endif
}

static void screen_fail(void) {
    ssd1306_clear(&disp);
    ssd1306_draw_string(&disp, 0, 0, 2, "Sensor FAIL");
    ssd1306_draw_string(&disp, 0, 16, 1, "TRIG GP12  ECHO GP13");
    ssd1306_show(&disp);
    led_red();
}

static uint32_t median7(uint32_t *v) {
    uint32_t a[7]; for (int i=0;i<7;i++) a[i]=v[i];
    for (int i=0;i<7;i++){
        int m=i;
        for (int j=i+1;j<7;j++) if (a[j]<a[m]) m=j;
        uint32_t t=a[i]; a[i]=a[m]; a[m]=t;
    }
    return a[3];
}

static void make_bar(char *dst, size_t n, uint32_t cm) {
    if (!n) return;
    uint32_t maxcm = 100;
    if (cm > maxcm) cm = maxcm;
    uint32_t slots = n - 1;
    uint32_t filled = (cm * slots) / maxcm;
    for (uint32_t i = 0; i < slots; i++) dst[i] = (i < filled) ? '#' : '-';
    dst[slots] = '\0';
}

int main() {
    stdio_init_all();
    oled_init();
    hw_init();

    const uint32_t thr_on = 110, thr_off = 90;
    bool far_state = false;

    uint32_t win[7] = {0,0,0,0,0,0,0};
    int widx = 0;
    bool warm = false;

    char line[32], bar[22];

    while (true) {
        gpio_put(PIN_TRIG, 0); sleep_us(2);
        gpio_put(PIN_TRIG, 1); sleep_us(10);
        gpio_put(PIN_TRIG, 0);

        absolute_time_t t0 = get_absolute_time();
        bool ok = false; uint32_t dcm = 0;

        while (absolute_time_diff_us(t0, get_absolute_time()) < 60000) {
            if (have_result) {
                have_result = false;
                int64_t us = absolute_time_diff_us(t_rise, t_fall);
                if (us > 0) { dcm = (uint32_t)(us / 58); ok = true; }
                break;
            }
            tight_loop_contents();
        }

        if (!ok) {
            screen_fail();
            warm = false;
        } else {
            win[widx] = dcm; widx = (widx + 1) % 7;
            if (!warm) {
                for (int i=0;i<7;i++) win[i] = dcm;
                warm = true;
            }
            uint32_t med = median7(win);

            if (!far_state && med > thr_on) far_state = true;
            else if (far_state && med < thr_off) far_state = false;

            if (far_state) led_yellow(); else led_green();

            if (med > 9999) med = 9999;
            ssd1306_clear(&disp);
            snprintf(line, sizeof(line), "Dist: %lu cm", (unsigned long)med);
            ssd1306_draw_string(&disp, 0, 0, 2, line);
            make_bar(bar, sizeof(bar), med);
            ssd1306_draw_string(&disp, 0, 20, 1, bar);
            ssd1306_show(&disp);
        }

        sleep_ms(100);
    }
}