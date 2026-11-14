// SPDX-License-Identifier: MIT
#pragma once
#include <time.h>
#include "esp_err.h"

typedef struct {
    int    id;
    char   title[96];
    time_t start;
    time_t end;          // 0 = unspecified
    char   person[32];   // "" = everyone
    char   notes[160];
} appointment_t;

esp_err_t appointments_init(void);
esp_err_t appointments_add(appointment_t *a);           // assigns a->id
esp_err_t appointments_update(const appointment_t *a);
esp_err_t appointments_delete(int id);
/** Fill out[] with appointments in [from,to); returns count. */
int appointments_list(time_t from, time_t to, const char *person, appointment_t *out, int max);
/** Short text summary of the next N days for the system instruction (caller frees). */
char *appointments_summary(int days);
