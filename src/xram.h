/* RP6502 XRAM Mapping */

#ifndef XRAM_H
#define XRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Insert xram.h snippets from docs to build your system. */
/* https://picocomputer.github.io/sdk.html#xram-memory-map */

/* XREG-genvägar, hämtade från RIA- och VGA-dokumentationen */
#define xreg_ria_keyboard(...) xreg(0, 0, 0, __VA_ARGS__)
#define xreg_vga_canvas(...)   xreg(1, 0, 0, __VA_ARGS__)
#define xreg_vga_mode(...)     xreg(1, 0, 1, __VA_ARGS__)

typedef struct
{
    int foo;
} xram_feature_t; /* layout */

/* Insert XRAM structs from those snippets here along with */
/* your own custom usage to define your XRAM memory map. */
typedef struct
{
    xram_feature_t foo;
} xram_layout_t;

/* Define constants to be your XRAM addresses. */
#define XRAM_FOO offsetof(xram_layout_t, foo)

#endif
