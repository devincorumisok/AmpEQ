#ifndef AMPEQ_CODEC_H
#define AMPEQ_CODEC_H
#include <stdio.h>
#include <stdlib.h>
int aeq_codec_dump(FILE *f, int all_audio, int full_xml);
/* returns 0 not found, 1 value only, 2 value + dB from the driver's range map */
int aeq_codec_control(const char *dev, unsigned id, int *value, double *db, double *dbmin, double *dbmax);
#endif
