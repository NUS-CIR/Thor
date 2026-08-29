#ifndef NFAPI_PROXY_MICROBENCHMARK_H
#define NFAPI_PROXY_MICROBENCHMARK_H

#include <stdint.h>

void microbenchmark_log_event(const char *direction, const char *message_type,
                              uint16_t sfn, uint16_t slot, int pnf_index,
                              const char *event_type);
int microbenchmark_flush(void);

#endif
