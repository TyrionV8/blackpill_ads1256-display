#include "usb_frame_transmitter.h"

#include "main.h"
#include "usbd_cdc_if.h"

#include <stdio.h>

#define USB_FRAME_TRANSMITTER_MAXIMUM_FRAMES_PER_BLOCK 8U
#define USB_FRAME_TRANSMITTER_PARTIAL_BLOCK_LATENCY_US 4000U
#define USB_FRAME_TRANSMITTER_DIAGNOSTIC_INTERVAL_US 1000000U
#define USB_FRAME_TRANSMITTER_BUFFER_SIZE 1024U

typedef enum
{
  USB_FRAME_TRANSMITTER_STATE_IDLE,
  USB_FRAME_TRANSMITTER_STATE_READY_TO_SUBMIT,
  USB_FRAME_TRANSMITTER_STATE_IN_FLIGHT
} UsbFrameTransmitterState;

static AcquisitionRingBuffer *acquisitionRingBuffer = NULL;
static uint8_t transmitBuffer[USB_FRAME_TRANSMITTER_BUFFER_SIZE];
static uint16_t transmitBufferLength = 0U;
static uint8_t framesInTransmitBuffer = 0U;
static uint32_t transmitBlockTailPosition = 0U;
static volatile bool transmitCompletedFromInterrupt = false;
static volatile bool transmitAbortedFromInterrupt = false;
static volatile bool hostPortOpenFromInterrupt = false;
static UsbFrameTransmitterState transmitterState =
    USB_FRAME_TRANSMITTER_STATE_IDLE;
static bool partialBlockWaiting = false;
static uint32_t partialBlockWaitStartTimestampMicroseconds = 0U;
static uint32_t lastDiagnosticTimestampMicroseconds = 0U;
static UsbFrameTransmitterStatistics transmitterStatistics;

static bool UsbFrameTransmitter_BuildBlock(
    uint32_t currentTimestampMicroseconds,
    uint32_t framesAcquired);

static bool UsbFrameTransmitter_AppendDiagnostics(
    uint32_t currentTimestampMicroseconds,
    uint32_t framesAcquired);

void UsbFrameTransmitter_Initialize(AcquisitionRingBuffer *ringBuffer)
{
  acquisitionRingBuffer = ringBuffer;
  transmitBufferLength = 0U;
  framesInTransmitBuffer = 0U;
  transmitBlockTailPosition = 0U;
  transmitCompletedFromInterrupt = false;
  transmitAbortedFromInterrupt = false;
  transmitterState = USB_FRAME_TRANSMITTER_STATE_IDLE;
  partialBlockWaiting = false;
  partialBlockWaitStartTimestampMicroseconds = 0U;
  lastDiagnosticTimestampMicroseconds = 0U;

  transmitterStatistics.usbSubmitBusyCount = 0U;
  transmitterStatistics.usbSubmitErrorCount = 0U;
  transmitterStatistics.usbBlocksCompleted = 0U;
  transmitterStatistics.framesTransmitted = 0U;
}

bool UsbFrameTransmitter_Process(uint32_t currentTimestampMicroseconds,
                                 uint32_t framesAcquired)
{
  bool blockCompleted = false;

  if (acquisitionRingBuffer == NULL)
  {
    return false;
  }

  if (transmitterState == USB_FRAME_TRANSMITTER_STATE_IN_FLIGHT)
  {
    if (!transmitCompletedFromInterrupt)
    {
      if (!transmitAbortedFromInterrupt)
      {
        return false;
      }

      __DMB();
      transmitAbortedFromInterrupt = false;
      transmitterStatistics.usbSubmitErrorCount++;
      transmitterState = USB_FRAME_TRANSMITTER_STATE_READY_TO_SUBMIT;
    }
    else
    {
      __DMB();
      transmitCompletedFromInterrupt = false;
      transmitAbortedFromInterrupt = false;

      AcquisitionRingBuffer_ReleaseTransmitted(
          acquisitionRingBuffer,
          transmitBlockTailPosition,
          framesInTransmitBuffer);

      transmitterStatistics.usbBlocksCompleted++;
      transmitterStatistics.framesTransmitted += framesInTransmitBuffer;
      transmitterState = USB_FRAME_TRANSMITTER_STATE_IDLE;
      framesInTransmitBuffer = 0U;
      transmitBufferLength = 0U;
      blockCompleted = true;
    }
  }

  if (transmitterState == USB_FRAME_TRANSMITTER_STATE_IDLE)
  {
    uint32_t queuedFrames =
        AcquisitionRingBuffer_Count(acquisitionRingBuffer);

    if (queuedFrames == 0U)
    {
      partialBlockWaiting = false;
      return blockCompleted;
    }

    if (queuedFrames < USB_FRAME_TRANSMITTER_MAXIMUM_FRAMES_PER_BLOCK)
    {
      if (!partialBlockWaiting)
      {
        partialBlockWaiting = true;
        partialBlockWaitStartTimestampMicroseconds =
            currentTimestampMicroseconds;
        return blockCompleted;
      }

      if ((currentTimestampMicroseconds -
           partialBlockWaitStartTimestampMicroseconds) <
          USB_FRAME_TRANSMITTER_PARTIAL_BLOCK_LATENCY_US)
      {
        return blockCompleted;
      }
    }

    partialBlockWaiting = false;

    if (!UsbFrameTransmitter_BuildBlock(currentTimestampMicroseconds,
                                        framesAcquired))
    {
      transmitterStatistics.usbSubmitErrorCount++;
      return blockCompleted;
    }

    transmitterState = USB_FRAME_TRANSMITTER_STATE_READY_TO_SUBMIT;
  }

  if (transmitterState == USB_FRAME_TRANSMITTER_STATE_READY_TO_SUBMIT)
  {
    transmitCompletedFromInterrupt = false;
    transmitAbortedFromInterrupt = false;
    __DMB();

    uint8_t transmitResult =
        CDC_Transmit_FS(transmitBuffer, transmitBufferLength);

    if (transmitResult == USBD_OK)
    {
      transmitterState = USB_FRAME_TRANSMITTER_STATE_IN_FLIGHT;
    }
    else if (transmitResult == USBD_BUSY)
    {
      transmitterStatistics.usbSubmitBusyCount++;
    }
    else
    {
      transmitterStatistics.usbSubmitErrorCount++;
    }
  }

  return blockCompleted;
}

void UsbFrameTransmitter_NotifyTransmitComplete(void)
{
  transmitCompletedFromInterrupt = true;
  __DMB();
}

void UsbFrameTransmitter_NotifyTransmitAborted(void)
{
  transmitAbortedFromInterrupt = true;
  __DMB();
}

void UsbFrameTransmitter_NotifyHostPortState(bool isOpen)
{
  hostPortOpenFromInterrupt = isOpen;

  if (!isOpen)
  {
    transmitAbortedFromInterrupt = true;
  }

  __DMB();
}

bool UsbFrameTransmitter_IsHostPortOpen(void)
{
  bool isOpen = hostPortOpenFromInterrupt;
  __DMB();
  return isOpen;
}

void UsbFrameTransmitter_NotifyRingBufferFlushed(void)
{
  /*
   * Um bloco ja entregue ao controlador USB precisa permanecer imutavel ate o
   * callback. Um bloco apenas preparado ainda pode ser abandonado com seguranca.
   */
  if (transmitterState == USB_FRAME_TRANSMITTER_STATE_READY_TO_SUBMIT)
  {
    transmitterState = USB_FRAME_TRANSMITTER_STATE_IDLE;
    transmitBufferLength = 0U;
    framesInTransmitBuffer = 0U;
    partialBlockWaiting = false;
  }
}

void UsbFrameTransmitter_GetStatistics(
    UsbFrameTransmitterStatistics *statistics)
{
  *statistics = transmitterStatistics;
}

static bool UsbFrameTransmitter_BuildBlock(
    uint32_t currentTimestampMicroseconds,
    uint32_t framesAcquired)
{
  uint32_t queuedFrames =
      AcquisitionRingBuffer_Count(acquisitionRingBuffer);
  uint32_t framesToTransmit = queuedFrames;

  if (framesToTransmit > USB_FRAME_TRANSMITTER_MAXIMUM_FRAMES_PER_BLOCK)
  {
    framesToTransmit = USB_FRAME_TRANSMITTER_MAXIMUM_FRAMES_PER_BLOCK;
  }

  transmitBufferLength = 0U;
  framesInTransmitBuffer = 0U;
  transmitBlockTailPosition =
      AcquisitionRingBuffer_GetTailPosition(acquisitionRingBuffer);

  for (uint32_t frameIndex = 0U;
       frameIndex < framesToTransmit;
       frameIndex++)
  {
    AcquisitionFrame frame;

    if (!AcquisitionRingBuffer_TryPeekAt(acquisitionRingBuffer,
                                        frameIndex,
                                        &frame))
    {
      return false;
    }

    size_t remainingBufferSize =
        sizeof(transmitBuffer) - transmitBufferLength;
    int charactersWritten = snprintf(
        (char *)&transmitBuffer[transmitBufferLength],
        remainingBufferSize,
        "FRAME,%lu,%lu,%ld,%ld,%ld,%ld\r\n",
        (unsigned long)frame.sequenceNumber,
        (unsigned long)frame.timestampMicroseconds,
        (long)frame.channel[0],
        (long)frame.channel[1],
        (long)frame.channel[2],
        (long)frame.channel[3]);

    if ((charactersWritten <= 0) ||
        ((size_t)charactersWritten >= remainingBufferSize))
    {
      return false;
    }

    transmitBufferLength += (uint16_t)charactersWritten;
    framesInTransmitBuffer++;
  }

  if ((currentTimestampMicroseconds -
       lastDiagnosticTimestampMicroseconds) >=
      USB_FRAME_TRANSMITTER_DIAGNOSTIC_INTERVAL_US)
  {
    if (!UsbFrameTransmitter_AppendDiagnostics(currentTimestampMicroseconds,
                                               framesAcquired))
    {
      return false;
    }
  }

  return framesInTransmitBuffer > 0U;
}

static bool UsbFrameTransmitter_AppendDiagnostics(
    uint32_t currentTimestampMicroseconds,
    uint32_t framesAcquired)
{
  AcquisitionRingBufferStatistics ringStatistics;
  AcquisitionRingBuffer_GetStatistics(acquisitionRingBuffer,
                                      &ringStatistics);

  size_t remainingBufferSize =
      sizeof(transmitBuffer) - transmitBufferLength;
  int charactersWritten = snprintf(
      (char *)&transmitBuffer[transmitBufferLength],
      remainingBufferSize,
      "# BUFFER,USED=%lu,HIGH=%lu,OVERRUN=%lu,FLUSHES=%lu,DROPPED=%lu,OVERWRITTEN=%lu,ACQUIRED=%lu,ENQUEUED=%lu\r\n"
      "# USB,BUSY=%lu,ERROR=%lu,BLOCKS=%lu,SENT=%lu\r\n",
      (unsigned long)ringStatistics.used,
      (unsigned long)ringStatistics.highWatermark,
      (unsigned long)ringStatistics.bufferFlushes,
      (unsigned long)ringStatistics.bufferFlushes,
      (unsigned long)ringStatistics.framesDiscarded,
      (unsigned long)ringStatistics.framesDiscarded,
      (unsigned long)framesAcquired,
      (unsigned long)ringStatistics.framesEnqueued,
      (unsigned long)transmitterStatistics.usbSubmitBusyCount,
      (unsigned long)transmitterStatistics.usbSubmitErrorCount,
      (unsigned long)transmitterStatistics.usbBlocksCompleted,
      (unsigned long)transmitterStatistics.framesTransmitted);

  if ((charactersWritten <= 0) ||
      ((size_t)charactersWritten >= remainingBufferSize))
  {
    return false;
  }

  transmitBufferLength += (uint16_t)charactersWritten;
  lastDiagnosticTimestampMicroseconds = currentTimestampMicroseconds;

  return true;
}
