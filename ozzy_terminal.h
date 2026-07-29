#pragma once

#include <furi.h>
#include <furi_hal_serial.h>
#include <gui/view.h>

#define OZZY_TERM_BAUD 115200
#define OZZY_TERM_RX_BUF_SIZE 2048
#define OZZY_TERM_LINE_MAX 256

typedef struct OzzyTerminal OzzyTerminal;

// Allocates the terminal module: opens the USART, starts the async RX
// worker, and builds its own View (text buffer + on-screen keyboard
// entry) that you can push into a ViewDispatcher, or drive manually
// like the rest of this app does.
OzzyTerminal* ozzy_terminal_alloc(void);

void ozzy_terminal_free(OzzyTerminal* term);

// Returns the View so it can be added to a ViewDispatcher, OR
// call ozzy_terminal_draw/ozzy_terminal_input directly if you're
// driving your own ViewPort loop (matches the rest of this app's style).
View* ozzy_terminal_get_view(OzzyTerminal* term);

void ozzy_terminal_draw(Canvas* canvas, OzzyTerminal* term);
void ozzy_terminal_handle_input(OzzyTerminal* term, InputEvent* event);

// True once the user has asked to leave the terminal screen (mapped to
// long-press BACK, since single BACK is used for backspace while typing).
bool ozzy_terminal_wants_exit(OzzyTerminal* term);
void ozzy_terminal_reset_exit_flag(OzzyTerminal* term);
