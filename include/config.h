#ifndef COMMON_H
#define COMMON_H

#include "macros.h"

// Hard-coded configuration
#define COL_BITS 3 // 2^COL_BITS DSQs in each row
#define ROW_BITS 3 // 2^ROW_BITS rows of DSQs

#define DEFAULT_ROW 3 // Default row to place task
#define DEFAULT_COL 3 // Default column to place task


// 2^{COL_BITS + COL_BITS} Total DSQs in a grid
#define GRID_NR_ROWS POW2(ROW_BITS)
#define GRID_NR_COLS POW2(COL_BITS)
#define GRID_SIZE (GRID_NR_ROWS * GRID_NR_COLS) 

#if GRID_SIZE > 64
    #error GRID_NR_ROWS * GRID_NR_COLS exceeds allowed size (64)
#endif

#if DEFAULT_ROW < 0 || DEFAULT_ROW >= GRID_NR_ROWS
    #error DEFAULT_ROW out of range [0, GRID_NR_ROWS)
#endif

#if DEFAULT_COL < 0 || DEFAULT_COL >= GRID_NR_COLS
    #error DEFAULT_COL out of range [0, GRID_NR_COLS)
#endif

#endif