// SPDX-License-Identifier: MIT
#include "appointments.h"
// TODO: LittleFS JSON store, reminder timer task
esp_err_t appointments_init(void) { return ESP_OK; }
esp_err_t appointments_add(appointment_t *a) { (void)a; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t appointments_update(const appointment_t *a) { (void)a; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t appointments_delete(int id) { (void)id; return ESP_ERR_NOT_SUPPORTED; }
int appointments_list(time_t f, time_t t, const char *p, appointment_t *o, int m) { (void)f; (void)t; (void)p; (void)o; (void)m; return 0; }
char *appointments_summary(int days) { (void)days; return NULL; }
