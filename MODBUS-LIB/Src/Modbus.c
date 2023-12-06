/*
 * Modbus.c
 *  Modbus RTU Master and Slave library for STM32 CUBE with FreeRTOS
 *  Created on: May 5, 2020
 *      Author: Alejandro Mera
 *      Adapted from https://github.com/smarmengol/Modbus-Master-Slave-for-Arduino
 */

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "task.h"
#include "queue.h"
#include "main.h"
#include "Modbus.h"
#include "timers.h"
#include "semphr.h"



#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))

#define lowByte(w) ((w) & 0xff)
#define highByte(w) ((w) >> 8)


modbusHandler_t *mHandlers[MAX_M_HANDLERS];


///Queue Modbus telegrams for master
const osMessageQueueAttr_t QueueTelegram_attributes = {
       .name = "QueueModbusTelegram"
};


const osThreadAttr_t myTaskModbusA_attributes = {
    .name = "TaskModbusSlave",
    .priority = (osPriority_t) osPriorityNormal,
    .stack_size = 128 * 4
};


//Task Modbus Master
//osThreadId_t myTaskModbusAHandle;
const osThreadAttr_t myTaskModbusB_attributes = {
    .name = "TaskModbusMaster",
    .priority = (osPriority_t) osPriorityNormal,
    .stack_size = 128 * 4
};


//Semaphore to access the Modbus Data
const osSemaphoreAttr_t ModBusSphr_attributes = {
    .name = "ModBusSphr"
};


uint8_t numberHandlers = 0;


static void sendTxBuffer(modbusHandler_t *modH);
static int16_t getRxBuffer(modbusHandler_t *modH);
static uint8_t validateAnswer(modbusHandler_t *modH);
static void buildException( uint8_t u8exception, modbusHandler_t *modH );
static uint8_t validateRequest(modbusHandler_t * modH);
static uint16_t word(uint8_t H, uint8_t l);
static void get_FC1(modbusHandler_t *modH);
static void get_FC3(modbusHandler_t *modH);
static int8_t process_FC1(modbusHandler_t *modH, uint8_t Database );
static int8_t process_FC3(modbusHandler_t *modH, uint8_t Database );
static int8_t process_FC5( modbusHandler_t *modH);
static int8_t process_FC6(modbusHandler_t *modH);
static int8_t process_FC15(modbusHandler_t *modH);
static int8_t process_FC16(modbusHandler_t *modH);
static void vTimerCallbackT35(TimerHandle_t *pxTimer);
static void vTimerCallbackTimeout(TimerHandle_t *pxTimer);
//static int16_t getRxBuffer(modbusHandler_t *modH);
static int8_t SendQuery(modbusHandler_t *modH ,  modbus_t telegram);


/* Ring Buffer functions */
// This function must be called only after disabling USART RX interrupt or inside of the RX interrupt
void RingAdd(modbusRingBuffer_t *xRingBuffer, uint8_t u8Val)
{

	xRingBuffer->uxBuffer[xRingBuffer->u8end] = u8Val;
	xRingBuffer->u8end = (xRingBuffer->u8end + 1) % MAX_BUFFER;
	if (xRingBuffer->u8available == MAX_BUFFER)
	{
		xRingBuffer->overflow = true;
		xRingBuffer->u8start = (xRingBuffer->u8start + 1) % MAX_BUFFER;
	}
	else
	{
		xRingBuffer->overflow = false;
		xRingBuffer->u8available++;
	}

}

// This function must be called only after disabling USART RX interrupt
uint8_t RingGetAllBytes(modbusRingBuffer_t *xRingBuffer, uint8_t *buffer)
{
	return RingGetNBytes(xRingBuffer, buffer, xRingBuffer->u8available);
}

// This function must be called only after disabling USART RX interrupt
uint8_t RingGetNBytes(modbusRingBuffer_t *xRingBuffer, uint8_t *buffer, uint8_t uNumber)
{
	uint8_t uCounter;
	if(xRingBuffer->u8available == 0  || uNumber == 0 ) return 0;
	if(uNumber > MAX_BUFFER) return 0;

	for(uCounter = 0; uCounter < uNumber && uCounter< xRingBuffer->u8available ; uCounter++)
	{
		buffer[uCounter] = xRingBuffer->uxBuffer[xRingBuffer->u8start];
		xRingBuffer->u8start = (xRingBuffer->u8start + 1) % MAX_BUFFER;
	}
	xRingBuffer->u8available = xRingBuffer->u8available - uCounter;
	xRingBuffer->overflow = false;
	RingClear(xRingBuffer);

	return uCounter;
}

uint8_t RingCountBytes(modbusRingBuffer_t *xRingBuffer)
{
return xRingBuffer->u8available;
}

void RingClear(modbusRingBuffer_t *xRingBuffer)
{
xRingBuffer->u8start = 0;
xRingBuffer->u8end = 0;
xRingBuffer->u8available = 0;
xRingBuffer->overflow = false;
}

/* End of Ring Buffer functions */



const unsigned char fctsupported[] =
{
    MB_FC_READ_COILS,
    MB_FC_READ_DISCRETE_INPUT,
    MB_FC_READ_REGISTERS,
    MB_FC_READ_INPUT_REGISTER,
    MB_FC_WRITE_COIL,
    MB_FC_WRITE_REGISTER,
    MB_FC_WRITE_MULTIPLE_COILS,
    MB_FC_WRITE_MULTIPLE_REGISTERS
};


/**
 * @brief
 * Initialization for a Master/Slave.
 * this function will check the configuration parameters
 * of the modbus handler
 *
 * @param modH   modbus handler
 */
void ModbusInit(modbusHandler_t * modH)
{

  if (numberHandlers < MAX_M_HANDLERS)
  {

	  //Initialize the ring buffer

	  RingClear(&modH->xBufferRX);

	  if(modH->uModbusType == MB_SLAVE)
	  {
		  //Create Modbus task slave
		  modH->myTaskModbusAHandle = osThreadNew(StartTaskModbusSlave, modH, &myTaskModbusA_attributes);
	  }
	  else if (modH->uModbusType == MB_MASTER)
	  {
		  //Create Modbus task Master  and Queue for telegrams
		  modH->myTaskModbusAHandle = osThreadNew(StartTaskModbusMaster, modH, &myTaskModbusB_attributes);


		  modH->xTimerTimeout=xTimerCreate("xTimerTimeout",  // Just a text name, not used by the kernel.
				  	  	modH->u16timeOut ,     		// The timer period in ticks.
						pdFALSE,         // The timers will auto-reload themselves when they expire.
						( void * )modH->xTimerTimeout,     // Assign each timer a unique id equal to its array index.
						(TimerCallbackFunction_t) vTimerCallbackTimeout  // Each timer calls the same callback when it expires.
                  	  	);

		  if(modH->xTimerTimeout == NULL)
		  {
			  while(1); //error creating timer, check heap and stack size
		  }


		  modH->QueueTelegramHandle = osMessageQueueNew (MAX_TELEGRAMS, sizeof(modbus_t), &QueueTelegram_attributes);

		  if(modH->QueueTelegramHandle == NULL)
		  {
			  while(1); //error creating queue for telegrams, check heap and stack size
		  }

	  }
	  else
	  {
		  while(1); //Error Modbus type not supported choose a valid Type
	  }

	  if  (modH->myTaskModbusAHandle == NULL)
	  {
		  while(1); //Error creating Modbus task, check heap and stack size
	  }


	  modH->xTimerT35 = xTimerCreate("TimerT35",         // Just a text name, not used by the kernel.
		  	  	  	  	  	  	  	T35 ,     // The timer period in ticks.
                                    pdFALSE,         // The timers will auto-reload themselves when they expire.
									( void * )modH->xTimerT35,     // Assign each timer a unique id equal to its array index.
                                    (TimerCallbackFunction_t) vTimerCallbackT35     // Each timer calls the same callback when it expires.
                                    );
	  if (modH->xTimerT35 == NULL)
	  {
		  while(1); //Error creating the timer, check heap and stack size
	  }


	  modH->ModBusSphrHandle = osSemaphoreNew(1, 1, &ModBusSphr_attributes);

	  if(modH->ModBusSphrHandle == NULL)
	  {
		  while(1); //Error creating the semaphore, check heap and stack size
	  }

	  mHandlers[numberHandlers] = modH;
	  numberHandlers++;
  }
  else
  {
	  while(1); //error no more Modbus handlers supported
  }

}

/**
 * @brief
 * Start object.
 *
 * Call this AFTER calling begin() on the serial port, typically within setup().
 *
 * (If you call this function, then you should NOT call any of
 * ModbusRtu's own begin() functions.)
 *
 * @ingroup setup
 */
void ModbusStart(modbusHandler_t * modH)
{

	if(modH->xTypeHW != USART_HW && modH->xTypeHW != USART_HW_DMA )
	{

		while(1); //ERROR select the type of hardware
	}

	if (modH->xTypeHW == USART_HW_DMA && ENABLE_USART_DMA == 0  )
	{
		while(1); //ERROR To use USART_HW_DMA you need to enable it in the ModbusConfig.h file
	}



	if (modH->xTypeHW == USART_HW || modH->xTypeHW ==  USART_HW_DMA )
	{

	      if (modH->EN_Port != NULL )
          {
              // return RS485 transceiver to transmit mode
          	HAL_GPIO_WritePin(modH->EN_Port, modH->EN_Pin, GPIO_PIN_RESET);
          }

          if (modH->uModbusType == MB_SLAVE &&  modH->u16regsHR == NULL )
          {
          	while(1); //ERROR define the DATA pointer shared through Modbus
          }

          //check that port is initialized
          while (HAL_UART_GetState(modH->port) != HAL_UART_STATE_READY)
          {

          }

#if ENABLE_USART_DMA ==1
          if( modH->xTypeHW == USART_HW_DMA )
          {


        	  if(HAL_UARTEx_ReceiveToIdle_DMA(modH->port, modH->xBufferRX.uxBuffer, MAX_BUFFER ) != HAL_OK)
        	   {
        	         while(1)
        	         {
        	                    	  //error in your initialization code
        	         }
        	   }
        	  __HAL_DMA_DISABLE_IT(modH->port->hdmarx, DMA_IT_HT); // we don't need half-transfer interrupt

          }
          else{

        	  // Receive data from serial port for Modbus using interrupt
        	  if(HAL_UART_Receive_IT(modH->port, &modH->dataRX, 1) != HAL_OK)
        	  {
        	           while(1)
        	           {
        	                       	  //error in your initialization code
        	           }
        	  }
          }

#else
          	  // Receive data from serial port for Modbus using interrupt
          	  if(HAL_UART_Receive_IT(modH->port, &modH->dataRX, 1) != HAL_OK)
          	  {
          		  while(1)
          		  {
          			  //error in your initialization code
          		  }
          	  }
#endif

          if(modH->u8id !=0 && modH->uModbusType == MB_MASTER )
          {
        	  while(1)
        	  {
        	     	  //error Master ID must be zero
        	  }
          }

          if(modH->u8id ==0 && modH->uModbusType == MB_SLAVE )
          {
             	  while(1)
               	  {
                  	     	  //error Master ID must be zero
               	  }
           }
	}

    modH->u8lastRec = modH->u8BufferSize = 0;
    modH->u16InCnt = modH->u16OutCnt = modH->u16errCnt = 0;

}

void vTimerCallbackT35(TimerHandle_t *pxTimer)
{
	//Notify that a stream has just arrived
	int i;
	//TimerHandle_t aux;
	for(i = 0; i < numberHandlers; i++)
	{

		if( (TimerHandle_t *)mHandlers[i]->xTimerT35 ==  pxTimer ){
			if(mHandlers[i]->uModbusType == MB_MASTER)
			{
				xTimerStop(mHandlers[i]->xTimerTimeout,0);
			}
			xTaskNotify(mHandlers[i]->myTaskModbusAHandle, 0, eSetValueWithOverwrite);
		}

	}
}

void vTimerCallbackTimeout(TimerHandle_t *pxTimer)
{
	//Notify that a stream has just arrived
	int i;
	//TimerHandle_t aux;
	for(i = 0; i < numberHandlers; i++)
	{

		if( (TimerHandle_t *)mHandlers[i]->xTimerTimeout ==  pxTimer ){
				xTaskNotify(mHandlers[i]->myTaskModbusAHandle, ERR_TIME_OUT, eSetValueWithOverwrite);
		}

	}

}


void StartTaskModbusSlave(void *argument)
{

  modbusHandler_t *modH =  (modbusHandler_t *)argument;
  //uint32_t notification;
  for(;;)
  {

	modH->i8lastError = 0;


   if(modH->xTypeHW == USART_HW || modH->xTypeHW == USART_HW_DMA)
   {

	  ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* Block until a Modbus Frame arrives */

	  if (getRxBuffer(modH) == ERR_BUFF_OVERFLOW)
	  {
	      modH->i8lastError = ERR_BUFF_OVERFLOW;
	   	  modH->u16errCnt++;
		  continue;
	  }
   }

   if (modH->u8BufferSize < 7)
   {
      //The size of the frame is invalid
      modH->i8lastError = ERR_BAD_SIZE;
      modH->u16errCnt++;

	  continue;
    }


   // check slave id
    if ( modH->u8Buffer[ID] !=  modH->u8id)
	{
    	continue;
	}

	// validate message: CRC, FCT, address and size
    uint8_t u8exception = validateRequest(modH);
	if (u8exception > 0)
	{
	    if (u8exception != ERR_TIME_OUT)
		{
		    buildException( u8exception, modH);
			sendTxBuffer(modH);
		}
		modH->i8lastError = u8exception;
		//return u8exception

		continue;
	 }

	 modH->i8lastError = 0;
	xSemaphoreTake(modH->ModBusSphrHandle , portMAX_DELAY); //before processing the message get the semaphore

	 // process message
	 switch(modH->u8Buffer[ FUNC ] )
	 {
			case MB_FC_READ_COILS:
				modH->i8state = process_FC1(modH,DB_COILS);
				break;
			case MB_FC_READ_DISCRETE_INPUT:
				modH->i8state = process_FC1(modH,DB_INPUT_COILS);
				break;
			case MB_FC_READ_REGISTERS:
				modH->i8state = process_FC3(modH,DB_HOLDING_REGISTER);
				break;
			case MB_FC_READ_INPUT_REGISTER:
				modH->i8state = process_FC3(modH,DB_INPUT_REGISTERS);
				break;
			case MB_FC_WRITE_COIL:
				modH->i8state = process_FC5(modH);
				break;
			case MB_FC_WRITE_REGISTER :
				modH->i8state = process_FC6(modH);
				break;
			case MB_FC_WRITE_MULTIPLE_COILS:
				modH->i8state = process_FC15(modH);
				break;
			case MB_FC_WRITE_MULTIPLE_REGISTERS :
				modH->i8state = process_FC16(modH);
				break;
			default:
				break;
	 }


	 xSemaphoreGive(modH->ModBusSphrHandle); //Release the semaphore

	 continue;

   }

}



void ModbusQuery(modbusHandler_t * modH, modbus_t telegram )
{
	//Add the telegram to the TX tail Queue of Modbus
	if (modH->uModbusType == MB_MASTER)
	{
	telegram.u32CurrentTask = (uint32_t *) osThreadGetId();
	xQueueSendToBack(modH->QueueTelegramHandle, &telegram, 0);
	}
	else{
		while(1);// error a slave cannot send queries as a master
	}
}



void ModbusQueryInject(modbusHandler_t * modH, modbus_t telegram )
{
	//Add the telegram to the TX head Queue of Modbus
	xQueueReset(modH->QueueTelegramHandle);
	telegram.u32CurrentTask = (uint32_t *) osThreadGetId();
	xQueueSendToFront(modH->QueueTelegramHandle, &telegram, 0);
}


/**
 * @brief
 * *** Only Modbus Master ***
 * Generate a query to an slave with a modbus_t telegram structure
 * The Master must be in COM_IDLE mode. After it, its state would be COM_WAITING.
 * This method has to be called only in loop() section.
 *
 * @see modbus_t
 * @param modH  modbus handler
 * @param modbus_t  modbus telegram structure (id, fct, ...)
 * @ingroup loop
 */
int8_t SendQuery(modbusHandler_t *modH ,  modbus_t telegram )
{


	uint8_t u8regsno, u8bytesno;
	uint8_t  error = 0;
	xSemaphoreTake(modH->ModBusSphrHandle , portMAX_DELAY); //before processing the message get the semaphore

	if (modH->u8id!=0) error = ERR_NOT_MASTER;
	if (modH->i8state != COM_IDLE) error = ERR_POLLING ;
	if ((telegram.u8id==0) || (telegram.u8id>247)) error = ERR_BAD_SLAVE_ID;


	if(error)
	{
		 modH->i8lastError = error;
		 xSemaphoreGive(modH->ModBusSphrHandle);
		 return error;
	}


	if (telegram.u8fct == MB_FC_READ_COILS || telegram.u8fct == MB_FC_READ_DISCRETE_INPUT ||
		telegram.u8fct == MB_FC_WRITE_COIL || telegram.u8fct == MB_FC_WRITE_MULTIPLE_COILS)
	{
		modH->u16regsCoils = telegram.u16reg;
	}
	else if (telegram.u8fct == MB_FC_READ_REGISTERS || telegram.u8fct == MB_FC_READ_INPUT_REGISTER ||
			telegram.u8fct == MB_FC_WRITE_REGISTER || telegram.u8fct == MB_FC_WRITE_MULTIPLE_REGISTERS)
	{
		modH->u16regsHR = telegram.u16reg;
	}

	// telegram header
	modH->u8Buffer[ ID ]         = telegram.u8id;
	modH->u8Buffer[ FUNC ]       = telegram.u8fct;
	modH->u8Buffer[ ADD_HI ]     = highByte(telegram.u16RegAdd );
	modH->u8Buffer[ ADD_LO ]     = lowByte( telegram.u16RegAdd );

	switch( telegram.u8fct )
	{
	case MB_FC_READ_COILS:
	case MB_FC_READ_DISCRETE_INPUT:
	case MB_FC_READ_REGISTERS:
	case MB_FC_READ_INPUT_REGISTER:
	    modH->u8Buffer[ NB_HI ]      = highByte(telegram.u16CoilsNo );
	    modH->u8Buffer[ NB_LO ]      = lowByte( telegram.u16CoilsNo );
	    modH->u8BufferSize = 6;
	    break;
	case MB_FC_WRITE_COIL:
	    modH->u8Buffer[ NB_HI ]      = (( telegram.u16reg[0]> 0) ? 0xff : 0);
	    modH->u8Buffer[ NB_LO ]      = 0;
	    modH->u8BufferSize = 6;
	    break;
	case MB_FC_WRITE_REGISTER:
	    modH->u8Buffer[ NB_HI ]      = highByte( telegram.u16reg[0]);
	    modH->u8Buffer[ NB_LO ]      = lowByte( telegram.u16reg[0]);
	    modH->u8BufferSize = 6;
	    break;
	case MB_FC_WRITE_MULTIPLE_COILS: // TODO: implement "sending coils"
	    u8regsno = telegram.u16CoilsNo / 16;
	    u8bytesno = u8regsno * 2;
	    if ((telegram.u16CoilsNo % 16) != 0)
	    {
	        u8bytesno++;
	        u8regsno++;
	    }

	    modH->u8Buffer[ NB_HI ]      = highByte(telegram.u16CoilsNo );
	    modH->u8Buffer[ NB_LO ]      = lowByte( telegram.u16CoilsNo );
	    modH->u8Buffer[ BYTE_CNT ]    = u8bytesno;
	    modH->u8BufferSize = 7;

	    for (uint16_t i = 0; i < u8bytesno; i++)
	    {
	        if(i%2)
	        {
	        	modH->u8Buffer[ modH->u8BufferSize ] = lowByte( telegram.u16reg[ i/2 ] );
	        }
	        else
	        {
	        	modH->u8Buffer[  modH->u8BufferSize ] = highByte( telegram.u16reg[ i/2 ] );

	        }
	        modH->u8BufferSize++;
	    }
	    break;

	case MB_FC_WRITE_MULTIPLE_REGISTERS:
	    modH->u8Buffer[ NB_HI ]      = highByte(telegram.u16CoilsNo );
	    modH->u8Buffer[ NB_LO ]      = lowByte( telegram.u16CoilsNo );
	    modH->u8Buffer[ BYTE_CNT ]    = (uint8_t) ( telegram.u16CoilsNo * 2 );
	    modH->u8BufferSize = 7;

	    for (uint16_t i=0; i< telegram.u16CoilsNo; i++)
	    {

	        modH->u8Buffer[  modH->u8BufferSize ] = highByte(  telegram.u16reg[ i ] );
	        modH->u8BufferSize++;
	        modH->u8Buffer[  modH->u8BufferSize ] = lowByte( telegram.u16reg[ i ] );
	        modH->u8BufferSize++;
	    }
	    break;
	}


	sendTxBuffer(modH);

	xSemaphoreGive(modH->ModBusSphrHandle);

	modH->i8state = COM_WAITING;
	modH->i8lastError = 0;
	return 0;


}


void StartTaskModbusMaster(void *argument)
{

  modbusHandler_t *modH =  (modbusHandler_t *)argument;
  uint32_t ulNotificationValue;
  modbus_t telegram;



  for(;;)
  {
	  /*Wait indefinitely for a telegram to send */
	  xQueueReceive(modH->QueueTelegramHandle, &telegram, portMAX_DELAY);

     // This is the case for implementations with only USART support
     SendQuery(modH, telegram);
     /* Block indefinitely until a Modbus Frame arrives or query timeouts*/
     ulNotificationValue = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

	  // notify the task the request timeout
      modH->i8lastError = 0;
      if(ulNotificationValue)
      {
    	  modH->i8state = COM_IDLE;
    	  modH->i8lastError = ERR_TIME_OUT;
    	  modH->u16errCnt++;
    	  xTaskNotify((TaskHandle_t)telegram.u32CurrentTask, modH->i8lastError, eSetValueWithOverwrite);
    	  continue;
      }

      getRxBuffer(modH);

	  if ( modH->u8BufferSize < 6){

		  modH->i8state = COM_IDLE;
		  modH->i8lastError = ERR_BAD_SIZE;
		  modH->u16errCnt++;
		  xTaskNotify((TaskHandle_t)telegram.u32CurrentTask, modH->i8lastError, eSetValueWithOverwrite);
		  continue;
	  }

	  xTimerStop(modH->xTimerTimeout,0); // cancel timeout timer


	  // validate message: id, CRC, FCT, exception
	  int8_t u8exception = validateAnswer(modH);
	  if (u8exception != 0)
	  {
		 modH->i8state = COM_IDLE;
         modH->i8lastError = u8exception;
		 xTaskNotify((TaskHandle_t)telegram.u32CurrentTask, modH->i8lastError, eSetValueWithOverwrite);
	     continue;
	  }

	  modH->i8lastError = u8exception;

	  xSemaphoreTake(modH->ModBusSphrHandle , portMAX_DELAY); //before processing the message get the semaphore
	  // process answer
	  switch( modH->u8Buffer[ FUNC ] )
	  {
	  case MB_FC_READ_COILS:
	  case MB_FC_READ_DISCRETE_INPUT:
	      //call get_FC1 to transfer the incoming message to u16regs buffer
	      get_FC1(modH);
	      break;
	  case MB_FC_READ_INPUT_REGISTER:
	  case MB_FC_READ_REGISTERS :
	      // call get_FC3 to transfer the incoming message to u16regs buffer
	      get_FC3(modH);
	      break;
	  case MB_FC_WRITE_COIL:
	  case MB_FC_WRITE_REGISTER :
	  case MB_FC_WRITE_MULTIPLE_COILS:
	  case MB_FC_WRITE_MULTIPLE_REGISTERS :
	      // nothing to do
	      break;
	  default:
	      break;
	  }
	  modH->i8state = COM_IDLE;

	  if (modH->i8lastError ==0) // no error the error_OK, we need to use a different value than 0 to detect the timeout
	  {
		  xSemaphoreGive(modH->ModBusSphrHandle); //Release the semaphore
		  xTaskNotify((TaskHandle_t)telegram.u32CurrentTask, ERR_OK_QUERY, eSetValueWithOverwrite);
	  }


	  continue;
	 }

}

/**
 * This method processes functions 1 & 2 (for master)
 * This method puts the slave answer into master data buffer
 *
 * @ingroup register
 */
void get_FC1(modbusHandler_t *modH)
{
    uint8_t u8byte, i;
    u8byte = 3;
     for (i=0; i< modH->u8Buffer[2]; i++) {

        if(i%2)
        {
        	modH->u16regsCoils[i/2]= word(modH->u8Buffer[i+u8byte], lowByte(modH->u16regsCoils[i/2]));
        }
        else
        {

        	modH->u16regsCoils[i/2]= word(highByte(modH->u16regsCoils[i/2]), modH->u8Buffer[i+u8byte]);
        }

     }
}

/**
 * This method processes functions 3 & 4 (for master)
 * This method puts the slave answer into master data buffer
 *
 * @ingroup register
 */
void get_FC3(modbusHandler_t *modH)
{
    uint8_t u8byte, i;
    u8byte = 3;

    for (i=0; i< modH->u8Buffer[ 2 ] /2; i++)
    {
    	modH->u16regsHR[ i ] = word(modH->u8Buffer[ u8byte ], modH->u8Buffer[ u8byte +1 ]);
        u8byte += 2;
    }
}



/**
 * @brief
 * This method validates master incoming messages
 *
 * @return 0 if OK, EXCEPTION if anything fails
 * @ingroup buffer
 */
uint8_t validateAnswer(modbusHandler_t *modH)
{
    // check message crc vs calculated crc

	uint16_t u16MsgCRC =
        ((modH->u8Buffer[modH->u8BufferSize - 2] << 8)
         | modH->u8Buffer[modH->u8BufferSize - 1]); // combine the crc Low & High bytes
    if ( calcCRC(modH->u8Buffer,  modH->u8BufferSize-2) != u16MsgCRC )
    {
    	modH->u16errCnt ++;
        return ERR_BAD_CRC;
    }

    // check exception
    if ((modH->u8Buffer[ FUNC ] & 0x80) != 0)
    {
    	modH->u16errCnt ++;
        return ERR_EXCEPTION;
    }

    // check fct code
    bool isSupported = false;
    for (uint8_t i = 0; i< sizeof( fctsupported ); i++)
    {
        if (fctsupported[i] == modH->u8Buffer[FUNC])
        {
            isSupported = 1;
            break;
        }
    }
    if (!isSupported)
    {
    	modH->u16errCnt ++;
        return EXC_FUNC_CODE;
    }

    return 0; // OK, no exception code thrown
}


/**
 * @brief
 * This method moves Serial buffer data to the Modbus u8Buffer.
 *
 * @return buffer size if OK, ERR_BUFF_OVERFLOW if u8BufferSize >= MAX_BUFFER
 * @ingroup buffer
 */
int16_t getRxBuffer(modbusHandler_t *modH)
{

    int16_t i16result;

    if(modH->xTypeHW == USART_HW)
    {
    	HAL_UART_AbortReceive_IT(modH->port); // disable interrupts to avoid race conditions on serial port
    }

	if (modH->xBufferRX.overflow)
    {
       	RingClear(&modH->xBufferRX); // clean up the overflowed buffer
       	i16result =  ERR_BUFF_OVERFLOW;
    }
	else
	{
		modH->u8BufferSize = RingGetAllBytes(&modH->xBufferRX, modH->u8Buffer);
		modH->u16InCnt++;
		i16result = modH->u8BufferSize;
	}

	if(modH->xTypeHW == USART_HW)
	{
		HAL_UART_Receive_IT(modH->port, &modH->dataRX, 1);
	}

    return i16result;
}





/**
 * @brief
 * This method validates slave incoming messages
 *
 * @return 0 if OK, EXCEPTION if anything fails
 * @ingroup modH Modbus handler
 */
uint8_t validateRequest(modbusHandler_t *modH)
{
	// check message crc vs calculated crc

	    uint16_t u16MsgCRC;
	    u16MsgCRC= ((modH->u8Buffer[modH->u8BufferSize - 2] << 8)
	    		   	         | modH->u8Buffer[modH->u8BufferSize - 1]); // combine the crc Low & High bytes


	    if ( calcCRC( modH->u8Buffer,  modH->u8BufferSize-2 ) != u16MsgCRC )
	    {
	       		modH->u16errCnt ++;
	       		return ERR_BAD_CRC;
	    }



	    // check fct code
	    bool isSupported = false;
	    for (uint8_t i = 0; i< sizeof( fctsupported ); i++)
	    {
	        if (fctsupported[i] == modH->u8Buffer[FUNC])
	        {
	            isSupported = 1;
	            break;
	        }
	    }
	    if (!isSupported)
	    {
	    	modH->u16errCnt ++;
	        return EXC_FUNC_CODE;
	    }

	    // check start address & nb range
	    uint16_t u16AdRegs = 0;
	    uint16_t u16NRegs = 0;

	    //uint8_t u8regs;
	    switch ( modH->u8Buffer[ FUNC ] )
	    {
	    case MB_FC_READ_COILS:
	    case MB_FC_READ_DISCRETE_INPUT:
	    case MB_FC_WRITE_MULTIPLE_COILS:
	    	u16AdRegs = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ]) / 16;
	    	u16NRegs = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ]) /16;
	    	if(word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ]) % 16) u16NRegs++; // check for incomplete words
	    	// verify address range
	    	if((u16AdRegs + u16NRegs) > modH->u16regCoils_size) return EXC_ADDR_RANGE;

	    	//verify answer frame size in bytes

	    	u16NRegs = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ]) / 8;
	    	if(word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ]) % 8) u16NRegs++;
	    	u16NRegs = u16NRegs + 5; // adding the header  and CRC ( Slave address + Function code  + number of data bytes to follow + 2-byte CRC )
	        if(u16NRegs > 256) return EXC_REGS_QUANT;

	        break;
	    case MB_FC_WRITE_COIL:
	    	u16AdRegs = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ]) / 16;
	    	if(word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ]) % 16) u16AdRegs++;	// check for incomplete words
	        if (u16AdRegs > modH->u16regCoils_size) return EXC_ADDR_RANGE;
	        break;
	    case MB_FC_WRITE_REGISTER :
	    	u16AdRegs = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ]);
	        if (u16AdRegs > modH-> u16regHR_size) return EXC_ADDR_RANGE;
	        break;
	    case MB_FC_READ_REGISTERS :
	    case MB_FC_READ_INPUT_REGISTER :
	    case MB_FC_WRITE_MULTIPLE_REGISTERS :
	    	u16AdRegs = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ]);
	        u16NRegs = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ]);
	        if (( u16AdRegs + u16NRegs ) > modH->u16regHR_size) return EXC_ADDR_RANGE;

	        //verify answer frame size in bytes
	        u16NRegs = u16NRegs*2 + 5; // adding the header  and CRC
	        if ( u16NRegs > 256 ) return EXC_REGS_QUANT;
	        break;
	    }
	    return 0; // OK, no exception code thrown

}

/**
 * @brief
 * This method creates a word from 2 bytes
 *
 * @return uint16_t (word)
 * @ingroup H  Most significant byte
 * @ingroup L  Less significant byte
 */
uint16_t word(uint8_t H, uint8_t L)
{
	bytesFields W;
	W.u8[0] = L;
	W.u8[1] = H;

	return W.u16[0];
}


/**
 * @brief
 * This method calculates CRC
 *
 * @return uint16_t calculated CRC value for the message
 * @ingroup Buffer
 * @ingroup u8length
 */
uint16_t calcCRC(uint8_t *Buffer, uint8_t u8length)
{
    unsigned int temp, temp2, flag;
    temp = 0xFFFF;
    for (unsigned char i = 0; i < u8length; i++)
    {
        temp = temp ^ Buffer[i];
        for (unsigned char j = 1; j <= 8; j++)
        {
            flag = temp & 0x0001;
            temp >>=1;
            if (flag)
                temp ^= 0xA001;
        }
    }
    // Reverse byte order.
    temp2 = temp >> 8;
    temp = (temp << 8) | temp2;
    temp &= 0xFFFF;
    // the returned value is already swapped
    // crcLo byte is first & crcHi byte is last
    return temp;

}


/**
 * @brief
 * This method builds an exception message
 *
 * @ingroup u8exception exception number
 * @ingroup modH modbus handler
 */
void buildException( uint8_t u8exception, modbusHandler_t *modH )
{
    uint8_t u8func = modH->u8Buffer[ FUNC ];  // get the original FUNC code

    modH->u8Buffer[ ID ]      = modH->u8id;
    modH->u8Buffer[ FUNC ]    = u8func + 0x80;
    modH->u8Buffer[ 2 ]       = u8exception;
    modH->u8BufferSize         = EXCEPTION_SIZE;
}


/**
 * @brief
 * This method transmits u8Buffer to Serial line.
 * Only if u8txenpin != 0, there is a flow handling in order to keep
 * the RS485 transceiver in output state as long as the message is being sent.
 * This is done with TC bit.
 * The CRC is appended to the buffer before starting to send it.
 *
 * @return nothing
 * @ingroup modH Modbus handler
 */
static void sendTxBuffer(modbusHandler_t *modH)
{
    // append CRC to message
	uint16_t u16crc = calcCRC(modH->u8Buffer, modH->u8BufferSize);
    modH->u8Buffer[ modH->u8BufferSize ] = u16crc >> 8;
    modH->u8BufferSize++;
    modH->u8Buffer[ modH->u8BufferSize ] = u16crc & 0x00ff;
    modH->u8BufferSize++;


    	if (modH->EN_Port != NULL)
        {
    		//enable transmitter, disable receiver to avoid echo on RS485 transceivers
    		HAL_HalfDuplex_EnableTransmitter(modH->port);
    		HAL_GPIO_WritePin(modH->EN_Port, modH->EN_Pin, GPIO_PIN_SET);
        }

#if ENABLE_USART_DMA ==1
    	if(modH->xTypeHW == USART_HW)
    	{
#endif
    		// transfer buffer to serial line IT
    		HAL_UART_Transmit_IT(modH->port, modH->u8Buffer,  modH->u8BufferSize);

#if ENABLE_USART_DMA ==1
    	}
        else
        {
        	//transfer buffer to serial line DMA
        	HAL_UART_Transmit_DMA(modH->port, modH->u8Buffer, modH->u8BufferSize);

        }
#endif

        ulTaskNotifyTake(pdTRUE, 250); //wait notification from TXE interrupt
/*
* If you are porting the library to a different MCU check the 
* USART datasheet and add the corresponding family in the following
* preprocessor conditions
*/
#if defined(STM32H7)  || defined(STM32F3) || defined(STM32L4) || defined(STM32L082xx) || defined(STM32F7) || defined(STM32WB)
          while((modH->port->Instance->ISR & USART_ISR_TC) ==0 )
#else
          // F429, F103, L152 ...
	  while((modH->port->Instance->SR & USART_SR_TC) ==0 )
#endif
         {
 	        //block the task until the the last byte is send out of the shifting buffer in USART
         }


         if (modH->EN_Port != NULL)
         {

             //return RS485 transceiver to receive mode
        	 HAL_GPIO_WritePin(modH->EN_Port, modH->EN_Pin, GPIO_PIN_RESET);
        	 //enable receiver, disable transmitter
        	 HAL_HalfDuplex_EnableReceiver(modH->port);

         }

         // set timeout for master query
         if(modH->uModbusType == MB_MASTER )
         {
        	 xTimerReset(modH->xTimerTimeout,0);
         }

     modH->u8BufferSize = 0;
     // increase message counter
     modH->u16OutCnt++;


}


/**
 * @brief
 * This method processes functions 1 & 2
 * This method reads a bit array and transfers it to the master
 *
 * @return u8BufferSize Response to master length
 * @ingroup discrete
 */
int8_t process_FC1(modbusHandler_t *modH, uint8_t Database)
{
    uint16_t u16currentRegister;
    uint8_t u8currentBit, u8bytesno, u8bitsno;
    uint8_t u8CopyBufferSize;
    uint16_t u16currentCoil, u16coil;

    uint16_t *u16regs;

    // get the first and last coil from the message
    uint16_t u16StartCoil = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ] );
    uint16_t u16Coilno = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ] );

    // put the number of bytes in the outcoming message
    u8bytesno = (uint8_t) (u16Coilno / 8);
    if (u16Coilno % 8 != 0) u8bytesno ++;
    modH->u8Buffer[ ADD_HI ]  = u8bytesno;
    modH->u8BufferSize         = ADD_LO;
    modH->u8Buffer[modH->u8BufferSize + u8bytesno - 1 ] = 0;

    // read each coil from the register map and put its value inside the outcoming message
    u8bitsno = 0;

    if (Database == 1){
    	u16regs = modH->u16regsCoils;
    }


    for (u16currentCoil = 0; u16currentCoil < u16Coilno; u16currentCoil++)
    {
        u16coil = u16StartCoil + u16currentCoil;
        u16currentRegister =  (u16coil / 16);
        u8currentBit = (uint8_t) (u16coil % 16);

        bitWrite(
        	modH->u8Buffer[ modH->u8BufferSize ],
            u8bitsno,
		    bitRead( u16regs[ u16currentRegister ], u8currentBit ) );
        u8bitsno ++;

        if (u8bitsno > 7)
        {
            u8bitsno = 0;
            modH->u8BufferSize++;
        }
    }

    // send outcoming message
    if (u16Coilno % 8 != 0) modH->u8BufferSize ++;
    u8CopyBufferSize = modH->u8BufferSize +2;
    sendTxBuffer(modH);
    return u8CopyBufferSize;
}


/**
 * @brief
 * This method processes functions 3 & 4
 * This method reads a word array and transfers it to the master
 *
 * @return u8BufferSize Response to master length
 * @ingroup register
 */
int8_t process_FC3(modbusHandler_t *modH, uint8_t Database)
{

    uint16_t u16StartAdd = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ] );
    uint8_t u8regsno = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ] );
    uint8_t u8CopyBufferSize;
    uint16_t i;

    uint16_t *u16regs;

    modH->u8Buffer[ 2 ]       = u8regsno * 2;
    modH->u8BufferSize         = 3;

    if (Database == DB_HOLDING_REGISTER)
    {
    	u16regs = modH->u16regsHR;
    }
    else if (Database == DB_INPUT_REGISTERS)
    {
    	u16regs = modH->u16regsRO;
    }

    for (i = u16StartAdd; i < u16StartAdd + u8regsno; i++)
    {
    	modH->u8Buffer[ modH->u8BufferSize ] = highByte(u16regs[i]);
    	modH->u8BufferSize++;
    	modH->u8Buffer[ modH->u8BufferSize ] = lowByte(u16regs[i]);
    	modH->u8BufferSize++;
    }
    u8CopyBufferSize = modH->u8BufferSize +2;
    sendTxBuffer(modH);

    return u8CopyBufferSize;
}

/**
 * @brief
 * This method processes function 5
 * This method writes a value assigned by the master to a single bit
 *
 * @return u8BufferSize Response to master length
 * @ingroup discrete
 */
int8_t process_FC5( modbusHandler_t *modH )
{
    uint8_t u8currentBit;
    uint16_t u16currentRegister;
    uint8_t u8CopyBufferSize;
    uint16_t u16coil = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ] );

    // point to the register and its bit
    u16currentRegister = (u16coil / 16);
    u8currentBit = (uint8_t) (u16coil % 16);

    // write to coil
    bitWrite(
    	modH->u16regsCoils[ u16currentRegister ],
        u8currentBit,
		modH->u8Buffer[ NB_HI ] == 0xff );


    // send answer to master
    modH->u8BufferSize = 6;
    u8CopyBufferSize =  modH->u8BufferSize +2;
    sendTxBuffer(modH);

    return u8CopyBufferSize;
}

/**
 * @brief
 * This method processes function 6
 * This method writes a value assigned by the master to a single word
 *
 * @return u8BufferSize Response to master length
 * @ingroup register
 */
int8_t process_FC6(modbusHandler_t *modH)
{

    uint16_t u16add = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ] );
    uint8_t u8CopyBufferSize;
    uint16_t u16val = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ] );

    modH->u16regsHR[ u16add ] = u16val;

    // keep the same header
    modH->u8BufferSize = RESPONSE_SIZE;

    u8CopyBufferSize = modH->u8BufferSize + 2;
    sendTxBuffer(modH);

    return u8CopyBufferSize;
}

/**
 * @brief
 * This method processes function 15
 * This method writes a bit array assigned by the master
 *
 * @return u8BufferSize Response to master length
 * @ingroup discrete
 */
int8_t process_FC15( modbusHandler_t *modH )
{
    uint8_t u8currentBit, u8frameByte, u8bitsno;
    uint16_t u16currentRegister;
    uint8_t u8CopyBufferSize;
    uint16_t u16currentCoil, u16coil;
    bool bTemp;

    // get the first and last coil from the message
    uint16_t u16StartCoil = word( modH->u8Buffer[ ADD_HI ], modH->u8Buffer[ ADD_LO ] );
    uint16_t u16Coilno = word( modH->u8Buffer[ NB_HI ], modH->u8Buffer[ NB_LO ] );


    // read each coil from the register map and put its value inside the outcoming message
    u8bitsno = 0;
    u8frameByte = 7;
    for (u16currentCoil = 0; u16currentCoil < u16Coilno; u16currentCoil++)
    {

        u16coil = u16StartCoil + u16currentCoil;
        u16currentRegister = (u16coil / 16);
        u8currentBit = (uint8_t) (u16coil % 16);

        bTemp = bitRead(
        			modH->u8Buffer[ u8frameByte ],
                    u8bitsno );

        bitWrite(
            modH->u16regsCoils[ u16currentRegister ],
            u8currentBit,
            bTemp );

        u8bitsno ++;

        if (u8bitsno > 7)
        {
            u8bitsno = 0;
            u8frameByte++;
        }
    }

    // send outcoming message
    // it's just a copy of the incomping frame until 6th byte
    modH->u8BufferSize         = 6;
    u8CopyBufferSize = modH->u8BufferSize +2;
    sendTxBuffer(modH);
    return u8CopyBufferSize;
}

/**
 * @brief
 * This method processes function 16
 * This method writes a word array assigned by the master
 *
 * @return u8BufferSize Response to master length
 * @ingroup register
 */
int8_t process_FC16(modbusHandler_t *modH )
{
    uint16_t u16StartAdd = modH->u8Buffer[ ADD_HI ] << 8 | modH->u8Buffer[ ADD_LO ];
    uint16_t u16regsno = modH->u8Buffer[ NB_HI ] << 8 | modH->u8Buffer[ NB_LO ];
    uint8_t u8CopyBufferSize;
    uint16_t i;
    uint16_t temp;

    // build header
    modH->u8Buffer[ NB_HI ]   = 0;
    modH->u8Buffer[ NB_LO ]   = (uint8_t) u16regsno; // answer is always 256 or less bytes
    modH->u8BufferSize         = RESPONSE_SIZE;

    // write registers
    for (i = 0; i < u16regsno; i++)
    {
        temp = word(
        		modH->u8Buffer[ (BYTE_CNT + 1) + i * 2 ],
				modH->u8Buffer[ (BYTE_CNT + 2) + i * 2 ]);

        modH->u16regsHR[ u16StartAdd + i ] = temp;
    }
    u8CopyBufferSize = modH->u8BufferSize +2;
    sendTxBuffer(modH);

    return u8CopyBufferSize;
}





