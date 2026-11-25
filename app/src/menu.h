/* The settings menu: controller-driven, saved to moonlight.ini on every change. Drawing thread only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "input.h"

typedef enum {
    MENU_NONE,      /* nothing to do */
    MENU_CONNECT,   /* the user chose Connect */
    MENU_EXIT,      /* the user chose Exit */
} MenuAction;

/* config is edited in place and saved to path. */
void menu_open(Config *config, const char *path);
/* A message shown under the menu (an error or why the last stream ended); NULL clears it. */
void menu_set_message(const char *message);
/* Whether the console's link is Wi-Fi, for the Rumble row's warning. */
void menu_set_wifi(bool on_wifi);
/* Handles one frame of input. */
MenuAction menu_update(const InputState *input);
void menu_draw(uint32_t width, uint32_t height);
