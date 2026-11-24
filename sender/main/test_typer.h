#ifndef TEST_TYPER_H
#define TEST_TYPER_H

#include "esp_err.h"
#include "keyboard_link.h"

typedef void (*test_typer_report_cb_t)(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE]);

esp_err_t test_typer_start(test_typer_report_cb_t on_report);

#endif
