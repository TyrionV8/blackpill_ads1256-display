#ifndef ACQUISITION_RING_BUFFER_H
#define ACQUISITION_RING_BUFFER_H

#include "acquisition_frame.h"

#include <stdbool.h>
#include <stdint.h>

/* Aproximadamente 4,096 segundos a 1.000 frames por segundo. */
#define ACQUISITION_RING_BUFFER_CAPACITY 4096U

typedef enum
{
  ACQUISITION_RING_BUFFER_PUSH_ENQUEUED,
  ACQUISITION_RING_BUFFER_PUSH_FLUSHED_BACKLOG
} AcquisitionRingBufferPushResult;

typedef struct
{
  AcquisitionFrame frames[ACQUISITION_RING_BUFFER_CAPACITY];
  volatile uint32_t head;
  volatile uint32_t tail;
  volatile uint32_t highWatermark;
  volatile uint32_t bufferFlushes;
  volatile uint32_t framesDiscarded;
  volatile uint32_t framesEnqueued;
} AcquisitionRingBuffer;

typedef struct
{
  uint32_t used;
  uint32_t highWatermark;
  uint32_t bufferFlushes;
  uint32_t framesDiscarded;
  uint32_t framesEnqueued;
} AcquisitionRingBufferStatistics;

void AcquisitionRingBuffer_Initialize(AcquisitionRingBuffer *ringBuffer);

AcquisitionRingBufferPushResult AcquisitionRingBuffer_Push(
    AcquisitionRingBuffer *ringBuffer,
    const AcquisitionFrame *frame);

bool AcquisitionRingBuffer_TryPeekAt(const AcquisitionRingBuffer *ringBuffer,
                                     uint32_t offsetFromTail,
                                     AcquisitionFrame *frame);

bool AcquisitionRingBuffer_TryPop(AcquisitionRingBuffer *ringBuffer);

uint32_t AcquisitionRingBuffer_GetTailPosition(
    const AcquisitionRingBuffer *ringBuffer);

void AcquisitionRingBuffer_ReleaseTransmitted(
    AcquisitionRingBuffer *ringBuffer,
    uint32_t transmittedBlockTailPosition,
    uint32_t transmittedFrameCount);

uint32_t AcquisitionRingBuffer_Count(
    const AcquisitionRingBuffer *ringBuffer);

void AcquisitionRingBuffer_GetStatistics(
    const AcquisitionRingBuffer *ringBuffer,
    AcquisitionRingBufferStatistics *statistics);

#endif /* ACQUISITION_RING_BUFFER_H */
