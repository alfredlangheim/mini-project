/* pong_interrupt_refactored.c
   Pong med timer-interrupt, VGA 8-bit frame buffer och VGA DMA.
   SW0 = vänster paddel (0=ner,1=upp), SW9 = höger paddel (0=ner,1=upp)
   BTN0 = reset / continue
*/

/* --- VGA / frame buffer --- */
#define VGA_BASE      0x08000000u

#define VGA_CTRL_BASE 0x04000100u
#define VGA_CTRL_BUFFER (*(volatile unsigned int *)(VGA_CTRL_BASE + 0x0))
#define VGA_CTRL_BACKBUFF (*(volatile unsigned int *)(VGA_CTRL_BASE + 0x4))
#define VGA_CTRL_RES (*(volatile unsigned int *)(VGA_CTRL_BASE + 0x8))
#define VGA_CTRL_STATUS (*(volatile unsigned int *)(VGA_CTRL_BASE + 0xC))

#define VGA_BUFFER_START 0x08000000u
#define VGA_BUFFER_END 0x08025800u

/* --- Timer --- */
#define TIMER_BASE   0x04000020
#define TIMER_STATUS (*(volatile unsigned int *)(TIMER_BASE + 0x0))
#define TIMER_CTRL   (*(volatile unsigned int *)(TIMER_BASE + 0x4))
#define TIMER_PERIODL (*(volatile unsigned int *)(TIMER_BASE + 0x8))
#define TIMER_PERIODH (*(volatile unsigned int *)(TIMER_BASE + 0xC))

/* --- Input / LED --- */
#define SW_ADDR  ((volatile unsigned int*)0x04000010)
#define BTN_ADDR ((volatile unsigned int*)0x040000d0)
#define LED_ADDR  ((volatile unsigned int*)0x04000000)

static inline int read_sw(void) { return (int)(*SW_ADDR & 0x3FF); }
static inline int read_btn(void){ return (int)(*BTN_ADDR & 0x1); }

/* --- Spelvariabler --- */
static int screen_w;
static int screen_h;
static unsigned int buffer_start = VGA_BUFFER_START;
static unsigned int buffer_end = VGA_BUFFER_END;

int p1_y, p2_y;
const int paddle_h = 40;
const int paddle_w = 5;
int ball_x, ball_y;
int ball_dx, ball_dy;

int p1_score, p2_score;
int game_over;
int winner;

/* hastigheter */
const int PADDLE_STEP = 5;
const int BALL_STEP_X = 3;
const int BALL_STEP_Y = 3;

/* --- Prototyper --- */
extern void enable_interrupt(void);
void handle_interrupt(unsigned cause);

/* --- Ritfunktioner --- */
static void clear_screen(unsigned int buffer, unsigned char color) {
    volatile unsigned char * base = (volatile unsigned char *)buffer;
    for (int i = 0; i < screen_h; ++i) {
        volatile unsigned char* row = base + i * screen_w;
        for (int j = 0; j < screen_w; ++j) {
            row[j] = color;
        }
    }
}

static void draw_rect(unsigned int buffer, int x, int y, int w, int h, unsigned char color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x >= screen_w || y >= screen_h) return;
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;

    volatile unsigned char * base = (volatile unsigned char *)buffer;
    for (int i = 0; i < h; ++i) {
        volatile unsigned char * row = base + (y + i) * screen_w + x;
        for (int j = 0; j < w; ++j) {
            row[j] = color;
        }
    }
}

/* 5x7 teckengrafik */
static const unsigned char FONT5x7[128][7] = {
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['N'] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['Y'] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    ['!'] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00}
};

static inline void draw_char5x7(unsigned int buffer, int x, int y, char c, unsigned char color, int scale) {
    if (c < 0 || c > 127) return;
    const unsigned char *rows = FONT5x7[(int)c];
    volatile unsigned char *base = (volatile unsigned char *)buffer;
    for (int r = 0; r < 7; ++r) {
        unsigned char row = rows[r];
        for (int col = 0; col < 5; ++col) {
            if (row & (1 << (4 - col))) {
                int px = x + col * scale;
                int py = y + r * scale;
                for (int dy = 0; dy < scale; ++dy) {
                    int ry = py + dy;
                    if (ry < 0 || ry >= screen_h) continue;
                    volatile unsigned char *dst = base + ry * screen_w;
                    for (int dx = 0; dx < scale; ++dx) {
                        int rx = px + dx;
                        if (rx < 0 || rx >= screen_w) continue;
                        dst[rx] = color;
                    }
                }
            }
        }
    }
}

static inline int text_width_px(const char *s, int scale) {
    int len = 0; while (s[len] != '\0') ++len;
    return len ? (len * (5 * scale) + (len - 1) * (1 * scale)) : 0;
}

static void draw_text(unsigned int buffer, int x, int y, const char *s, unsigned char color, int scale) {
    int cx = x;
    for (int i = 0; s[i] != '\0'; ++i) {
        draw_char5x7(buffer, cx, y, s[i], color, scale);
        cx += (5 + 1) * scale;
    }
}

/* Rita poäng */
static void draw_score(void) {
    for (int i = 0; i < p1_score; ++i) draw_rect(buffer_end, 30 + i*10, 8, 8, 8, 0xFF);
    for (int i = 0; i < p2_score; ++i) draw_rect(buffer_end, screen_w - 30 - i*10 - 8, 8, 8, 8, 0xFF);
}

/* Uppdatera DMA */
static inline void update_vga_dma(void) {
    VGA_CTRL_BACKBUFF = buffer_end;
    VGA_CTRL_BUFFER = 0;

    while (VGA_CTRL_STATUS & 0x1);

    unsigned int temp = buffer_start;
    buffer_start = buffer_end;
    buffer_end = temp;
}

/* --- Reset --- */
static void reset_positions(void) {
    p1_y = screen_h/2 - paddle_h/2;
    p2_y = screen_h/2 - paddle_h/2;
    ball_x = screen_w/2 - 2;
    ball_y = screen_h/2 - 2;
    ball_dx = (ball_dx == 0) ? BALL_STEP_X : -ball_dx;
    ball_dy = BALL_STEP_Y;
}

static void reset_game(void) {
    p1_score = 0;
    p2_score = 0;
    game_over = 0;
    winner = 0;
    ball_dx = -BALL_STEP_X;
    ball_dy = BALL_STEP_Y;
    reset_positions();
}

/* --- Update paddlar --- */
static void update_paddles(void) {
    int sw = read_sw();
    if (sw & (1 << 9)) p1_y -= PADDLE_STEP; else p1_y += PADDLE_STEP;
    if (sw & 0x1) p2_y -= PADDLE_STEP; else p2_y += PADDLE_STEP;

    if (p1_y < 0) p1_y = 0;
    if (p1_y > screen_h - paddle_h) p1_y = screen_h - paddle_h;
    if (p2_y < 0) p2_y = 0;
    if (p2_y > screen_h - paddle_h) p2_y = screen_h - paddle_h;
}

/* --- Update boll --- */
static void update_ball(void) {
    ball_x += ball_dx;
    ball_y += ball_dy;

    if (ball_y <= 0) { ball_y = 0; ball_dy = -ball_dy; }
    if (ball_y >= screen_h - 5) { ball_y = screen_h - 5; ball_dy = -ball_dy; }

    if (ball_x <= 10 + paddle_w) {
        if ((ball_y + 5 >= p1_y) && (ball_y <= p1_y + paddle_h)) {
            ball_x = 10 + paddle_w;
            ball_dx = -ball_dx;
        }
    }
    if (ball_x + 5 >= screen_w - 15) {
        if ((ball_y + 5 >= p2_y) && (ball_y <= p2_y + paddle_h)) {
            ball_x = screen_w - 15 - 5;
            ball_dx = -ball_dx;
        }
    }

    if (ball_x < 0) {
        p2_score++;
        if (p2_score >= 5) { game_over = 1; winner = 2; }
        reset_positions();
    } else if (ball_x + 5 > screen_w) {
        p1_score++;
        if (p1_score >= 5) { game_over = 1; winner = 1; }
        reset_positions();
    }
}

/* --- Render frame --- */
static void render_frame(void) {
    clear_screen(buffer_end, 0x00);

    draw_rect(buffer_end, 10, p1_y, paddle_w, paddle_h, 0xFF);
    draw_rect(buffer_end, screen_w - 15, p2_y, paddle_w, paddle_h, 0xFF);
    draw_rect(buffer_end, ball_x, ball_y, 5, 5, 0xAA);

    draw_score();

    if (game_over) {
        const char *msg = (winner == 1) ? "PLAYER 1 WON!" : "PLAYER 2 WON!";
        int scale = 2;
        int tw = text_width_px(msg, scale);
        int tx = (screen_w - tw)/2;
        int ty = screen_h/4;
        draw_text(buffer_end, tx, ty, msg, 0xFF, scale);
    }

    update_vga_dma();
}

/* --- Interrupt handler --- */
void handle_interrupt(unsigned cause) {
    TIMER_STATUS = 0;
    *LED_ADDR = (1u << 0) | (1u << 9);

    if (read_btn()) reset_game();

    if (!game_over) {
        update_paddles();
        update_ball();
    }

    render_frame();
}

/* --- Init --- */
void labinit_interrupts(void) {
    unsigned int period = 1200000;
    TIMER_PERIODL = (period & 0xFFFF);
    TIMER_PERIODH = (period >> 16) & 0xFFFF;

    TIMER_CTRL = (1 << 0) | (1 << 1) | (1 << 2);

    unsigned int res = VGA_CTRL_RES;
    screen_w = (int)(res & 0xFFFF);
    screen_h = (int)((res >> 16) & 0xFFFF);

    VGA_CTRL_BACKBUFF = VGA_BUFFER_START;
    unsigned int frame_bytes = (unsigned int)(screen_w * screen_h);
    buffer_end = buffer_start + frame_bytes;

    clear_screen(buffer_end, 0x04);
    VGA_CTRL_BACKBUFF = buffer_start;

    while (VGA_CTRL_STATUS & 0x1u);

    reset_game();
    enable_interrupt();
}

/* --- Main --- */
int main(void) {
    labinit_interrupts();
    while (1) {}
}
