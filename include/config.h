#ifndef COMMON_H
#define COMMON_H

// Hard-coded configuration
#define COL_BITS 3 // 2^COL_BITS DSQs in each row
#define ROW_BITS 3 // 2^ROW_BITS rows of DSQs
// 2^{COL_BITS + COL_BITS} Total DSQs in a grid
#define GRID_NR_ROWS (1 << ROW_BITS)
#define GRID_NR_COLS (1 << COL_BITS)
#define GRID_SIZE (GRID_ROWS * GRID_COLS) 

#endif