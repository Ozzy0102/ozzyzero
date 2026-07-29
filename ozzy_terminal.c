#include "ozzy_terminal.h"
#include <furi_hal_serial_control.h>
#include <string.h>
#include <stdlib.h>

#define TAG "OzzyTerminal"

// Simple on-screen keyboard character set, cycled with LEFT/RIGHT,
// same "carousel" feel as the rest of the app rather than a full
// QWERTY grid (keeps this readable on a 128x64 screen).
static const char kb_chars[] =
    "abcdefghijklmnopqrstuvwxyz0123456789 .,-_/:@!?~<>|&*()[]{}=+";
#define KB_CHAR_COUNT (sizeof(kb_chars) - 1)

struct OzzyTerminal {
    FuriHalSerialHandle* serial_handle;

    // Scrollback buffer (raw bytes received from the Pi)
    char rx_buf[OZZY_TERM_RX_BUF_SIZE];
    size_t rx_len;
    FuriMutex* rx_mutex;

    // Line currently being typed, sent on Enter (OK on the "Enter" key slot)
    char line_buf[OZZY_TERM_LINE_MAX];
    size_t line_len;

    // On-screen keyboard cursor
    size_t kb_index;

    bool wants_exit;

    View* view; // optional, if used with ViewDispatcher
};

// ---- Serial RX callback (runs in interrupt/worker context - keep it fast) ----

static void ozzy_terminal_uart_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    OzzyTerminal* term = context;

    if(event == FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);

        furi_mutex_acquire(term->rx_mutex, FuriWaitForever);
        if(term->rx_len < OZZY_TERM_RX_BUF_SIZE - 1) {
            term->rx_buf[term->rx_len++] = (char)byte;
        } else {
            // Buffer full: drop oldest half to keep scrolling instead of
            // freezing. Simple approach, fine for a debug/control console.
            size_t shift = OZZY_TERM_RX_BUF_SIZE / 2;
            memmove(term->rx_buf, term->rx_buf + shift, term->rx_len - shift);
            term->rx_len -= shift;
            term->rx_buf[term->rx_len++] = (char)byte;
        }
        furi_mutex_release(term->rx_mutex);
    }
}

// ---- Lifecycle ----

OzzyTerminal* ozzy_terminal_alloc(void) {
    OzzyTerminal* term = malloc(sizeof(OzzyTerminal));
    memset(term, 0, sizeof(OzzyTerminal));

    term->rx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    term->serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(term->serial_handle) {
        furi_hal_serial_init(term->serial_handle, OZZY_TERM_BAUD);
        furi_hal_serial_async_rx_start(
            term->serial_handle, ozzy_terminal_uart_rx_callback, term, false);
    } else {
        FURI_LOG_E(TAG, "Failed to acquire USART - already in use?");
    }

    term->view = NULL; // this app drives its own ViewPort loop; view unused for now

    return term;
}

void ozzy_terminal_free(OzzyTerminal* term) {
    if(term->serial_handle) {
        furi_hal_serial_async_rx_stop(term->serial_handle);
        furi_hal_serial_deinit(term->serial_handle);
        furi_hal_serial_control_release(term->serial_handle);
    }
    furi_mutex_free(term->rx_mutex);
    free(term);
}

View* ozzy_terminal_get_view(OzzyTerminal* term) {
    return term->view;
}

bool ozzy_terminal_wants_exit(OzzyTerminal* term) {
    return term->wants_exit;
}

void ozzy_terminal_reset_exit_flag(OzzyTerminal* term) {
    term->wants_exit = false;
}

// ---- Sending a line to the Pi ----

static void ozzy_terminal_send_line(OzzyTerminal* term) {
    if(!term->serial_handle || term->line_len == 0) return;

    furi_hal_serial_tx(term->serial_handle, (const uint8_t*)term->line_buf, term->line_len);
    const uint8_t newline = '\n';
    furi_hal_serial_tx(term->serial_handle, &newline, 1);
    furi_hal_serial_tx_wait_complete(term->serial_handle);

    // Echo what we sent into the scrollback so the user sees their own command
    furi_mutex_acquire(term->rx_mutex, FuriWaitForever);
    char prefix[4] = "> ";
    size_t avail = OZZY_TERM_RX_BUF_SIZE - term->rx_len;
    size_t to_copy = 2;
    if(to_copy > avail) to_copy = avail;
    memcpy(term->rx_buf + term->rx_len, prefix, to_copy);
    term->rx_len += to_copy;

    avail = OZZY_TERM_RX_BUF_SIZE - term->rx_len;
    to_copy = term->line_len;
    if(to_copy > avail) to_copy = avail;
    memcpy(term->rx_buf + term->rx_len, term->line_buf, to_copy);
    term->rx_len += to_copy;

    if(term->rx_len < OZZY_TERM_RX_BUF_SIZE - 1) {
        term->rx_buf[term->rx_len++] = '\n';
    }
    furi_mutex_release(term->rx_mutex);

    term->line_len = 0;
    term->line_buf[0] = '\0';
}

// ---- Drawing ----

void ozzy_terminal_draw(Canvas* canvas, OzzyTerminal* term) {
    canvas_clear(canvas);
    canvas_draw_frame(canvas, 0, 0, 128, 64);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 7, AlignCenter, AlignCenter, "UART TERMINAL");
    canvas_draw_line(canvas, 2, 12, 126, 12);

    // Scrollback: show the tail end of the buffer (most recent lines)
    furi_mutex_acquire(term->rx_mutex, FuriWaitForever);

    const int scrollback_area_top = 14;
    const int scrollback_area_bottom = 40;
    const int line_height = 7;
    int max_lines = (scrollback_area_bottom - scrollback_area_top) / line_height;

    // Walk backward from the end of rx_buf, splitting on '\n', to find
    // the start index of the last `max_lines` lines.
    int lines_found = 0;
    int start = (int)term->rx_len;
    for(int i = (int)term->rx_len - 1; i >= 0 && lines_found < max_lines; i--) {
        if(term->rx_buf[i] == '\n') {
            lines_found++;
            if(lines_found == max_lines) {
                start = i + 1;
                break;
            }
        }
        if(i == 0) start = 0;
    }

    // Render from `start` to end, drawing one row per '\n'-delimited chunk
    char line[64];
    size_t line_pos = 0;
    int y = scrollback_area_top + 5;
    for(int i = start; i < (int)term->rx_len && y < scrollback_area_bottom + 5; i++) {
        char c = term->rx_buf[i];
        if(c == '\n' || line_pos >= sizeof(line) - 1) {
            line[line_pos] = '\0';
            canvas_draw_str(canvas, 3, y, line);
            y += line_height;
            line_pos = 0;
        } else if(c >= 32 && c < 127) {
            line[line_pos++] = c;
        }
    }
    if(line_pos > 0 && y < scrollback_area_bottom + 5) {
        line[line_pos] = '\0';
        canvas_draw_str(canvas, 3, y, line);
    }

    furi_mutex_release(term->rx_mutex);

    // Divider before input area
    canvas_draw_line(canvas, 2, 42, 126, 42);

    // Current typed line
    canvas_set_font(canvas, FontSecondary);
    char typed[40];
    snprintf(typed, sizeof(typed), "> %s_", term->line_buf);
    canvas_draw_str(canvas, 3, 49, typed);

    // On-screen keyboard: current character highlighted, neighbors shown for context
    char kb_display[6];
    char prev_char = kb_chars[(term->kb_index == 0) ? KB_CHAR_COUNT - 1 : term->kb_index - 1];
    char cur_char = kb_chars[term->kb_index];
    char next_char = kb_chars[(term->kb_index + 1) % KB_CHAR_COUNT];
    snprintf(kb_display, sizeof(kb_display), "%c[%c]%c", prev_char, cur_char, next_char);
    canvas_draw_str_aligned(canvas, 64, 57, AlignCenter, AlignCenter, kb_display);

    canvas_draw_str_aligned(
        canvas, 64, 63, AlignCenter, AlignCenter, "OK:add UP:space DOWN:send");
}

// ---- Input handling ----
//
// Controls (single-hand, d-pad only, no physical keyboard on Flipper):
//   LEFT / RIGHT  -> cycle character on the on-screen keyboard
//   OK (short)    -> append the highlighted character to the typed line
//   UP            -> append a space
//   DOWN          -> send the typed line to the Pi over UART
//   BACK (short)  -> backspace one character
//   BACK (long)   -> exit the terminal screen back to the carousel

void ozzy_terminal_handle_input(OzzyTerminal* term, InputEvent* event) {
    if(event->type == InputTypeLong && event->key == InputKeyBack) {
        term->wants_exit = true;
        return;
    }

    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return;

    switch(event->key) {
    case InputKeyLeft:
        term->kb_index = (term->kb_index == 0) ? KB_CHAR_COUNT - 1 : term->kb_index - 1;
        break;
    case InputKeyRight:
        term->kb_index = (term->kb_index + 1) % KB_CHAR_COUNT;
        break;
    case InputKeyOk:
        if(term->line_len < OZZY_TERM_LINE_MAX - 1) {
            term->line_buf[term->line_len++] = kb_chars[term->kb_index];
            term->line_buf[term->line_len] = '\0';
        }
        break;
    case InputKeyUp:
        if(term->line_len < OZZY_TERM_LINE_MAX - 1) {
            term->line_buf[term->line_len++] = ' ';
            term->line_buf[term->line_len] = '\0';
        }
        break;
    case InputKeyDown:
        ozzy_terminal_send_line(term);
        break;
    case InputKeyBack:
        if(term->line_len > 0) {
            term->line_len--;
            term->line_buf[term->line_len] = '\0';
        }
        break;
    default:
        break;
    }
}
