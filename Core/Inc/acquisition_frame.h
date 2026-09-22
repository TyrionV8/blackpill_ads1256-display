#ifndef ACQUISITION_FRAME_H
#define ACQUISITION_FRAME_H

#include <stdint.h>

#define ACQUISITION_FRAME_CHANNEL_COUNT 4U

typedef struct
{
  uint32_t sequenceNumber;
  uint32_t timestampMicroseconds;
  int32_t channel[ACQUISITION_FRAME_CHANNEL_COUNT];
} AcquisitionFrame;

_Static_assert(sizeof(AcquisitionFrame) == 24U,
               "AcquisitionFrame precisa ter 24 bytes");

#endif /* ACQUISITION_FRAME_H */
