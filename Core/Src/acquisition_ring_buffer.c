#include "acquisition_ring_buffer.h"

#include "main.h"

#define ACQUISITION_RING_BUFFER_INDEX_MASK \
  (ACQUISITION_RING_BUFFER_CAPACITY - 1U)

_Static_assert((ACQUISITION_RING_BUFFER_CAPACITY &
                ACQUISITION_RING_BUFFER_INDEX_MASK) == 0U,
               "A capacidade do ring buffer precisa ser potencia de dois");

void AcquisitionRingBuffer_Initialize(AcquisitionRingBuffer *ringBuffer)
{
  ringBuffer->head = 0U;
  ringBuffer->tail = 0U;
  ringBuffer->highWatermark = 0U;
  ringBuffer->bufferFlushes = 0U;
  ringBuffer->framesDiscarded = 0U;
  ringBuffer->framesEnqueued = 0U;
}

AcquisitionRingBufferPushResult AcquisitionRingBuffer_Push(
    AcquisitionRingBuffer *ringBuffer,
    const AcquisitionFrame *frame)
{
  uint32_t head = ringBuffer->head;
  uint32_t tail = ringBuffer->tail;
  uint32_t used = head - tail;
  AcquisitionRingBufferPushResult pushResult =
      ACQUISITION_RING_BUFFER_PUSH_ENQUEUED;

  if (used >= ACQUISITION_RING_BUFFER_CAPACITY)
  {
    /*
     * Aceita a perda de todo o backlog e volta a acompanhar o produtor.
     * O frame novo sera o unico item da fila depois desta operacao.
     */
    __DMB();
    ringBuffer->tail = head;
    ringBuffer->bufferFlushes++;
    ringBuffer->framesDiscarded += used;
    used = 0U;
    pushResult = ACQUISITION_RING_BUFFER_PUSH_FLUSHED_BACKLOG;
  }

  /* Observa a liberacao do consumidor antes de reutilizar esta posicao. */
  __DMB();
  ringBuffer->frames[head & ACQUISITION_RING_BUFFER_INDEX_MASK] = *frame;

  /* Publica o frame somente depois que todos os seus campos foram copiados. */
  __DMB();
  ringBuffer->head = head + 1U;
  ringBuffer->framesEnqueued++;

  used++;
  if (used > ringBuffer->highWatermark)
  {
    ringBuffer->highWatermark = used;
  }

  return pushResult;
}

bool AcquisitionRingBuffer_TryPeekAt(const AcquisitionRingBuffer *ringBuffer,
                                     uint32_t offsetFromTail,
                                     AcquisitionFrame *frame)
{
  uint32_t tail = ringBuffer->tail;
  uint32_t used = ringBuffer->head - tail;

  if (offsetFromTail >= used)
  {
    return false;
  }

  __DMB();
  *frame = ringBuffer->frames[
      (tail + offsetFromTail) & ACQUISITION_RING_BUFFER_INDEX_MASK];

  return true;
}

bool AcquisitionRingBuffer_TryPop(AcquisitionRingBuffer *ringBuffer)
{
  uint32_t tail = ringBuffer->tail;

  if (ringBuffer->head == tail)
  {
    return false;
  }

  /* Libera a posicao somente depois que o consumidor terminou de usa-la. */
  __DMB();
  ringBuffer->tail = tail + 1U;

  return true;
}

uint32_t AcquisitionRingBuffer_GetTailPosition(
    const AcquisitionRingBuffer *ringBuffer)
{
  return ringBuffer->tail;
}

void AcquisitionRingBuffer_ReleaseTransmitted(
    AcquisitionRingBuffer *ringBuffer,
    uint32_t transmittedBlockTailPosition,
    uint32_t transmittedFrameCount)
{
  uint32_t currentTail = ringBuffer->tail;
  uint32_t framesAlreadyRemoved =
      currentTail - transmittedBlockTailPosition;

  if (framesAlreadyRemoved < transmittedFrameCount)
  {
    uint32_t remainingFramesToRelease =
        transmittedFrameCount - framesAlreadyRemoved;

    __DMB();
    ringBuffer->tail = currentTail + remainingFramesToRelease;
  }
}

uint32_t AcquisitionRingBuffer_Count(
    const AcquisitionRingBuffer *ringBuffer)
{
  return ringBuffer->head - ringBuffer->tail;
}

void AcquisitionRingBuffer_GetStatistics(
    const AcquisitionRingBuffer *ringBuffer,
    AcquisitionRingBufferStatistics *statistics)
{
  statistics->used = AcquisitionRingBuffer_Count(ringBuffer);
  statistics->highWatermark = ringBuffer->highWatermark;
  statistics->bufferFlushes = ringBuffer->bufferFlushes;
  statistics->framesDiscarded = ringBuffer->framesDiscarded;
  statistics->framesEnqueued = ringBuffer->framesEnqueued;
}
