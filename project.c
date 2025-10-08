#include <stdint.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define VGA_BASE      0x08000000u
#define VGA_CTRL_BASE 0x04000100u
#define VGA_CTRL_BACKBUFF 0x04000104u
#define VGA_CTRL_RES 0x04000108u
#define VGA_CTRL_STATUS 0x0400010Cu

volatile uint8_t *vga = (volatile uint8_t*)VGA_BASE;
volatile uint32_t *vga_ctrl = (volatile uint32_t*)VGA_CTRL_BASE;

typedef struct {
    int x, y, w, h;
    uint8_t color;
} Paddle;

typedef struct {
    int x, y, dx, dy;
    uint8_t color;
} Ball;

Paddle paddle1 = {10, 100, 5, 40, 0xFF};
Paddle paddle2 = {305, 100, 5, 40, 0xFF};
Ball ball = {160, 120, 2, 2, 0xAA};

void clear_screen(uint8_t color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++)
        vga[i] = color;
}

void draw_rect(int x, int y, int w, int h, uint8_t color) {
    for (int j = 0; j < h; j++) {
        if (y + j < 0 || y + j >= SCREEN_HEIGHT) continue;
        for (int i = 0; i < w; i++) {
            if (x + i < 0 || x + i >= SCREEN_WIDTH) continue;
            vga[(y + j) * SCREEN_WIDTH + (x + i)] = color;
        }
    }
}

void update_vga_dma() {
    vga_ctrl[1] = (unsigned int)vga;
    vga_ctrl[0] = 0;
}

void draw_scene() {
    clear_screen(0x00);
    draw_rect(paddle1.x, paddle1.y, paddle1.w, paddle1.h, paddle1.color);
    draw_rect(paddle2.x, paddle2.y, paddle2.w, paddle2.h, paddle2.color);
    draw_rect(ball.x, ball.y, 5, 5, ball.color);
    update_vga_dma();
}

int get_sw() {
    volatile int *sw = (volatile int *)0x04000010;
    return *sw & 0x3FF;
}

int get_btn() {
    volatile int *btn = (volatile int *)0x040000d0;
    return *btn & 1;
}

void handle_interrupt(unsigned cause) {
    // Vi använder inte interrupts i det här programmet
}

void reset_game() {
    paddle1.y = 100;
    paddle2.y = 100;
    ball.x = 160;
    ball.y = 120;
    ball.dx = 2;
    ball.dy = 2;
}

int main(void) {
    reset_game();

    while (1) {
        int sw = get_sw();
        int btn = get_btn();

        // SW0 (LSB) -> paddle1 down
        if ((sw & 1) == 0) paddle1.y += 5;
        else paddle1.y -= 5;

        // SW9 (MSB) -> paddle2 down
        if ((sw & (1 << 9)) == 0) paddle2.y += 5;
        else paddle2.y -= 5;

        // Begränsa paddlar
        if (paddle1.y < 0) paddle1.y = 0;
        if (paddle1.y > SCREEN_HEIGHT - paddle1.h) paddle1.y = SCREEN_HEIGHT - paddle1.h;
        if (paddle2.y < 0) paddle2.y = 0;
        if (paddle2.y > SCREEN_HEIGHT - paddle2.h) paddle2.y = SCREEN_HEIGHT - paddle2.h;

        // Bollrörelse
        ball.x += ball.dx;
        ball.y += ball.dy;

        if (ball.y <= 0 || ball.y >= SCREEN_HEIGHT - 5) ball.dy = -ball.dy;
        if (ball.x <= paddle1.x + paddle1.w && ball.y + 5 >= paddle1.y && ball.y <= paddle1.y + paddle1.h)
            ball.dx = -ball.dx;
        if (ball.x + 5 >= paddle2.x && ball.y + 5 >= paddle2.y && ball.y <= paddle2.y + paddle2.h)
            ball.dx = -ball.dx;
        if (ball.x < 0 || ball.x > SCREEN_WIDTH) reset_game();

        // Reset-knapp
        if (btn) reset_game();

        draw_scene();

        for (volatile int i = 0; i < 20000; i++); // enkel delay
    }
}