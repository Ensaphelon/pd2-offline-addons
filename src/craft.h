#ifndef PD2ADDONS_CRAFT_H
#define PD2ADDONS_CRAFT_H

#include <windows.h>

/* What the item menu (item_menu.c, the client) and the craft (craft.c, the server) say to each
   other. Offline both run in this process, so the server's answers are plain memory: the client
   asks with a packet — which puts the question on the server's thread, where the units may be
   read — and reads what came back on a later frame. */

#define CRAFT_ACTIONS 9          /* 1..7 the recipe families, 8 corruption, 9 selling */
#define CRAFT_ACTION_CORRUPT 8
#define CRAFT_ACTION_SELL 9

typedef struct {
    DWORD code;                  /* the ingredient's item code ("r10 ", "jewf", ...) */
    int have;                    /* how many there are to take: material tab and items together */
} craft_need;

typedef struct {
    int action;                  /* 1..8 */
    char family[16];             /* "blood" ... "corrupt" */
    int needs;
    craft_need need[3];
    BOOL possible;
    char why[48];                /* when not possible: what stops it */
    char effect[96];             /* what it is sure to add, short ("PDR 4-6%, ED 50-75%") */
} craft_option;

typedef struct {
    LONG seq;                    /* changes with every answer */
    DWORD item_id;
    int quality;                 /* ItemData +0: 4 magic, 6 rare, 8 crafted ... */
    int count;
    craft_option option[CRAFT_ACTIONS];
    char problem[96];            /* when nothing at all can be done right now */
} craft_info;

BOOL craft_ready(void);
void craft_query(DWORD item_id);
void craft_request(DWORD item_id, int action);
BOOL craft_info_get(craft_info *out);
/* The last craft's outcome, for a line on screen: FALSE when there is none newer than `seen`. */
BOOL craft_status_get(LONG *seen, char *out, size_t size);
/* A sale away from a vendor happened since `seen`: the client plays the vendor's sale sound,
   which at a vendor comes with the trade packet that no sale away from one has. */
BOOL craft_sale_sound_get(LONG *seen);

#endif
