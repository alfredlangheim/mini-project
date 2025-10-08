/* pong_interrupt.c
   Pong med timer-interrupt, VGA 8-bit frame buffer och VGA DMA.
   SW0 = vänster paddel (0=ner,1=upp), SW9 = höger paddel (0=ner,1=upp)
   BTN0 = reset / continue
*/

#include <stdint.h>

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

/* --- input --- */
#define SW_ADDR  ((volatile unsigned int*)0x04000010)
#define BTN_ADDR ((volatile unsigned int*)0x040000d0)
#define LED_ADDR  ((volatile unsigned int*)0x04000000)

static inline int read_sw(void) { return (int)(*SW_ADDR & 0x3FF); }
static inline int read_btn(void){ return (int)(*BTN_ADDR & 0x1); }

/* --- Spelvariabler (globala så interrupt kan nå) --- */
static int screen_w;
static int screen_h;
static unsigned int buffer_start = VGA_BUFFER_START;
static unsigned int buffer_end = VGA_BUFFER_END;
int p1_y, p2_y;              // paddel y-koord
const int paddle_h = 40;
const int paddle_w = 5;
int ball_x, ball_y;
int ball_dx, ball_dy;

int p1_score, p2_score;
int game_over;               // 0 = spel pågår, 1 = nån vann (pausat)
int winner;                  // 1 = p1 vann, 2 = p2 vann

/* hastigheter */
const int PADDLE_STEP = 5;
const int BALL_STEP_X = 3;
const int BALL_STEP_Y = 3;

/* --- Prototyper (boot.o förväntar sig enable_interrupt extern) --- */
extern void enable_interrupt(void);
void handle_interrupt(unsigned cause); // måste finnas för länkning

/* --- Ritfunktioner --- */
//static void clear_screen(uint8_t color) {
//    for (int i = 0; i < screen_w * screen_h; ++i)
//        vga[i] = color;
//}

static void clear_screen(unsigned int buffer, char color) {
    volatile unsigned char * base = (volatile unsigned char *)buffer;
    for (int i = 0; i < screen_h; ++i) {
        volatile unsigned char* row = base + i * screen_w;
        for (int j = 0; j < screen_w; ++j) {
            row[j] = color;
        }
    }
}

static void draw_rect(unsigned int buffer, int x, int y, int w, int h, char color) {
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

/* --- Enkel 5x7-teckengrafik för textmeddelanden på vinnarskärmen --- */
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
                /* rita en liten skala*skala-fylld ruta */
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
    int len = 0; while (s[len] != '\0') ++len; /* egen strlen */
    /* 5 pixlar per tecken + 1 pix mellanslag */
    return len ? (len * (5 * scale) + (len - 1) * (1 * scale)) : 0;
}

static void draw_text(unsigned int buffer, int x, int y, const char *s, unsigned char color, int scale) {
    int cx = x;
    for (int i = 0; s[i] != '\0'; ++i) {
        draw_char5x7(buffer, cx, y, s[i], color, scale);
        cx += (5 + 1) * scale; /* teckenbredd + 1 px spacing */
    }
}

/* Rita poäng som små block högst upp */
static void draw_score(void) {
    for (int i = 0; i < p1_score; ++i) {
        draw_rect(buffer_end, 30 + i * 10, 8, 8, 8, 0xFF);
    }
    for (int i = 0; i < p2_score; ++i) {
        draw_rect(buffer_end, screen_w - 30 - i * 10 - 8, 8, 8, 8, 0xFF);
    }
}

/* Uppdatera VGA DMA för att visa aktuell buffer */
static inline void update_vga_dma(void) {
    VGA_CTRL_BACKBUFF = buffer_end; 
    VGA_CTRL_BUFFER = 0; // starta DMA

    while (VGA_CTRL_STATUS & 0x1) {
        // vänta på att DMA är klar
    }

    unsigned int temp = buffer_start;
    buffer_start = buffer_end;
    buffer_end = temp;

}

/* --- Reset / init --- */
static void reset_positions(void) {
    p1_y = screen_h/2 - paddle_h/2;
    p2_y = screen_h/2 - paddle_h/2;
    ball_x = screen_w/2 - 2;
    ball_y = screen_h/2 - 2;
    /* byt riktning efter varje poäng så spelet inte fastnar */
    ball_dx = (ball_dx == 0) ? BALL_STEP_X : -ball_dx; 
    ball_dy = BALL_STEP_Y;
}

static void reset_game(void) {
    p1_score = 0;
    p2_score = 0;
    game_over = 0;
    winner = 0;
    /* börja med bollriktning åt vänster för variation */
    ball_dx = -BALL_STEP_X;
    ball_dy = BALL_STEP_Y;
    reset_positions();
}

/* --- Interrupt handler: körs varje timer-tick --- */
void handle_interrupt(unsigned cause) {
    /* Clear timer IRQ (samma som i din lab) */
    TIMER_STATUS = 0;
    /* Tänd LED för SW0 och SW9 */
    *LED_ADDR = (1u << 0) | (1u << 9);

    /* Om spel är pausat pga vinst, visa pausad scen + vinnarmeddelande */
    if (game_over) {
        /* Visa pausad scen med paddlar/boll/poäng samt vinnarmeddelande */
        clear_screen(buffer_end, 0x00);
        draw_rect(buffer_end, 10, p1_y, paddle_w, paddle_h, 0xFF);
        draw_rect(buffer_end, screen_w - 15, p2_y, paddle_w, paddle_h, 0xFF);
        draw_rect(buffer_end, ball_x, ball_y, 5, 5, 0xAA);
        draw_score();

        const char *msg = (winner == 1) ? "PLAYER 1 WON!" : "PLAYER 2 WON!";
        int scale = 2; /* gör texten läsbar */
        int tw = text_width_px(msg, scale);
        int tx = (screen_w - tw) / 2;
        int ty = screen_h / 4; /* en bit upp på skärmen */
        draw_text(buffer_end, tx, ty, msg, 0xFF, scale);

        update_vga_dma();

        /* Tryck BTN0 för att starta om */
        if (read_btn()) {
            reset_game();
        }
        return;
    }

    /* Läs input (switchar) och styr paddlar */
    int sw = read_sw();
    /* SW9 = bit9 för Player 1 */
    if (sw & (1 << 9)) p1_y -= PADDLE_STEP; else p1_y += PADDLE_STEP;
    /* SW0 = bit0 för Player 2 */
    if (sw & 0x1) p2_y -= PADDLE_STEP; else p2_y += PADDLE_STEP;

    /* Begränsa paddlar till skärmen */
    if (p1_y < 0) p1_y = 0;
    if (p1_y > screen_h - paddle_h) p1_y = screen_h - paddle_h;
    if (p2_y < 0) p2_y = 0;
    if (p2_y > screen_h - paddle_h) p2_y = screen_h - paddle_h;

    /* Boll-logik */
    ball_x += ball_dx;
    ball_y += ball_dy;

    /* Studsa mot över/underkant */
    if (ball_y <= 0) { ball_y = 0; ball_dy = -ball_dy; }
    if (ball_y >= screen_h - 5) { ball_y = screen_h - 5; ball_dy = -ball_dy; }

    /* Kollision vänster paddel (p1 @ x=10) */
    if (ball_x <= 10 + paddle_w) {
        if ((ball_y + 5 >= p1_y) && (ball_y <= p1_y + paddle_h)) {
            ball_x = 10 + paddle_w; /* flytta utanför paddel för undvika fastkörning */
            ball_dx = -ball_dx;
        }
    }
    /* Kollision höger paddel (p2 @ x = screen_w - 15 = 305) */
    if (ball_x + 5 >= screen_w - 15) {
        if ((ball_y + 5 >= p2_y) && (ball_y <= p2_y + paddle_h)) {
            ball_x = screen_w - 15 - 5;
            ball_dx = -ball_dx;
        }
    }

    /* Poäng och återställning */
    if (ball_x < 0) {
        /* höger spelare får poäng */
        p2_score++;
        if (p2_score >= 5) {
            game_over = 1; winner = 2;
        }
        reset_positions();
    } else if (ball_x + 5> screen_w) {
        /* vänster spelare får poäng */
        p1_score++;
        if (p1_score >= 5) {
            game_over = 1; winner = 1;
        }
        reset_positions();
    }

    /* Kontroller: BTN = reset hela spelet */
    if (read_btn()) {
        reset_game();
    }

    /* Rita frame */
    clear_screen(buffer_end, 0x00);
    draw_rect(buffer_end, 10, p1_y, paddle_w, paddle_h, 0xFF);
    draw_rect(buffer_end, screen_w - 15, p2_y, paddle_w, paddle_h, 0xFF);
    draw_rect(buffer_end, ball_x, ball_y, 5, 5, 0xAA);
    draw_score();

    /* Visa frame via DMA */
    update_vga_dma();
}

/* --- Init timer & interrupts (anropa från main) --- */
void labinit_interrupts(void) {
    unsigned int period = 1200000; /* justera för tick-frekvens (smått experimentellt) */
    TIMER_PERIODL = (period & 0xFFFF);
    TIMER_PERIODH = (period >> 16) & 0xFFFF;
    /* Starta timer: enable + continuous (samma bits du använde tidigare) */
    TIMER_CTRL = (1 << 0) | (1 << 1) | (1 << 2);

    unsigned int res = VGA_CTRL_RES;

    screen_w = (int)((res & 0xFFFF));
    screen_h = (int)((res >> 16) & 0xFFFF);

    VGA_CTRL_BACKBUFF = VGA_BUFFER_START;

    /* Dubbel buffer baserad på resolution */
    unsigned int frame_bytes = (unsigned int)(screen_w * screen_h);

    buffer_end = buffer_start + frame_bytes;

    /* Prefill and present once, then clear the other buffer */
    clear_screen(buffer_end, 0x04);
    VGA_CTRL_BACKBUFF = buffer_start;

    while (VGA_CTRL_STATUS & 0x1u) {}



    reset_game();
    enable_interrupt(); /* extern från dina startfiler */
}

/* --- Main --- */
int main(void) {
    labinit_interrupts();

    /* Huvudloopen gör ingenting — allt sköts i interrupt */
    while (1) {
        
    }  
}
