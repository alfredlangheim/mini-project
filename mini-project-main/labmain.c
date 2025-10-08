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
const int BALL_STEP_X = 1;
const int BALL_STEP_Y = 1;

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

    /* Om spel är pausat pga vinst, kolla BTN för att starta om */
    if (game_over) {
        if (read_btn()) {
            reset_game();
        }
        /* visa vinnarskärm (blinkande färg) */
        clear_screen(buffer_end, (winner == 1) ? 0x22 : 0x44);
        update_vga_dma();
        return;
    }

    /* Läs input (switchar) och styr paddlar */
    int sw = read_sw();
    /* SW0 = bit0, 1 = upp ; 0 = ner */
    if (sw & 0x1) p1_y -= PADDLE_STEP; else p1_y += PADDLE_STEP;
    /* SW9 = bit9 */
    if (sw & (1 << 9)) p2_y -= PADDLE_STEP; else p2_y += PADDLE_STEP;

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
