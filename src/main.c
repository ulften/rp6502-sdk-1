
/*
 * Snake för Picocomputer 6502 - steg 2: levande bakgrund
 *
 * Nytt jämfört med steg 1:
 *  - Gräset vajar: tile-bilderna för gräs skrivs om i XRAM var 32:a bildruta.
 *    Eftersom kartan bara pekar på tile-numret ändras alla gräsrutor på en gång.
 *  - Murarna glöder och äpplet blänker: vi ändrar enstaka färger i paletten
 *    (2 byte per färg). Hela skärmen byter färg utan att kartan ritas om.
 *
 * Styrning:  A = sväng vänster, D = sväng höger, ESC = avsluta
 *
 * Grafiken är nu två "plan" som VGA lägger ovanpå varandra:
 *   plan 0: tile map (mode 2). 40x30 celler, 1 byte per cell = tile-nummer.
 *           Tiles är 8x8 pixlar, 4 bitar per pixel (16 färger), 32 byte styck.
 *   plan 1: text (mode 1, 1 bit färg) bara på översta raden, för poängen.
 *
 * Allt ligger i XRAM. VGA läser det direkt varje bildruta:
 *   0x1000  textrad (40 byte)
 *   0x1100  tile map (1200 byte)
 *   0x2000  tile-bilder (32 byte per tile)
 *   0xFF00  tangentbord (32 byte)
 *   0xFF40  konfiguration för tile map (16 byte)
 *   0xFF50  konfiguration för textraden (16 byte)
 *   0xFF80  palett (16 färger * 2 byte)
 */

#include <rp6502.h>
#include <stdio.h>
#include <stdlib.h>

/* XREG-genvägar, hämtade från RIA- och VGA-dokumentationen */
#define xreg_ria_keyboard(...) xreg(0, 0, 0, __VA_ARGS__)
#define xreg_vga_canvas(...) xreg(1, 0, 0, __VA_ARGS__)
#define xreg_vga_mode(...) xreg(1, 0, 1, __VA_ARGS__)

/* ---------- XRAM-karta ---------- */
#define TEXT_XRAM 0x1000
#define MAP_XRAM 0x1100
#define TILES_XRAM 0x2000
#define KEYS_XRAM 0xFF00
#define CFG_TILES 0xFF40
#define CFG_TEXT 0xFF50
#define PAL_XRAM 0xFF80

#define COLS 40
#define ROWS 30

/* mode 2: 4 bitar per pixel, 8x8-tiles */
#define MODE2_4BPP 0x02
#define MODE2_8X8 0x00

/* ---------- tile-nummer ---------- */
#define T_HUD 0
#define T_GRASS_A 1
#define T_GRASS_B 2
#define T_WALL 3
#define T_BODY 4
#define T_FOOD 5
#define T_STONE 6
#define T_HEAD 7 /* 7=upp 8=höger 9=ner 10=vänster (samma ordning som dir) */

/* ---------- USB HID-tangentkoder ---------- */
#define KEY_A 0x04
#define KEY_D 0x07
#define KEY_ESC 0x29
#define KEY_SPACE 0x2C

/* Byt plats på dessa två om du vill ha A = höger och D = vänster */
#define KEY_TURN_LEFT KEY_A
#define KEY_TURN_RIGHT KEY_D

/* ---------- spelinställningar ---------- */
#define STEP_FRAMES 8
#define MAXLEN 1024
#define MASK (MAXLEN - 1)

/* ---------- tillstånd ---------- */
static unsigned char keys[32];
static unsigned char sx[MAXLEN], sy[MAXLEN];
static unsigned head, tail;
static unsigned score;
static unsigned char tick; /* räknar bildrutor, styr animationerna */

static const signed char dx[4] = {0, 1, 0, -1};
static const signed char dy[4] = {-1, 0, 1, 0};

#define KEY_DOWN(code) ((keys[(code) >> 3] & (1 << ((code) & 7))) != 0)

/* ---------- palett: 16 färger (R, G, B, 0-255) ---------- */
static const unsigned char pal[16][3] = {
    {0, 0, 0},       /* 0  svart (HUD)          */
    {36, 110, 44},   /* 1  gräs                 */
    {56, 150, 60},   /* 2  gräs, ljusare tuss   */
    {70, 70, 110},   /* 3  mur, mörk            */
    {120, 120, 170}, /* 4  mur, ljus (murbruk)  */
    {40, 190, 70},   /* 5  orm, kropp           */
    {15, 100, 35},   /* 6  orm, kant            */
    {120, 235, 100}, /* 7  orm, huvud/glans     */
    {255, 255, 255}, /* 8  vitt (ögon)          */
    {10, 10, 10},    /* 9  nästan svart (pupill)*/
    {225, 40, 40},   /* 10 äpple, rött          */
    {140, 15, 25},   /* 11 äpple, mörkrött      */
    {120, 80, 30},   /* 12 skaft, brunt         */
    {255, 255, 255}, /* 13 äpplets glans (pulserar) */
    {40, 40, 40},     /* 14 sten - grå */
    {0, 0, 0}};

/* ---------- tile-bilder som text: en siffra/bokstav per pixel ----------
   0-9 och a-f är index i paletten ovan. */
static const char *const ART[T_HEAD][8] = {
    /* T_HUD */
    {"00000000", "00000000", "00000000", "00000000",
     "00000000", "00000000", "00000000", "00000000"},
    /* T_GRASS_A */
    {"11111111", "11121111", "11111111", "11111111",
     "11111121", "11111111", "12111111", "11111111"},
    /* T_GRASS_B */
    {"11111111", "11111111", "11112111", "21111111",
     "11111111", "11111112", "11211111", "11111111"},
    /* T_WALL */
    {"44444444", "33343333", "33343333", "44444444",
     "33333433", "33333433", "44444444", "33343333"},
    /* T_BODY */
    {"66666666", "65555556", "65755556", "65555556",
     "65555556", "65555756", "65555556", "66666666"},
    /* T_FOOD */
    {"111c1111", "11aaaa11", "1aadaaa1", "1aaaaaa1",
     "1aaaaaa1", "1baaaab1", "11bbbb11", "11111111"}, 
     /* T_STONE */
    {"11111111", "111eee11", "11e0eee1", "1eeeeee1",
     "1eeeeee1", "11e0eee1", "111eee11", "11111111"} 
    };

/* Huvudet ritas bara en gång (uppåt) och roteras sedan i koden */
static const char *const ART_HEAD_UP[8] = {
    "66666666", "67977976", "67877876", "67777776",
    "67777776", "67777776", "67777776", "66666666"};

/* Gräsets andra bildruta: tuvorna har flyttat sig ett steg åt höger.
   [0] hör till T_GRASS_A och [1] till T_GRASS_B. */
static const char *const ART_GRASS_ALT[2][8] = {
    {"11111111", "11112111", "11111111", "11111111",
     "11111112", "11111111", "11211111", "11111111"},
    {"11111111", "11111111", "11111211", "12111111",
     "11111111", "21111111", "11121111", "11111111"}};

/* ---------- XRAM-hjälpfunktioner ---------- */

static void xram_write(unsigned addr, const unsigned char *src, unsigned n)
{
    RIA.addr0 = addr;
    RIA.step0 = 1;
    while (n--)
        RIA.rw0 = *src++;
}

static void put_tile(unsigned char x, unsigned char y, unsigned char t)
{
    RIA.addr0 = MAP_XRAM + (unsigned)y * COLS + x;
    RIA.step0 = 1;
    RIA.rw0 = t;
}

static unsigned char get_tile(unsigned char x, unsigned char y)
{
    RIA.addr0 = MAP_XRAM + (unsigned)y * COLS + x;
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

/* ---------- tiles och palett till XRAM ---------- */

/* Skriver en färg i paletten (RGB555 + alpha-bit 0x20 som gör färgen synlig) */
static void set_color(unsigned char i, unsigned char r, unsigned char g, unsigned char b)
{
    unsigned c = ((unsigned)(b >> 3) << 11) |
                 ((unsigned)(g >> 3) << 6) |
                 (unsigned)(r >> 3) | 0x20;
    RIA.addr0 = PAL_XRAM + (unsigned)i * 2;
    RIA.step0 = 1;
    RIA.rw0 = c & 0xFF; /* låg byte först */
    RIA.rw0 = c >> 8;
}

static void load_palette(void)
{
    unsigned char i;
    for (i = 0; i < 16; i++)
        set_color(i, pal[i][0], pal[i][1], pal[i][2]);
}

static void load_art(unsigned char px[8][8], const char *const *art)
{
    unsigned char r, c;
    char ch;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
        {
            ch = art[r][c];
            px[r][c] = (ch <= '9') ? (ch - '0') : (ch - 'a' + 10);
        }
}

/* Packar 8x8 pixlar till 32 byte: två pixlar per byte, vänstra pixeln i
   övre halvan av byten. Byt ordning här om bilderna ser spegelvända ut. */
static void store_tile(unsigned char id, unsigned char px[8][8])
{
    unsigned char r, c;
    RIA.addr0 = TILES_XRAM + (unsigned)id * 32;
    RIA.step0 = 1;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c += 2)
            RIA.rw0 = (px[r][c] << 4) | px[r][c + 1];
}

static void rotate_cw(unsigned char px[8][8])
{
    unsigned char t[8][8];
    unsigned char r, c;
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            t[r][c] = px[7 - c][r];
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            px[r][c] = t[r][c];
}

static void load_tiles(void)
{
    unsigned char px[8][8];
    unsigned char t, k;

    for (t = 0; t < T_HEAD; t++)
    {
        load_art(px, ART[t]);
        store_tile(t, px);
    }

    load_art(px, ART_HEAD_UP);
    for (k = 0; k < 4; k++)
    {
        store_tile(T_HEAD + k, px);
        rotate_cw(px); /* upp -> höger -> ner -> vänster */
    }
}

/* ---------- video ---------- */

static void video_init(void)
{
    /* mode2_config_t: x_wrap, y_wrap, x_pos, y_pos, bredd, höjd i tiles,
       pekare till kartan, till paletten och till tile-bilderna */
    static const unsigned char cfg_tiles[16] = {
        0, 0,
        0, 0,
        0, 0,
        COLS, 0,
        ROWS, 0,
        MAP_XRAM & 0xFF, MAP_XRAM >> 8,
        PAL_XRAM & 0xFF, PAL_XRAM >> 8,
        TILES_XRAM & 0xFF, TILES_XRAM >> 8};
    /* mode1_config_t för en enda textrad. Palett och font 0xFFFF = inbyggda */
    static const unsigned char cfg_text[16] = {
        0, 0,
        0, 0,
        0, 0,
        COLS, 0,
        1, 0,
        TEXT_XRAM & 0xFF, TEXT_XRAM >> 8,
        0xFF, 0xFF,
        0xFF, 0xFF};
    unsigned char i;

    xreg_vga_canvas(1); /* 320x240 */

    load_palette();
    load_tiles();

    xram_write(CFG_TILES, cfg_tiles, 16);
    xram_write(CFG_TEXT, cfg_text, 16);

    /* tomma blanksteg på textraden */
    RIA.addr0 = TEXT_XRAM;
    RIA.step0 = 1;
    for (i = 0; i < COLS; i++)
        RIA.rw0 = ' ';

    /* plan 0: tile map över hela canvasen */
    xreg_vga_mode(2, MODE2_4BPP | MODE2_8X8, CFG_TILES, 0);
    /* plan 1: text på scanline 0-7 (översta raden) */
    xreg_vga_mode(1, 0, CFG_TEXT, 1, 0, 8);

    xreg_ria_keyboard(KEYS_XRAM);
}

/* Gräset varierar lite: samma position ger alltid samma tile */
static unsigned char grass_tile(unsigned char x, unsigned char y)
{
    return (((unsigned)x * 7 + (unsigned)y * 13) % 5 == 0) ? T_GRASS_B : T_GRASS_A;
}

static unsigned char is_grass(unsigned char t)
{
    return t == T_GRASS_A || t == T_GRASS_B;
}

static void draw_board(void)
{
    unsigned char x, y, t;

    RIA.addr0 = MAP_XRAM;
    RIA.step0 = 1;
    for (y = 0; y < ROWS; y++)
        for (x = 0; x < COLS; x++)
        {
            if (y == 0)
                t = T_HUD;
            else if (y == 1 || y == ROWS - 1 || x == 0 || x == COLS - 1)
                t = T_WALL;
            else
                t = grass_tile(x, y);
            RIA.rw0 = t;
        }
}

/* Text på rad 0, kolumn 1..28, resten blanksteg */
static void status(const char *s)
{
    unsigned char x = 1;
    RIA.addr0 = TEXT_XRAM + 1;
    RIA.step0 = 1;
    while (*s)
    {
        RIA.rw0 = *s++;
        x++;
    }
    while (x < 29)
    {
        RIA.rw0 = ' ';
        x++;
    }
}

static void show_score(void)
{
    char buf[12];
    unsigned char i;
    sprintf(buf, "SCORE %u", score);
    RIA.addr0 = TEXT_XRAM + 30;
    RIA.step0 = 1;
    for (i = 0; buf[i]; i++)
        RIA.rw0 = buf[i];
    for (; i < 39; i++)
        RIA.rw0 = ' ';
}

static void place_food(void)
{
    unsigned char fx, fy;
    do
    {
        fx = 1 + rand() % 38; /* kolumn 1..38 */
        fy = 2 + rand() % 27; /* rad 2..28    */
    } while (!is_grass(get_tile(fx, fy)));
    put_tile(fx, fy, T_FOOD);
}

static void place_stones(void)
{
    unsigned char fx, fy;
    int i;

    for (i = 0; i < 5; i++) {
        do
        {
            fx = 1 + rand() % 38; /* kolumn 1..38 */
            fy = 2 + rand() % 27; /* rad 2..28    */
        } while (!is_grass(get_tile(fx, fy)));
        put_tile(fx, fy, T_STONE);
    }
}

/* ---------- animation ---------- */

/* Gräset: skriv om tile-bilden för T_GRASS_A och T_GRASS_B.
   phase 0 = ursprungsbilden, phase 1 = tuvorna ett steg åt höger. */
static void sway(unsigned char phase)
{
    unsigned char px[8][8];

    load_art(px, phase ? ART_GRASS_ALT[0] : ART[T_GRASS_A]);
    store_tile(T_GRASS_A, px);
    load_art(px, phase ? ART_GRASS_ALT[1] : ART[T_GRASS_B]);
    store_tile(T_GRASS_B, px);
}

/* Palettpuls: murbruket (färg 4) och äpplets glans (färg 13) */
static void pulse(void)
{
    unsigned char p = (tick >> 2) & 15; /* 0..15 */
    if (p > 7)
        p = 15 - p; /* triangelvåg 0..7..0 */

    set_color(4, 120 + p * 16, 120 + p * 16, 170 + p * 10);
    set_color(13, 255, 255 - p * 20, 255 - p * 20);
}

/* Anropas en gång per bildruta, både på titelskärmen och i spelet */
static void animate(void)
{
    ++tick;
    if ((tick & 3) == 0)
        pulse();
    if ((tick & 31) == 0)
        sway((tick >> 5) & 1);
}

/* ---------- spelet ---------- */

/* Returnerar 0 om spelaren tryckte ESC, 1 om ormen dog */
static unsigned char play(void)
{
    unsigned char dir = 1; /* börja åt höger */
    unsigned char turn = 0;
    unsigned char frames = 0;
    unsigned char a, d, prev_a, prev_d;
    unsigned char hx, hy, nx, ny, t;

    draw_board();
    status("A/D = TURN   ESC = QUIT");

    head = tail = 0;
    sx[0] = 18; sy[0] = 15;
    sx[1] = 19; sy[1] = 15;
    sx[2] = 20; sy[2] = 15;
    head = 3;
    put_tile(18, 15, T_BODY);
    put_tile(19, 15, T_BODY);
    put_tile(20, 15, T_HEAD + dir);

    score = 0;
    show_score();
    place_stones();
    place_food();

    read_keys();
    prev_a = KEY_DOWN(KEY_TURN_LEFT);
    prev_d = KEY_DOWN(KEY_TURN_RIGHT);

    for (;;)
    {
        wait_vsync();
        animate();
        read_keys();

        if (KEY_DOWN(KEY_ESC))
            return 0;

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

        t = get_tile(nx, ny); /* vad finns i cellen framför huvudet? */
        if (t == T_WALL || t == T_BODY || t == T_STONE)
            return 1;

        put_tile(hx, hy, T_BODY); /* gamla huvudet blir kropp */
        sx[head] = nx;
        sy[head] = ny;
        head = (head + 1) & MASK;
        put_tile(nx, ny, T_HEAD + dir); /* huvudet tittar åt färdriktningen */

        if (t == T_FOOD)
        {
            ++score;
            show_score();
            place_food();
        }
        else
        {
            /* svansen försvinner: tillbaka till gräs */
            put_tile(sx[tail], sy[tail], grass_tile(sx[tail], sy[tail]));
            tail = (tail + 1) & MASK;
        }
    }
}

static unsigned wait_for_space(void)
{
    unsigned n = 1;
    for (;;)
    {
        wait_vsync();
        animate();
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
