#ifndef USB_FRAME_TRANSMITTER_H
#define USB_FRAME_TRANSMITTER_H

#include "acquisition_ring_buffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  uint32_t usbSubmitBusyCount;
  uint32_t usbSubmitErrorCount;
  uint32_t usbBlocksCompleted;
  uint32_t framesTransmitted;
} UsbFrameTransmitterStatistics;

void UsbFrameTransmitter_Initialize(AcquisitionRingBuffer *ringBuffer);

bool UsbFrameTransmitter_Process(uint32_t currentTimestampMicroseconds,
                                 uint32_t framesAcquired);

void UsbFrameTransmitter_NotifyTransmitComplete(void);
void UsbFrameTransmitter_NotifyTransmitAborted(void);
void UsbFrameTransmitter_NotifyHostPortState(bool isOpen);

bool UsbFrameTransmitter_IsHostPortOpen(void);

void UsbFrameTransmitter_NotifyRingBufferFlushed(void);

void UsbFrameTransmitter_GetStatistics(
    UsbFrameTransmitterStatistics *statistics);

#endif /* USB_FRAME_TRANSMITTER_H */
