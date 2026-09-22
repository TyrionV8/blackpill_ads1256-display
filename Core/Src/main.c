/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "usb_device.h"
#include "gpio.h"

/* USER CODE BEGIN Includes */
#include "application_config.h"
#include "acquisition_frame.h"
#include "acquisition_ring_buffer.h"
#include "adc_ads1256.h"
#include "display_st7735.h"
#include "microsecond_delay.h"
#include "usb_cdc_serial.h"
#include "usb_frame_transmitter.h"

#include <stdint.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

static AcquisitionRingBuffer acquisitionRingBuffer;

_Static_assert(ACQUISITION_FRAME_CHANNEL_COUNT ==
                   ADC_ADS1256_DIFFERENTIAL_CHANNEL_COUNT,
               "O frame e o ADS1256 precisam ter a mesma quantidade de canais");

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

static void Application_SetVectorTableToStartAfterWeActBootloader(void);
static void Application_SetInitialPinStates(void);
static void Application_DrawDisplaySelfTest(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  Application_SetVectorTableToStartAfterWeActBootloader();

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI2_Init();
  MX_USB_DEVICE_Init();
  MX_SPI3_Init();
  /* USER CODE BEGIN 2 */
  MicrosecondDelay_Initialize();
  Application_SetInitialPinStates();

  /*
   * Aguarda o computador reconhecer a USB CDC.
   * No Linux, a porta normalmente aparece como /dev/ttyACM0.
   */
  HAL_Delay(2000);

  DisplayST7735_Initialize();

  DisplayST7735_FillScreen(DISPLAY_COLOR_BLACK);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  UsbCdcSerial_WriteTextBlocking("# Inicializando ADS1256...\r\n");
  AdcADS1256_Initialize();
  UsbCdcSerial_WriteTextBlocking("# ADS1256 OK. Iniciando leitura.\r\n");
  UsbCdcSerial_WriteTextBlocking("# Formato CSV: FRAME,seq,t_us,d1,d2,d3,d4\r\n");

  uint32_t sampleSequenceNumber = 0;
  uint32_t framesAcquired = 0;
  bool acquisitionSessionActive = false;

  AcquisitionRingBuffer_Initialize(&acquisitionRingBuffer);
  UsbFrameTransmitter_Initialize(&acquisitionRingBuffer);

  while (1)
  {
    if (!UsbFrameTransmitter_IsHostPortOpen())
    {
      int32_t ignoredAdcValues[ADC_ADS1256_DIFFERENTIAL_CHANNEL_COUNT];

      if (acquisitionSessionActive)
      {
        acquisitionSessionActive = false;
        AcquisitionRingBuffer_Initialize(&acquisitionRingBuffer);
        UsbFrameTransmitter_Initialize(&acquisitionRingBuffer);
      }

      AdcADS1256_ReadDifferentialChannelFrame(ignoredAdcValues);
      continue;
    }

    if (!acquisitionSessionActive)
    {
      AcquisitionRingBuffer_Initialize(&acquisitionRingBuffer);
      UsbFrameTransmitter_Initialize(&acquisitionRingBuffer);
      sampleSequenceNumber = 0U;
      framesAcquired = 0U;
      acquisitionSessionActive = true;
    }

    AcquisitionFrame acquiredFrame;

    acquiredFrame.sequenceNumber = sampleSequenceNumber;
    acquiredFrame.timestampMicroseconds =
        MicrosecondDelay_GetTimestampMicroseconds();

    AdcADS1256_ReadDifferentialChannelFrame(acquiredFrame.channel);

    if (!UsbFrameTransmitter_IsHostPortOpen())
    {
      acquisitionSessionActive = false;
      AcquisitionRingBuffer_Initialize(&acquisitionRingBuffer);
      UsbFrameTransmitter_Initialize(&acquisitionRingBuffer);
      continue;
    }

    sampleSequenceNumber++;
    framesAcquired++;

    AcquisitionRingBufferPushResult pushResult =
        AcquisitionRingBuffer_Push(&acquisitionRingBuffer,
                                   &acquiredFrame);

    if (pushResult == ACQUISITION_RING_BUFFER_PUSH_FLUSHED_BACKLOG)
    {
      UsbFrameTransmitter_NotifyRingBufferFlushed();
    }

    if (UsbFrameTransmitter_Process(
            MicrosecondDelay_GetTimestampMicroseconds(),
            framesAcquired))
    {
      HAL_GPIO_TogglePin(APPLICATION_LED_GPIO_PORT,
                         APPLICATION_LED_GPIO_PIN);
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }

  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
static void Application_SetVectorTableToStartAfterWeActBootloader(void)
{
  /*
   * Como a aplicacao esta em 0x08004000 por causa do bootloader HID da WeAct,
   * a tabela de vetores tambem precisa apontar para 0x08004000.
   */
  SCB->VTOR = APPLICATION_START_ADDRESS_IN_FLASH;
  __DSB();
  __ISB();
}

static void Application_SetInitialPinStates(void)
{
  AdcADS1256_SetPinsToSafeState();
  DisplayST7735_SetPinsToSafeState();

  HAL_GPIO_WritePin(APPLICATION_LED_GPIO_PORT,
                    APPLICATION_LED_GPIO_PIN,
                    GPIO_PIN_RESET);
}

static void Application_DrawDisplaySelfTest(void)
{
  DisplayST7735_FillScreen(DISPLAY_COLOR_BLACK);

  DisplayST7735_DrawFilledRectangle(5, 5, 10, 10, DISPLAY_COLOR_GREEN);
  DisplayST7735_DrawFilledRectangle(20, 5, 10, 10, DISPLAY_COLOR_BLUE);
  DisplayST7735_DrawFilledRectangle(35, 5, 10, 10, DISPLAY_COLOR_YELLOW);
}

/* USER CODE END 4 */

