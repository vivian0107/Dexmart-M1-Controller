#include "m1_diag_board.h"
#include "m1_motion.h"

#include <stdio.h>
#include <string.h>

static FDCAN_HandleTypeDef *bus;
static UART_HandleTypeDef *uart;
static M1_Motion diag;
static uint8_t rx_byte;
static volatile uint8_t input[256];
static volatile uint16_t head, tail;
static volatile uint8_t input_lost;
static uint32_t tx_at, tx_buffer;
static uint8_t tx_pending;


static void console(void *unused, const char *line) {
    (void)unused;
    (void)HAL_UART_Transmit(uart,(const uint8_t *)line,(uint16_t)strlen(line),100);
    (void)HAL_UART_Transmit(uart,(const uint8_t *)"\r\n",2,10);
}
static int transmit(void *unused, const M1_Frame *f) {
    FDCAN_TxHeaderTypeDef h={0};
    (void)unused;
    if(!m1_motion_frame_allowed(f) || HAL_FDCAN_GetTxFifoFreeLevel(bus)==0) return 0;
    h.Identifier=f->id; h.IdType=FDCAN_STANDARD_ID; h.TxFrameType=FDCAN_DATA_FRAME;
    h.DataLength=FDCAN_DLC_BYTES_8; h.ErrorStateIndicator=FDCAN_ESI_ACTIVE;
    h.BitRateSwitch=FDCAN_BRS_OFF; h.FDFormat=FDCAN_CLASSIC_CAN;
    h.TxEventFifoControl=FDCAN_NO_TX_EVENTS; h.MessageMarker=0;
    if(HAL_FDCAN_AddMessageToTxFifoQ(bus,&h,f->data)!=HAL_OK) return 0;
    tx_buffer=HAL_FDCAN_GetLatestTxFifoQRequestBuffer(bus);
    tx_at=HAL_GetTick(); tx_pending=1;
    return 1;
}
static void flush_bus(void *unused) {
    (void)unused;
    (void)HAL_FDCAN_AbortTxRequest(bus,FDCAN_TX_BUFFER0|FDCAN_TX_BUFFER1|FDCAN_TX_BUFFER2);
    tx_pending=0;
    while(HAL_FDCAN_GetRxFifoFillLevel(bus,FDCAN_RX_FIFO0)>0) {
        FDCAN_RxHeaderTypeDef h;
        uint8_t data[64];
        if(HAL_FDCAN_GetRxMessage(bus,FDCAN_RX_FIFO0,&h,data)!=HAL_OK) break;
    }
}
HAL_StatusTypeDef M1_DiagBoard_Init(FDCAN_HandleTypeDef *can, UART_HandleTypeDef *console_uart) {
    FDCAN_FilterTypeDef filter={0};
    bus=can; uart=console_uart;
    head=tail=0; input_lost=0; tx_pending=0;
    m1_motion_init(&diag,transmit,console,flush_bus,NULL);
    filter.IdType=FDCAN_STANDARD_ID; filter.FilterIndex=0;
    filter.FilterType=FDCAN_FILTER_RANGE; filter.FilterConfig=FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1=0x581; filter.FilterID2=0x5ff;
    if(HAL_FDCAN_ConfigFilter(bus,&filter)!=HAL_OK) return HAL_ERROR;
    filter.FilterIndex=1; filter.FilterID1=0x701; filter.FilterID2=0x77f;
    if(HAL_FDCAN_ConfigFilter(bus,&filter)!=HAL_OK) return HAL_ERROR;
    filter.FilterIndex=2; filter.FilterID1=0x81; filter.FilterID2=0xff;
    if(HAL_FDCAN_ConfigFilter(bus,&filter)!=HAL_OK) return HAL_ERROR;
    if(HAL_FDCAN_ConfigGlobalFilter(bus,FDCAN_REJECT,FDCAN_REJECT,FDCAN_REJECT_REMOTE,FDCAN_REJECT_REMOTE)!=HAL_OK ||
       HAL_FDCAN_Start(bus)!=HAL_OK) {
        console(NULL,"M1M1 ERROR reason=can_init_failed"); return HAL_ERROR;
    }
    /* RX interrupt keeps console input intact while serial logs are transmitted. */
    HAL_NVIC_SetPriority(LPUART1_IRQn,5,0); HAL_NVIC_EnableIRQ(LPUART1_IRQn);
    if(HAL_UART_Receive_IT(uart,&rx_byte,1)!=HAL_OK) return HAL_ERROR;
    m1_motion_transport(&diag,1);
    return HAL_OK;
}
void LPUART1_IRQHandler(void) { if(uart) HAL_UART_IRQHandler(uart); }
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *handle) {
    if(handle==uart) {
        uint16_t next=(uint16_t)((head+1u)&255u);
        if(next==tail) input_lost=1;
        else { input[head]=rx_byte; head=next; }
        if(HAL_UART_Receive_IT(uart,&rx_byte,1)!=HAL_OK) input_lost=1;
    }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *handle) {
    if(handle==uart) {
        input_lost=1;
        (void)HAL_UART_Receive_IT(uart,&rx_byte,1);
    }
}
void M1_DiagBoard_Poll(void) {
    FDCAN_ProtocolStatusTypeDef status={0};

    uint32_t now=HAL_GetTick();
    unsigned budget;
    if(input_lost) {
        uint32_t primask=__get_PRIMASK();
        __disable_irq(); tail=head; input_lost=0; __set_PRIMASK(primask);
        m1_motion_input_lost(&diag);
    }
    for(budget=0;budget<128 && tail!=head;++budget) {
        uint8_t byte=input[tail]; tail=(uint16_t)((tail+1u)&255u);
        m1_motion_input(&diag,byte);
    }
    for(budget=0;budget<32 && HAL_FDCAN_GetRxFifoFillLevel(bus,FDCAN_RX_FIFO0)>0;++budget) {
        FDCAN_RxHeaderTypeDef h;
        uint8_t bytes[64];
        M1_Frame f;
        if(HAL_FDCAN_GetRxMessage(bus,FDCAN_RX_FIFO0,&h,bytes)!=HAL_OK) break;
        if(h.IdType!=FDCAN_STANDARD_ID || h.RxFrameType!=FDCAN_DATA_FRAME ||
           h.FDFormat!=FDCAN_CLASSIC_CAN || h.DataLength>FDCAN_DLC_BYTES_8) continue;
        f.id=(uint16_t)h.Identifier; f.length=(uint8_t)h.DataLength;
        memset(f.data,0,sizeof(f.data)); memcpy(f.data,bytes,f.length);
        m1_motion_receive(&diag,&f,now);
    }
    m1_motion_transport(&diag,HAL_FDCAN_GetProtocolStatus(bus,&status)==HAL_OK && !status.BusOff);
    if(__HAL_FDCAN_GET_FLAG(bus,FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST)) {
        __HAL_FDCAN_CLEAR_FLAG(bus,FDCAN_FLAG_RX_FIFO0_MESSAGE_LOST);
        m1_motion_transport(&diag,0); /* Discard cached health after lost bus evidence. */
    }
    /* Discard an unacknowledged queued read rather than sending it much later. */
    if(tx_pending) {
        if(!HAL_FDCAN_IsTxBufferMessagePending(bus,tx_buffer)) tx_pending=0;
        else if(now-tx_at>=100) { (void)HAL_FDCAN_AbortTxRequest(bus,tx_buffer); tx_pending=0; }
    }
    m1_motion_tick(&diag,now);
}
