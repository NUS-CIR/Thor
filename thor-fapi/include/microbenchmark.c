#include "microbenchmark.h"

#ifdef MICROBENCHMARK_LOGGING

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

#define DEFAULT_EVENT_CAPACITY 1000000UL
#define DEFAULT_OUTPUT_PATH "/tmp/proxy_message_log.csv"

typedef struct
{
    uint64_t ts_us;
    char direction[6];
    char message_type[24];
    char event_type[8];
    int32_t pnf_index;
    uint16_t sfn;
    uint16_t slot;
} microbenchmark_event_t;

static microbenchmark_event_t *events;
static size_t event_count;
static size_t event_capacity;
static bool full_warning_emitted;

static size_t configured_capacity(void)
{
    const char *value = getenv("NFAPI_MICROBENCHMARK_CAPACITY");
    if (value == NULL || *value == '\0') return DEFAULT_EVENT_CAPACITY;

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0)
    {
        log_warn("Ignoring invalid NFAPI_MICROBENCHMARK_CAPACITY='%s'", value);
        return DEFAULT_EVENT_CAPACITY;
    }
    return (size_t)parsed;
}

void microbenchmark_log_event(const char *direction, const char *message_type,
                              uint16_t sfn, uint16_t slot, int pnf_index,
                              const char *event_type)
{
    if (events == NULL)
    {
        event_capacity = configured_capacity();
        events = calloc(event_capacity, sizeof(*events));
        if (events == NULL)
        {
            log_error("Failed to allocate microbenchmark buffer for %zu events", event_capacity);
            event_capacity = 0;
            return;
        }
    }
    if (event_count >= event_capacity)
    {
        if (!full_warning_emitted)
        {
            log_warn("Microbenchmark buffer is full at %zu events; later events are dropped", event_capacity);
            full_warning_emitted = true;
        }
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    microbenchmark_event_t *event = &events[event_count++];
    event->ts_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    snprintf(event->direction, sizeof(event->direction), "%s", direction);
    snprintf(event->message_type, sizeof(event->message_type), "%s", message_type);
    snprintf(event->event_type, sizeof(event->event_type), "%s", event_type);
    event->pnf_index = pnf_index;
    event->sfn = sfn;
    event->slot = slot;
}

int microbenchmark_flush(void)
{
    if (events == NULL || event_count == 0) return 0;

    const char *path = getenv("NFAPI_MICROBENCHMARK_LOG");
    if (path == NULL || *path == '\0') path = DEFAULT_OUTPUT_PATH;
    FILE *output = fopen(path, "w");
    if (output == NULL)
    {
        log_error("Failed to open microbenchmark output %s: %s", path, strerror(errno));
        return -1;
    }

    static char io_buffer[1 << 20];
    setvbuf(output, io_buffer, _IOFBF, sizeof(io_buffer));
    for (size_t i = 0; i < event_count; i++)
    {
        const microbenchmark_event_t *event = &events[i];
        fprintf(output, "%llu,%s,%s,%u,%u,%d,%s\n",
                (unsigned long long)event->ts_us, event->direction,
                event->message_type, event->sfn, event->slot,
                event->pnf_index, event->event_type);
    }
    int result = fclose(output);
    if (result < 0)
        log_error("Failed to close microbenchmark output %s: %s", path, strerror(errno));
    else
        log_info("Flushed %zu microbenchmark events to %s", event_count, path);

    free(events);
    events = NULL;
    event_count = 0;
    event_capacity = 0;
    full_warning_emitted = false;
    return result;
}

#else

void microbenchmark_log_event(const char *direction, const char *message_type,
                              uint16_t sfn, uint16_t slot, int pnf_index,
                              const char *event_type)
{
    (void)direction;
    (void)message_type;
    (void)sfn;
    (void)slot;
    (void)pnf_index;
    (void)event_type;
}

int microbenchmark_flush(void)
{
    return 0;
}

#endif
