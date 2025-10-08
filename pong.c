// Pong for VGA or SDL2 fallback
// Compile for SDL2 (desktop testing):
//   gcc -DUSE_SDL -O2 pong.c -o pong `sdl2-config --cflags --libs`
// Compile for baremetal/framebuffer (RGB565 at FRAMEBUFFER_BASE):
//   gcc -O2 pong.c -o pong
//   (Define FRAMEBUFFER_BASE appropriately for your board.)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------- Display config ----------------------
#ifndef VGA_WIDTH
#define VGA_WIDTH  640
#endif
#ifndef VGA_HEIGHT
#define VGA_HEIGHT 480
#endif

// Choose pixel format depending on backend
#ifdef USE_SDL
  typedef uint32_t pixel_t; // ARGB8888 for SDL texture
#else
  typedef uint16_t pixel_t; // RGB565 for memory-mapped VGA
#endif

// ---------------------- Backend glue ------------------------
#ifdef USE_SDL
#include <SDL2/SDL.h>
static SDL_Window*   g_win  = NULL;
static SDL_Renderer* g_ren  = NULL;
static SDL_Texture*  g_tex  = NULL; // ARGB8888 streaming texture
static pixel_t*      g_back = NULL; // backbuffer (VGA_WIDTH * VGA_HEIGHT)

static int backend_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return -1;
    }
    g_win = SDL_CreateWindow("Pong (VGA emu)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              VGA_WIDTH, VGA_HEIGHT, SDL_WINDOW_SHOWN);
    if (!g_win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return -1; }
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                              VGA_WIDTH, VGA_HEIGHT);
    if (!g_tex) { fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError()); return -1; }
    g_back = (pixel_t*)malloc(sizeof(pixel_t) * VGA_WIDTH * VGA_HEIGHT);
    if (!g_back) { fprintf(stderr, "malloc backbuffer failed\n"); return -1; }
    return 0;
}

static void backend_present(void) {
    SDL_UpdateTexture(g_tex, NULL, g_back, VGA_WIDTH * sizeof(pixel_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

static void backend_shutdown(void) {
    if (g_back) free(g_back);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

static inline void put_pixel(int x, int y, pixel_t c) {
    if ((unsigned)x >= (unsigned)VGA_WIDTH || (unsigned)y >= (unsigned)VGA_HEIGHT) return;
    g_back[y * VGA_WIDTH + x] = c;
}

static inline pixel_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    // ARGB8888, no alpha blending
    return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void backend_delay_ms(uint32_t ms) { SDL_Delay(ms); }

#else
// Bare-metal / memory-mapped RGB565 framebuffer
#ifndef FRAMEBUFFER_BASE
#define FRAMEBUFFER_BASE 0x08000000u // <-- Adjust for your board
#endif
static volatile pixel_t* const g_fb = (volatile pixel_t*)FRAMEBUFFER_BASE;

static int backend_init(void) { return 0; }
static void backend_present(void) { /* no-op: we draw directly to FB */ }
static void backend_shutdown(void) { /* no-op */ }

static inline void put_pixel(int x, int y, pixel_t c) {
    if ((unsigned)x >= (unsigned)VGA_WIDTH || (unsigned)y >= (unsigned)VGA_HEIGHT) return;
    g_fb[y * VGA_WIDTH + x] = c;
}

static inline pixel_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (pixel_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline pixel_t rgb(uint8_t r, uint8_t g, uint8_t b) { return rgb565(r,g,b); }

// Crude delay loop (replace with timer if available)
static void backend_delay_ms(uint32_t ms) {
    volatile uint32_t i;
    for (i = 0; i < ms * 16000u; ++i) { __asm__ __volatile__("nop"); }
}
#endif

// ---------------------- Drawing Primitives ------------------
static inline void clear_screen(pixel_t color) {
#ifdef USE_SDL
    size_t N = (size_t)VGA_WIDTH * VGA_HEIGHT;
    for (size_t i = 0; i < N; ++i) g_back[i] = color;
#else
    // Direct write to framebuffer
    for (int y = 0; y < VGA_HEIGHT; ++y) {
        for (int x = 0; x < VGA_WIDTH; ++x) {
            put_pixel(x, y, color);
        }
    }
#endif
}

static inline void fill_rect(int x, int y, int w, int h, pixel_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > VGA_WIDTH) x1 = VGA_WIDTH; if (y1 > VGA_HEIGHT) y1 = VGA_HEIGHT;
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            put_pixel(xx, yy, color);
        }
    }
}

// Minimal 5x7 font for uppercase letters, digits, and a few symbols
// Each glyph is 5x7, stored in 7 rows of 5 bits (LSB left)
typedef struct { char ch; uint8_t rows[7]; } Glyph;
#define GLY(ch,a,b,c,d,e,f,g) { ch, {a,b,c,d,e,f,g} }
static const Glyph kFont[] = {
    // Digits 0-9
    GLY('0',0x1E,0x11,0x13,0x15,0x19,0x11,0x1E),
    GLY('1',0x04,0x0C,0x14,0x04,0x04,0x04,0x1F),
    GLY('2',0x1E,0x01,0x01,0x1E,0x10,0x10,0x1F),
    GLY('3',0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E),
    GLY('4',0x02,0x06,0x0A,0x12,0x1F,0x02,0x02),
    GLY('5',0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E),
    GLY('6',0x0E,0x10,0x10,0x1E,0x11,0x11,0x1E),
    GLY('7',0x1F,0x01,0x02,0x04,0x08,0x08,0x08),
    GLY('8',0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E),
    GLY('9',0x1E,0x11,0x11,0x1F,0x01,0x01,0x0E),
    // Uppercase A-Z (subset used in messages)
    GLY('A',0x0E,0x11,0x11,0x1F,0x11,0x11,0x11),
    GLY('E',0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F),
    GLY('I',0x1F,0x04,0x04,0x04,0x04,0x04,0x1F),
    GLY('N',0x11,0x19,0x15,0x13,0x11,0x11,0x11),
    GLY('O',0x0E,0x11,0x11,0x11,0x11,0x11,0x0E),
    GLY('P',0x1E,0x11,0x11,0x1E,0x10,0x10,0x10),
    GLY('R',0x1E,0x11,0x11,0x1E,0x14,0x12,0x11),
    GLY('S',0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E),
    GLY('T',0x1F,0x04,0x04,0x04,0x04,0x04,0x04),
    GLY('V',0x11,0x11,0x11,0x11,0x0A,0x0A,0x04),
    GLY('W',0x11,0x11,0x11,0x15,0x15,0x1F,0x11),
    GLY('Y',0x11,0x11,0x0A,0x04,0x04,0x04,0x04),
    // Space and symbols
    GLY(' ',0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    GLY(':',0x00,0x04,0x00,0x00,0x00,0x04,0x00),
};
static const size_t kFontCount = sizeof(kFont)/sizeof(kFont[0]);

static const Glyph* find_glyph(char c) {
    for (size_t i=0;i<kFontCount;++i) if (kFont[i].ch==c) return &kFont[i];
    return NULL;
}

static void draw_char(int x, int y, char c, int scale, pixel_t col) {
    const Glyph* g = find_glyph(c);
    if (!g) return; // skip unknown
    for (int row=0; row<7; ++row) {
        uint8_t bits = g->rows[row];
        for (int colb=0; colb<5; ++colb) {
            if (bits & (1 << (4-colb))) {
                fill_rect(x + colb*scale, y + row*scale, scale, scale, col);
            }
        }
    }
}

static void draw_text(int x, int y, const char* s, int scale, pixel_t col) {
    int cx = x;
    for (const char* p=s; *p; ++p) {
        draw_char(cx, y, *p, scale, col);
        cx += 6 * scale; // 5px + 1px spacing
    }
}

// ---------------------- Game Logic --------------------------

typedef struct { float x,y,w,h; } Rect;

typedef struct {
    float x,y;   // center position
    float vx,vy; // velocity in pixels per second
    float r;     // radius
} Ball;

typedef struct { int up1,down1,up2,down2,reset; } Input;

static void read_input(Input* in) {
    memset(in, 0, sizeof(*in));
#ifdef USE_SDL
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) exit(0);
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) exit(0);
    }
    const uint8_t* ks = SDL_GetKeyboardState(NULL);
    in->up1   = ks[SDL_SCANCODE_W];
    in->down1 = ks[SDL_SCANCODE_S];
    in->up2   = ks[SDL_SCANCODE_UP];
    in->down2 = ks[SDL_SCANCODE_DOWN];
    in->reset = ks[SDL_SCANCODE_R];
#else
    // TODO: Map to your board's input (e.g., GPIO buttons)
    // For now, no input; could add simple AI for P2 and auto-center P1
#endif
}

static int aabb_intersects(Rect a, Rect b) {
    return !(a.x+a.w < b.x || b.x+b.w < a.x || a.y+a.h < b.y || b.y+b.h < a.y);
}

static void clamp(float* v, float lo, float hi) { if (*v < lo) *v = lo; else if (*v > hi) *v = hi; }

int main(void) {
    if (backend_init() != 0) return 1;

    const int   W = VGA_WIDTH;
    const int   H = VGA_HEIGHT;
    const int   MARGIN = 10;
    const float PADDLE_W = 12.0f;
    const float PADDLE_H = 80.0f;
    const float PADDLE_SPEED = 420.0f; // px/s
    const float BALL_SPEED = 420.0f;   // base speed

    Rect p1 = { MARGIN, (H - PADDLE_H)/2.0f, PADDLE_W, PADDLE_H };
    Rect p2 = { W - MARGIN - PADDLE_W, (H - PADDLE_H)/2.0f, PADDLE_W, PADDLE_H };
    Ball ball = { W/2.0f, H/2.0f, BALL_SPEED, BALL_SPEED*0.35f, 6.0f };

    int score1 = 0, score2 = 0;
    const int WIN_SCORE = 5;
    int game_over = 0; // 0 playing, 1 someone won

    const pixel_t COL_BG   = rgb(0,0,0);
    const pixel_t COL_FG   = rgb(255,255,255);
    const pixel_t COL_DIM  = rgb(120,120,120);

    uint32_t prev_ms = 0;
#ifdef USE_SDL
    prev_ms = SDL_GetTicks();
#endif

    for (;;) {
        // Timing (fixed-ish 60 FPS)
#ifdef USE_SDL
        uint32_t now = SDL_GetTicks();
        float dt = (now - prev_ms) / 1000.0f; if (dt > 0.033f) dt = 0.033f; // clamp
        prev_ms = now;
#else
        float dt = 0.016f; // approx 60 FPS for baremetal
#endif

        // Input
        Input in; read_input(&in);

        if (!game_over) {
            // Paddles movement
            if (in.up1)   p1.y -= PADDLE_SPEED * dt;
            if (in.down1) p1.y += PADDLE_SPEED * dt;
            if (in.up2)   p2.y -= PADDLE_SPEED * dt;
            if (in.down2) p2.y += PADDLE_SPEED * dt;

#ifndef USE_SDL
            // Simple AI for P2 when no inputs present (tracks ball)
            float target = ball.y - p2.h * 0.5f;
            if (target > p2.y) p2.y += PADDLE_SPEED * 0.6f * dt;
            else if (target < p2.y) p2.y -= PADDLE_SPEED * 0.6f * dt;
#endif

            clamp(&p1.y, 0.0f, (float)H - p1.h);
            clamp(&p2.y, 0.0f, (float)H - p2.h);

            // Ball physics
            ball.x += ball.vx * dt;
            ball.y += ball.vy * dt;

            // Collide with top/bottom walls
            if (ball.y - ball.r < 0) { ball.y = ball.r; ball.vy = -ball.vy; }
            if (ball.y + ball.r > H) { ball.y = H - ball.r; ball.vy = -ball.vy; }

            // Collide with paddles (AABB vs circle approx using small rect around ball)
            Rect brect = { ball.x - ball.r, ball.y - ball.r, ball.r*2, ball.r*2 };
            if (aabb_intersects(brect, p1) && ball.vx < 0) {
                // Compute hit position (-1..1) on paddle to vary angle
                float rel = ((ball.y - p1.y) / p1.h) * 2.0f - 1.0f;
                float angle = rel * (M_PI * 0.35f); // up to ~35 degrees
                float speed = sqrtf(ball.vx*ball.vx + ball.vy*ball.vy) * 1.03f; // slight accel
                ball.vx = cosf(angle) * speed;  if (ball.vx < 120.0f) ball.vx = 120.0f; // ensure rightward
                ball.vy = sinf(angle) * speed;
                ball.x = p1.x + p1.w + ball.r; // push out
            }
            if (aabb_intersects(brect, p2) && ball.vx > 0) {
                float rel = ((ball.y - p2.y) / p2.h) * 2.0f - 1.0f;
                float angle = rel * (M_PI * 0.35f);
                float speed = sqrtf(ball.vx*ball.vx + ball.vy*ball.vy) * 1.03f;
                ball.vx = -cosf(angle) * speed; if (ball.vx > -120.0f) ball.vx = -120.0f; // ensure leftward
                ball.vy =  sinf(angle) * speed;
                ball.x = p2.x - ball.r; // push out
            }

            // Scoring
            if (ball.x < -20) { // P2 scores
                score2++;
                ball.x = W/2.0f; ball.y = H/2.0f;
                ball.vx =  BALL_SPEED; ball.vy = BALL_SPEED * ((rand()%200-100)/200.0f);
            } else if (ball.x > W + 20) { // P1 scores
                score1++;
                ball.x = W/2.0f; ball.y = H/2.0f;
                ball.vx = -BALL_SPEED; ball.vy = BALL_SPEED * ((rand()%200-100)/200.0f);
            }

            if (score1 >= WIN_SCORE || score2 >= WIN_SCORE) {
                game_over = 1;
            }
        } else {
            // Game over; wait for reset key to restart
            if (in.reset) {
                score1 = score2 = 0; game_over = 0;
                p1.y = p2.y = (H - PADDLE_H)/2.0f;
                ball.x = W/2.0f; ball.y = H/2.0f; ball.vx = BALL_SPEED; ball.vy = BALL_SPEED*0.35f;
            }
        }

        // ---------------- Render ----------------
        clear_screen(COL_BG);

        // Middle dashed line
        for (int y=0; y<H; y+=24) fill_rect(W/2 - 2, y, 4, 16, COL_DIM);

        // Paddles and ball
        fill_rect((int)p1.x, (int)p1.y, (int)p1.w, (int)p1.h, COL_FG);
        fill_rect((int)p2.x, (int)p2.y, (int)p2.w, (int)p2.h, COL_FG);
        fill_rect((int)(ball.x - ball.r), (int)(ball.y - ball.r), (int)(ball.r*2), (int)(ball.r*2), COL_FG);

        // Scores
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", score1);
        draw_text(W/2 - 80, 20, buf, 4, COL_FG);
        snprintf(buf, sizeof(buf), "%d", score2);
        draw_text(W/2 + 40, 20, buf, 4, COL_FG);

        if (game_over) {
            const char* msg = (score1 > score2) ? "P1 WINS" : "P2 WINS";
            draw_text(W/2 - 3*6*5, H/2 - 40, msg, 5, COL_FG); // centered approx
            draw_text(W/2 - 12*6*2/2, H/2 + 10, "PRESS R TO RESTART", 2, COL_FG);
        }

        backend_present();

        // Frame pacing for non-vsync backends
#ifndef USE_SDL
        backend_delay_ms(16);
#endif
    }

    backend_shutdown();
    return 0;
}
