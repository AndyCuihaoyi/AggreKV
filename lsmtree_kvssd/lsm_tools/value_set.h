#ifndef VALUESET_H
#define VALUESET_H

#include "../lsmtree/lsm_settings.h"
#include "container.h"
typedef struct value_set value_set;

value_set *inf_get_valueset(PTR in_v, uint32_t length);
void inf_free_valueset(value_set **in);

#endif // VALUESET_H