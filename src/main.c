

/*
 * Snake för Picocomputer 6502 (RP6502)
 *
 * Styrning:  A = sväng vänster, D = sväng höger, ESC = avsluta
 *
 * Så fungerar det:
 *  - VGA: canvas 320x240, mode 1 (teckenläge) med 8x8-tecken = 40x30 celler.
 *    Varje cell är 3 byte i XRAM: tecken, förgrundsfärg, bakgrundsfärg.
 *    Vi ritar med mellanslag och ändrar bara bakgrundsfärgen.
 *  - Tangentbord: RIA skriver 32 byte (256 bitar) till XRAM. Bit N är 1
 *    medan USB HID-tangent N hålls nedtryckt.
 *  - XRAM nås via "portal 0": RIA.addr0 (adress), RIA.step0 (steg) och
 *    RIA.rw0 (läs/skriv en byte och stega fram adressen).
 *  - Spelplanen i XRAM är också spelets "sanning": för att se om huvudet
 *    krockar läser vi färgen i cellen framför.
 */

#include "xram.h"
#include <rp6502.h>
#include <stdio.h>
#include <stdlib.h>


/* ---------- XRAM-karta ---------- */
#define TEXT_XRAM 0x1000 /* 40*30*3 = 3600 byte teckendata  */
#define KEYS_XRAM 0xFF00 /* 32 byte tangentbord             */
#define CFG_XRAM 0xFF40  /* mode 1-konfiguration, 16 byte   */

#define COLS 40
#define ROWS 30
#define CELL_BYTES 3

/* ---------- färger (index i den inbyggda ANSI-paletten) ---------- */
#define C_EMPTY 16 /* svart, ej transparent */
#define C_WALL 4   /* blå                   */
#define C_BODY 2   /* grön                  */
#define C_HEAD 10  /* ljusgrön              */
#define C_FOOD 1   /* röd                   */
#define C_TEXT 15  /* vit                   */

/* ---------- USB HID-tangentkoder ---------- */
#define KEY_A 0x04
#define KEY_D 0x07
#define KEY_ESC 0x29
#define KEY_SPACE 0x2C

#define KEY_TURN_LEFT KEY_A
#define KEY_TURN_RIGHT KEY_D

/* ---------- spelinställningar ---------- */
#define STEP_FRAMES 6 /* antal bildrutor (1/60 s) per steg. Lägre = snabbare */
#define MAXLEN 1024   /* måste vara en tvåpotens */
#define MASK (MAXLEN - 1)

/* ---------- tillstånd ---------- */
static unsigned char keys[32];
static unsigned char sx[MAXLEN], sy[MAXLEN]; /* ormens celler (ringbuffert) */
static unsigned head, tail;                  /* head = nästa lediga plats   */
static unsigned score;

static const signed char dx[4] = {0, 1, 0, -1}; /* 0=upp 1=höger 2=ner 3=vänster */
static const signed char dy[4] = {-1, 0, 1, 0};

#define KEY_DOWN(code) ((keys[(code) >> 3] & (1 << ((code) & 7))) != 0)

/* ---------- XRAM-hjälpfunktioner ---------- */

static void put_cell(unsigned char x, unsigned char y,
                     unsigned char glyph, unsigned char fg, unsigned char bg)
{
    RIA.addr0 = TEXT_XRAM + ((unsigned)y * COLS + x) * CELL_BYTES;
    RIA.step0 = 1;
    RIA.rw0 = glyph;
    RIA.rw0 = fg;
    RIA.rw0 = bg;
}

static unsigned char get_bg(unsigned char x, unsigned char y)
{
    RIA.addr0 = TEXT_XRAM + ((unsigned)y * COLS + x) * CELL_BYTES + 2;
    RIA.step0 = 1;
    return RIA.rw0;
}

static void read_keys(void)
{
    unsigned char i;
    RIA.addr0 = KEYS_XRAM;
    RIA.step0 = 1;
    for (i = 0; i < 32; i++)
        keys[i] = RIA.rw0;
}

static void wait_vsync(void)
{
    unsigned char v = RIA.vsync;
    while (v == RIA.vsync)
        ;
}

/* ---------- ritning ---------- */

static void video_init(void)
{
    /* mode1_config_t: x_wrap, y_wrap, x_pos, y_pos, bredd, höjd,
       datapekare, palettpekare, fontpekare (0xFFFF = inbyggd) */
    static const unsigned char cfg[16] = {
        0, 0,
        0, 0,
        0, 0,
        COLS, 0,
        ROWS, 0,
        TEXT_XRAM & 0xFF, TEXT_XRAM >> 8,
        0xFF, 0xFF,
        0xFF, 0xFF};
    unsigned char i;

    xreg_vga_canvas(1); /* 320x240 */

    RIA.addr0 = CFG_XRAM;
    RIA.step0 = 1;
    for (i = 0; i < 16; i++)
        RIA.rw0 = cfg[i];

    xreg_vga_mode(1, 3, CFG_XRAM, 0); /* mode 1, 8-bitars färg, plan 0 */
    xreg_ria_keyboard(KEYS_XRAM);
}

static void draw_board(void)
{
    unsigned i;
    unsigned char x, y;

    /* rensa hela skärmen */
    RIA.addr0 = TEXT_XRAM;
    RIA.step0 = 1;
    for (i = 0; i < COLS * ROWS; i++)
    {
        RIA.rw0 = ' ';
        RIA.rw0 = C_TEXT;
        RIA.rw0 = C_EMPTY;
    }

    /* väggar: rad 1 och 29, kolumn 0 och 39 */
    for (x = 0; x < COLS; x++)
    {
        put_cell(x, 1, ' ', C_TEXT, C_WALL);
        put_cell(x, ROWS - 1, ' ', C_TEXT, C_WALL);
    }
    for (y = 1; y < ROWS; y++)
    {
        put_cell(0, y, ' ', C_TEXT, C_WALL);
        put_cell(COLS - 1, y, ' ', C_TEXT, C_WALL);
    }
}

/* Skriver text på rad 0, kolumn 1..28, och fyller resten med blanksteg */
static void status(const char *s)
{
    unsigned char x = 1;
    while (*s)
        put_cell(x++, 0, *s++, C_TEXT, C_EMPTY);
    while (x < 29)
        put_cell(x++, 0, ' ', C_TEXT, C_EMPTY);
}

static void show_score(void)
{
    char buf[12];
    unsigned char i;
    sprintf(buf, "SCORE %u", score);
    for (i = 0; buf[i]; i++)
        put_cell(30 + i, 0, buf[i], C_TEXT, C_EMPTY);
}

static void place_food(void)
{
    unsigned char fx, fy;
    do
    {
        fx = 1 + rand() % 38; /* kolumn 1..38 */
        fy = 2 + rand() % 27; /* rad 2..28    */
    } while (get_bg(fx, fy) != C_EMPTY); /* försök tills cellen är ledig */
    put_cell(fx, fy, ' ', C_TEXT, C_FOOD);
}

/* ---------- spelet ---------- */

/* Returnerar 0 om spelaren tryckte ESC, 1 om ormen dog */
static unsigned char play(void)
{
    unsigned char dir = 1; /* börja åt höger */
    unsigned char turn = 0;
    unsigned char frames = 0;
    unsigned char a, d, prev_a, prev_d;
    unsigned char hx, hy, nx, ny, c;

    draw_board();
    status("A/D = TURN   ESC = QUIT");

    /* ormen: tre celler, huvudet längst till höger */
    head = tail = 0;
    sx[0] = 18; sy[0] = 15;
    sx[1] = 19; sy[1] = 15;
    sx[2] = 20; sy[2] = 15;
    head = 3;
    put_cell(18, 15, ' ', C_TEXT, C_BODY);
    put_cell(19, 15, ' ', C_TEXT, C_BODY);
    put_cell(20, 15, ' ', C_TEXT, C_HEAD);

    score = 0;
    show_score();
    place_food();

    read_keys();
    prev_a = KEY_DOWN(KEY_TURN_LEFT);
    prev_d = KEY_DOWN(KEY_TURN_RIGHT);

    for (;;)
    {
        wait_vsync();
        read_keys();

        if (KEY_DOWN(KEY_ESC))
            return 0;

        /* Svängar ska reagera på själva nedtryckningen, inte på att
           tangenten hålls nere, så vi jämför med förra bildrutan. */
        a = KEY_DOWN(KEY_TURN_LEFT);
        d = KEY_DOWN(KEY_TURN_RIGHT);
        if (a && !prev_a && !turn)
            turn = 3; /* dir + 3 = vänstersväng */
        if (d && !prev_d && !turn)
            turn = 1; /* dir + 1 = högersväng   */
        prev_a = a;
        prev_d = d;

        if (++frames < STEP_FRAMES)
            continue;
        frames = 0;

        dir = (dir + turn) & 3;
        turn = 0;

        hx = sx[(head - 1) & MASK];
        hy = sy[(head - 1) & MASK];
        nx = hx + dx[dir];
        ny = hy + dy[dir];

        c = get_bg(nx, ny); /* vad finns i cellen framför huvudet? */
        if (c == C_WALL || c == C_BODY)
            return 1;

        put_cell(hx, hy, ' ', C_TEXT, C_BODY); /* gamla huvudet blir kropp */
        sx[head] = nx;
        sy[head] = ny;
        head = (head + 1) & MASK;
        put_cell(nx, ny, ' ', C_TEXT, C_HEAD);

        if (c == C_FOOD)
        {
            ++score; /* ormen växer: svansen får vara kvar */
            show_score();
            place_food();
        }
        else
        {
            put_cell(sx[tail], sy[tail], ' ', C_TEXT, C_EMPTY);
            tail = (tail + 1) & MASK;
        }
    }
}

/* Väntar på mellanslag. Returnerar antal bildrutor som gått (används som
   slumpfrö), eller 0 om ESC trycktes. */
static unsigned wait_for_space(void)
{
    unsigned n = 1;
    for (;;)
    {
        wait_vsync();
        read_keys();
        if (KEY_DOWN(KEY_ESC))
            return 0;
        if (KEY_DOWN(KEY_SPACE))
            return n;
        ++n;
    }
}

int main(void)
{
    unsigned seed;

    video_init();
    draw_board();
    status("PRESS SPACE TO START");

    seed = wait_for_space();
    if (!seed)
        return 0;
    srand(seed);

    for (;;)
    {
        if (!play())
            return 0;
        status("GAME OVER  SPACE = AGAIN");
        if (!wait_for_space())
            return 0;
    }
}