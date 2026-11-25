#ifndef M1_DIAG_BOARD_H
#define M1_DIAG_BOARD_H
#include "main.h"
HAL_StatusTypeDef M1_DiagBoard_Init(FDCAN_HandleTypeDef *can, UART_HandleTypeDef *console);
void M1_DiagBoard_Poll(void);
#endif
