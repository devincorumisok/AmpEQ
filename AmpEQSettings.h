#ifndef AMPEQ_SETTINGS_H
#define AMPEQ_SETTINGS_H
#include <stdint.h>
#include "AmpEQShared.h"
#ifdef __cplusplus
extern "C" {
#endif
int aeq_settings_from_dict(const void *cfdict, AEQSettings *s);
int aeq_settings_load(AEQSettings *s);
uint32_t aeq_settings_publish(const AEQSettings *s);
uint32_t aeq_settings_publish_saved(void);
uint64_t aeq_state(const char *name);
#ifdef __cplusplus
}
#endif
#endif
