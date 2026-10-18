/*
 * Snake för Picocomputer 6502 - steg 3c: stenar bakom svansen
 *
 * Nytt i 3c: var STONE_EVERY:e äpple ger en sten som landar i cellen som
 * svansen lämnar. Den avslöjas när svansspriten glider bort. Högst STONE_MAX
 * sådana stenar finns kvar på planen; när en ny kommer blir den äldsta gräs.
 *
 * Steg 3b: sprites för huvud och svans
 *
 * Ändringar jämfört med steg 3:
 *  - Svansen är också en sprite. När svansen lämnar en cell sätts kartan till
 *    gräs direkt, men en kropps-sprite glider från den cellen till nästa
 *    segment. Då rör sig båda ändarna av ormen mjukt (mitten står stilla
 *    mellan stegen, precis som hos en riktig orm).
 *  - Stenar (T_STONE) som ormen måste undvika. Antal styrs av NUM_STONES.
 *  - Poängraden fylls med blanksteg så att gamla siffror inte blir kvar.
 *
 * Steg 3:
 *
 * Nytt i steg 3:
 *  - Ormens huvud är en sprite (mode 5, 4 bitar, 8x8) som glider en pixel per
 *    bildruta mellan cellerna, i stället för att hoppa en hel cell åt gången.
 *    Spelets logik går fortfarande i hela celler, men VGA ritar sprite-lagret
 *    ovanpå tile-kartan utan att 6502:an ritar om något.
 *  - Spriten använder samma bilder som huvud-tiles (samma format i XRAM) men
 *    en egen palett där färg 0 är genomskinlig.
 *  - Huvudets cell i kartan är T_OCC: en tile som ser ut som gräs men räknas
 *    som upptagen. Äpplet ligger kvar i kartan tills huvudet har glidit dit.
 *
 * Från steg 2 (jämfört med steg 1):
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
 *   0xFFA0  spritepalett (som ovan, men färg 0 genomskinlig)
 *   0xFFC0  spritelista (2 sprites * 8 byte: 0 = svans, 1 = huvud)
 */

#include <rp6502.h>
#include <stdio.h>
#include <stdlib.h>

/* XREG-genvägar, hämtade från RIA- och VGA-dokumentationen */
#define xreg_ria_keyboard(...) xreg(0, 0, 0, __VA_ARGS__)
#define xreg_vga_canvas(...) xreg(1, 0, 0, __VA_ARGS__)
#define xreg_vga_mode(...) xreg(1, 0, 1, __VA_ARGS__)
#define xreg_vga_mode5(...) xreg(1, 0, 1, 5, __VA_ARGS__)

/* ---------- XRAM-karta ---------- */
#define TEXT_XRAM 0x1000
#define MAP_XRAM 0x1100
#define TILES_XRAM 0x2000
#define KEYS_XRAM 0xFF00
#define CFG_TILES 0xFF40
#define CFG_TEXT 0xFF50
#define PAL_XRAM 0xFF80
#define SPR_PAL 0xFFA0
#define SPR_CFG 0xFFC0

#define COLS 40
#define ROWS 30

/* mode 2: 4 bitar per pixel, 8x8-tiles */
#define MODE2_4BPP 0x02
#define MODE2_8X8 0x00

/* mode 5: sprites med palett, 4 bitar, 8x8 */
#define MODE5_4BPP 0x02
#define MODE5_8X8 0x00

/* ---------- tile-nummer ---------- */
#define T_HUD 0
#define T_GRASS_A 1
#define T_GRASS_B 2
#define T_WALL 3
#define T_BODY 4
#define T_FOOD 5
#define T_HEAD 6 /* 6=upp 7=höger 8=ner 9=vänster (samma ordning som dir) */
#define T_OCC 10 /* ser ut som gräs, markerar cellen där huvudet är */
#define T_STONE 11

/* ---------- USB HID-tangentkoder ---------- */
#define KEY_A 0x04
#define KEY_D 0x07
#define KEY_ESC 0x29
#define KEY_SPACE 0x2C

/* Byt plats på dessa två om du vill ha A = höger och D = vänster */
#define KEY_TURN_LEFT KEY_A
#define KEY_TURN_RIGHT KEY_D

/* ---------- spelinställningar ---------- */
#define NUM_STONES 8   /* stenar som finns från början */
#define STONE_MAX 6    /* högst så många stenar från äpplen åt gången */
#define STONE_EVERY 1  /* en sten per så här många äpplen */
#define STEP_FRAMES 8
#define MAXLEN 1024
#define MASK (MAXLEN - 1)

/* ---------- tillstånd ---------- */
static unsigned char keys[32];
static unsigned char sx[MAXLEN], sy[MAXLEN];
static unsigned head, tail;
static unsigned score;
static unsigned char tick; /* räknar bildrutor, styr animationerna */

/* kö med de stenar som äpplen gett, äldst först (ringbuffert) */
static unsigned char stq_x[STONE_MAX], stq_y[STONE_MAX];
static unsigned char stq_first, stq_count;

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
    {160, 160, 165}, /* 14 sten, ljus          */
    {90, 90, 100}};  /* 15 sten, mörk          */

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
     "1aaaaaa1", "1baaaab1", "11bbbb11", "11111111"}};

/* Huvudet ritas bara en gång (uppåt) och roteras sedan i koden */
static const char *const ART_HEAD_UP[8] = {
    "66666666", "67977976", "67877876", "67777776",
    "67777776", "67777776", "67777776", "66666666"};

static const char *const ART_STONE[8] = {
    "111ee111", "11eeeef1", "1eeeeeff", "1eeeeeff",
    "1eeeeeff", "1feeeeff", "11ffff11", "11111111"};

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

/* Placerar en sprite (idx 0 = svans, 1 = huvud). x och y är pixlar på
   canvasen (0..319, 0..239).
   t är tile-numret vars bild används som sprite-bild (T_HEAD + riktning).
   Spriten är ett mode5_sprite_t: x, y, bildpekare, palettpekare (8 byte). */
static void sprite_set(unsigned char idx, unsigned x, unsigned y, unsigned char t)
{
    unsigned img = TILES_XRAM + (unsigned)t * 32;
    RIA.addr0 = SPR_CFG + (unsigned)idx * 8;
    RIA.step0 = 1;
    RIA.rw0 = x & 0xFF;
    RIA.rw0 = x >> 8;
    RIA.rw0 = y & 0xFF;
    RIA.rw0 = y >> 8;
    RIA.rw0 = img & 0xFF;
    RIA.rw0 = img >> 8;
    RIA.rw0 = SPR_PAL & 0xFF;
    RIA.rw0 = SPR_PAL >> 8;
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

/* Skriver en färg i en palett (RGB555 + alpha-bit 0x20 som gör färgen synlig) */
static void set_color_at(unsigned base, unsigned char i,
                         unsigned char r, unsigned char g, unsigned char b)
{
    unsigned c = ((unsigned)(b >> 3) << 11) |
                 ((unsigned)(g >> 3) << 6) |
                 (unsigned)(r >> 3) | 0x20;
    RIA.addr0 = base + (unsigned)i * 2;
    RIA.step0 = 1;
    RIA.rw0 = c & 0xFF; /* låg byte först */
    RIA.rw0 = c >> 8;
}

static void set_color(unsigned char i, unsigned char r, unsigned char g, unsigned char b)
{
    set_color_at(PAL_XRAM, i, r, g, b);
}

static void load_palette(void)
{
    unsigned char i;
    for (i = 0; i < 16; i++)
        set_color(i, pal[i][0], pal[i][1], pal[i][2]);

    /* Spritepaletten är en kopia, men färg 0 är genomskinlig (alpha-bit av) */
    for (i = 1; i < 16; i++)
        set_color_at(SPR_PAL, i, pal[i][0], pal[i][1], pal[i][2]);
    RIA.addr0 = SPR_PAL;
    RIA.step0 = 1;
    RIA.rw0 = 0;
    RIA.rw0 = 0;
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

    load_art(px, ART[T_GRASS_A]); /* T_OCC ser ut som gräs */
    store_tile(T_OCC, px);

    load_art(px, ART_STONE);
    store_tile(T_STONE, px);
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

    sprite_set(0, 320, 240, T_BODY); /* utanför canvasen = osynlig */
    sprite_set(1, 320, 240, T_HEAD);

    /* tomma blanksteg på textraden */
    RIA.addr0 = TEXT_XRAM;
    RIA.step0 = 1;
    for (i = 0; i < COLS; i++)
        RIA.rw0 = ' ';

    /* plan 0: tile map över hela canvasen */
    xreg_vga_mode(2, MODE2_4BPP | MODE2_8X8, CFG_TILES, 0);
    /* plan 1: text på scanline 0-7 (översta raden) */
    xreg_vga_mode(1, 0, CFG_TEXT, 1, 0, 8);

    /* sprite-lagret ovanpå plan 0: 2 sprites, 8x8, 4 bitar */
    xreg_vga_mode5(MODE5_4BPP | MODE5_8X8, SPR_CFG, 2, 0);

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
    for (; i < 10; i++) /* fyll ut till kolumn 39 så gamla siffror försvinner */
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

/* Slumpar ut stenar på ledigt gräs, men inte på ormens startsträcka
   (rad 14-16, kolumn 16-32) så att man inte dör direkt. */
static void place_stones(void)
{
    unsigned char i, fx, fy;
    for (i = 0; i < NUM_STONES; i++)
    {
        do
        {
            fx = 1 + rand() % 38;
            fy = 2 + rand() % 27;
        } while (!is_grass(get_tile(fx, fy)) ||
                 (fy >= 14 && fy <= 16 && fx >= 16 && fx <= 32));
        put_tile(fx, fy, T_STONE);
    }
}

/* Lägger en sten i cellen (x,y). Är kön full blir den äldsta stenen gräs. */
static void drop_stone(unsigned char x, unsigned char y)
{
    unsigned char i;

    if (stq_count == STONE_MAX)
    {
        put_tile(stq_x[stq_first], stq_y[stq_first],
                 grass_tile(stq_x[stq_first], stq_y[stq_first]));
        stq_first = (stq_first + 1) % STONE_MAX;
        --stq_count;
    }
    i = (stq_first + stq_count) % STONE_MAX;
    stq_x[i] = x;
    stq_y[i] = y;
    ++stq_count;
    put_tile(x, y, T_STONE);
}

/* ---------- animation ---------- */

/* Gräset: skriv om tile-bilden för T_GRASS_A och T_GRASS_B.
   phase 0 = ursprungsbilden, phase 1 = tuvorna ett steg åt höger. */
static void sway(unsigned char phase)
{
    unsigned char px[8][8];

    load_art(px, phase ? ART_GRASS_ALT[0] : ART[T_GRASS_A]);
    store_tile(T_GRASS_A, px);
    store_tile(T_OCC, px); /* T_OCC ska vaja som gräs */
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
    unsigned char vdir = 1;
    unsigned char turn = 0;
    unsigned char frames = 0;
    unsigned char started = 0; /* huvudet glider först efter första steget */
    unsigned char a, d, prev_a, prev_d;
    unsigned char hx, hy, nx, ny, t;
    unsigned char vx = 20, vy = 15; /* cellen huvudspriten glider ifrån */
    unsigned char tx = 18, ty = 15; /* cellen svansspriten glider ifrån */
    signed char tdx = 0, tdy = 0;   /* svansens riktning (0 = står still) */
    unsigned char off;
    unsigned char pending = 0; /* stenar som ska landa bakom svansen */

    draw_board();
    status("A/D = TURN   ESC = QUIT");

    head = tail = 0;
    sx[0] = 18; sy[0] = 15;
    sx[1] = 19; sy[1] = 15;
    sx[2] = 20; sy[2] = 15;
    head = 3;
    put_tile(18, 15, T_BODY);
    put_tile(19, 15, T_BODY);
    put_tile(20, 15, T_OCC); /* huvudets cell: ser ut som gräs men är upptagen */
    sprite_set(0, 18 * 8, 15 * 8, T_BODY);
    sprite_set(1, 20 * 8, 15 * 8, T_HEAD + dir);

    score = 0;
    show_score();
    stq_first = stq_count = 0;
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

        /* Spelets logik går i hela celler, ett steg var STEP_FRAMES bildruta */
        if (++frames >= STEP_FRAMES)
        {
            frames = 0;

            dir = (dir + turn) & 3;
            turn = 0;

            hx = sx[(head - 1) & MASK];
            hy = sy[(head - 1) & MASK];
            nx = hx + dx[dir];
            ny = hy + dy[dir];

            /* båda spritarna har nu glidit klart; vänd huvudet åt nya hållet */
            sprite_set(0, (unsigned)(tx + tdx) * 8, (unsigned)(ty + tdy) * 8, T_BODY);
            sprite_set(1, (unsigned)hx * 8, (unsigned)hy * 8, T_HEAD + dir);

            t = get_tile(nx, ny); /* vad finns i cellen framför huvudet? */
            if (t == T_WALL || t == T_BODY || t == T_OCC || t == T_STONE)
                return 1;

            put_tile(hx, hy, T_BODY); /* gamla huvudcellen blir kropp */
            sx[head] = nx;
            sy[head] = ny;
            head = (head + 1) & MASK;

            if (t == T_FOOD)
            {
                /* Äpplet ligger kvar i kartan tills huvudspriten har glidit dit.
                   Ormen växer: svansen står still. */
                ++score;
                show_score();
                if (score % STONE_EVERY == 0)
                    ++pending; /* landar när svansen nästa gång flyttar sig */
                place_food();
                tx = sx[tail];
                ty = sy[tail];
                tdx = 0;
                tdy = 0;
            }
            else
            {
                put_tile(nx, ny, T_OCC); /* huvudets nya cell */

                /* Svansen lämnar sin cell: kartan blir gräs direkt, men
                   svansspriten glider från cellen mot nästa segment. */
                tx = sx[tail];
                ty = sy[tail];
                put_tile(tx, ty, grass_tile(tx, ty));
                if (pending)
                {
                    drop_stone(tx, ty); /* syns när svansspriten glider bort */
                    --pending;
                }
                tail = (tail + 1) & MASK;
                tdx = (signed char)((int)sx[tail] - (int)tx);
                tdy = (signed char)((int)sy[tail] - (int)ty);
            }

            vx = hx;
            vy = hy;
            vdir = dir;
            started = 1;
        }

        /* Varje bildruta: flytta spritarna en bit mot sina nästa celler */
        off = (unsigned char)((unsigned)frames * 8 / STEP_FRAMES);
        sprite_set(0, (unsigned)(tx * 8 + tdx * off),
                   (unsigned)(ty * 8 + tdy * off), T_BODY);
        if (!started)
            off = 0;
        sprite_set(1, (unsigned)(vx * 8 + dx[vdir] * off),
                   (unsigned)(vy * 8 + dy[vdir] * off), T_HEAD + vdir);
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
