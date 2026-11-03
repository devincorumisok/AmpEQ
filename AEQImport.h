#ifndef AEQ_IMPORT_H
#define AEQ_IMPORT_H
#include <stddef.h>
#include "EQDSP.h"
#ifdef __cplusplus
extern "C" {
#endif
/* returns 1 and fills 12 fitted band gains on success */
int aeq_import_text(const char *text, float gains_out[EQ_BANDS], char *kind, size_t kindlen);
#ifdef __cplusplus
}
#endif
#endif
