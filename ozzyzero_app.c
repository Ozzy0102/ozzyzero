#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <loader/loader.h>
#include <string.h>
#include "ozzy_terminal.h"

#define TAG "Ozzyzero"

// Sentinel loader_name meaning "open our own UART terminal screen"
// instead of asking Loader to start a built-in app.
#define OZZY_TERMINAL_SENTINEL "__TERMINAL__"

// ---- Data model ----
//
// Each sub-item maps to the *registered app name* the Loader service
// uses internally (the same name the stock main menu uses to launch
// NFC, Sub-GHz, etc.). NULL loader_name = not launchable yet
// (placeholder / reimplemented-later item).

typedef struct {
    const char* label;
    const char* loader_name; // NULL = placeholder, no real app to launch
} SubItem;

typedef struct {
    const char* name;
    const SubItem* subitems;
    uint8_t subitem_count;
} Category;

// NOTE: these loader_name strings must match the appid registered in
// each built-in app's own application.fam (apptype=MENU or similar).
// Verify against your Momentum checkout's applications/main/*/application.fam
// before building - names can differ slightly between firmware versions.
static const SubItem nfc_items[] = {
    {"NFC App", "NFC"},
};

static const SubItem subghz_items[] = {
    {"Sub-GHz App", "Sub-GHz"},
};

static const SubItem ir_items[] = {
    {"Infrared App", "Infrared"},
};

static const SubItem ibutton_items[] = {
    {"iButton App", "iButton"},
};

static const SubItem gpio_items[] = {
    {"GPIO App", "GPIO"},
    {"UART Terminal", OZZY_TERMINAL_SENTINEL},
};

static const SubItem badusb_items[] = {
    {"Bad USB App", "Bad USB"},
};

static const SubItem u2f_items[] = {
    {"U2F App", "U2F"},
};

static const SubItem games_items[] = {
    {"Coming Soon", NULL},
};

static const Category categories[] = {
    {"NFC", nfc_items, 1},
    {"Sub-GHz", subghz_items, 1},
    {"Infrared", ir_items, 1},
    {"iButton", ibutton_items, 1},
    {"GPIO", gpio_items, 2},
    {"BadUSB", badusb_items, 1},
    {"U2F", u2f_items, 1},
    {"Pi Games", games_items, 1},
};

#define CATEGORY_COUNT (sizeof(categories) / sizeof(categories[0]))

// ---- App state ----

typedef enum {
    OzzyScreenMain,
    OzzyScreenSub,
    OzzyScreenTerminal,
} OzzyScreen;

typedef struct {
    ViewPort* view_port;
    Gui* gui;
    Loader* loader;
    FuriMessageQueue* input_queue;
    OzzyTerminal* terminal; // lazily allocated on first entry, freed on exit

    OzzyScreen screen;
    uint8_t main_index;
    uint8_t sub_index;

    const char* status_message; // brief on-screen feedback, e.g. launch errors

    bool running;
} OzzyzeroApp;

// ---- Drawing ----

static void draw_header(Canvas* canvas, const char* text) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, text);
    canvas_draw_line(canvas, 4, 14, 124, 14);
}

static void draw_carousel_arrows(Canvas* canvas) {
    canvas_draw_str(canvas, 2, 40, "<");
    canvas_draw_str(canvas, 122, 40, ">");
}

static void draw_main_screen(Canvas* canvas, OzzyzeroApp* app) {
    canvas_clear(canvas);
    canvas_draw_frame(canvas, 0, 0, 128, 64);
    draw_header(canvas, "OZZYZERO");

    const Category* cat = &categories[app->main_index];

    canvas_set_font(canvas, FontSecondary);
    char position[16];
    snprintf(position, sizeof(position), "%d/%d", app->main_index + 1, (int)CATEGORY_COUNT);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, position);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignCenter, cat->name);
    canvas_draw_rframe(canvas, 54, 40, 20, 14, 2);

    draw_carousel_arrows(canvas);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "OK: select   BACK: exit to Flipper");
}

static void draw_sub_screen(Canvas* canvas, OzzyzeroApp* app) {
    canvas_clear(canvas);
    canvas_draw_frame(canvas, 0, 0, 128, 64);

    const Category* cat = &categories[app->main_index];
    draw_header(canvas, cat->name);

    const SubItem* item = &cat->subitems[app->sub_index];

    canvas_set_font(canvas, FontSecondary);
    char position[16];
    snprintf(position, sizeof(position), "%d/%d", app->sub_index + 1, cat->subitem_count);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, position);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, item->label);

    draw_carousel_arrows(canvas);

    canvas_set_font(canvas, FontSecondary);
    if(app->status_message) {
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, app->status_message);
    }
    canvas_draw_str_aligned(canvas, 64, 58, AlignCenter, AlignCenter, "OK: open   BACK: categories");
}

static void draw_callback(Canvas* canvas, void* ctx) {
    OzzyzeroApp* app = ctx;
    if(app->screen == OzzyScreenMain) {
        draw_main_screen(canvas, app);
    } else if(app->screen == OzzyScreenSub) {
        draw_sub_screen(canvas, app);
    } else if(app->screen == OzzyScreenTerminal && app->terminal) {
        ozzy_terminal_draw(canvas, app->terminal);
    }
}

static void input_callback(InputEvent* event, void* ctx) {
    OzzyzeroApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

// ---- Real hand-off to a built-in app ----

static void open_terminal(OzzyzeroApp* app) {
    if(!app->terminal) {
        app->terminal = ozzy_terminal_alloc();
    }
    ozzy_terminal_reset_exit_flag(app->terminal);
    app->screen = OzzyScreenTerminal;
}

static void launch_builtin(OzzyzeroApp* app, const SubItem* item) {
    if(item->loader_name == NULL) {
        app->status_message = "Not wired up yet";
        return;
    }

    if(strcmp(item->loader_name, OZZY_TERMINAL_SENTINEL) == 0) {
        open_terminal(app);
        return;
    }

    // This mirrors what the stock main menu itself does to start
    // NFC / Sub-GHz / etc: ask the Loader service to start the app
    // by its registered name. Loader takes over the GUI; when the
    // user exits that app normally, control returns to whatever
    // was running before (i.e. back here, or to the desktop -
    // depends on firmware version's loader behavior).
    LoaderStatus status = loader_start(app->loader, item->loader_name, NULL, NULL);

    switch(status) {
    case LoaderStatusOk:
        app->status_message = NULL;
        break;
    case LoaderStatusErrorUnknownApp:
        app->status_message = "App name not found";
        FURI_LOG_E(TAG, "loader_start: unknown app '%s'", item->loader_name);
        break;
    case LoaderStatusErrorAppStarted:
        app->status_message = "Already running";
        break;
    default:
        app->status_message = "Launch failed";
        FURI_LOG_E(TAG, "loader_start failed for '%s'", item->loader_name);
        break;
    }
}

// ---- Navigation ----

static void handle_main_input(OzzyzeroApp* app, InputEvent* event) {
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return;

    switch(event->key) {
    case InputKeyLeft:
        app->main_index = (app->main_index == 0) ? CATEGORY_COUNT - 1 : app->main_index - 1;
        break;
    case InputKeyRight:
        app->main_index = (app->main_index + 1) % CATEGORY_COUNT;
        break;
    case InputKeyOk:
        app->screen = OzzyScreenSub;
        app->sub_index = 0;
        app->status_message = NULL;
        break;
    case InputKeyBack:
        app->running = false; // "Return to Flipper OS"
        break;
    default:
        break;
    }
}

static void handle_sub_input(OzzyzeroApp* app, InputEvent* event) {
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return;

    const Category* cat = &categories[app->main_index];

    switch(event->key) {
    case InputKeyLeft:
        app->sub_index = (app->sub_index == 0) ? cat->subitem_count - 1 : app->sub_index - 1;
        app->status_message = NULL;
        break;
    case InputKeyRight:
        app->sub_index = (app->sub_index + 1) % cat->subitem_count;
        app->status_message = NULL;
        break;
    case InputKeyOk:
        launch_builtin(app, &cat->subitems[app->sub_index]);
        break;
    case InputKeyBack:
        app->screen = OzzyScreenMain;
        app->status_message = NULL;
        break;
    default:
        break;
    }
}

// ---- Lifecycle ----

static OzzyzeroApp* ozzyzero_app_alloc(void) {
    OzzyzeroApp* app = malloc(sizeof(OzzyzeroApp));
    app->screen = OzzyScreenMain;
    app->main_index = 0;
    app->sub_index = 0;
    app->status_message = NULL;
    app->terminal = NULL;
    app->running = true;

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->loader = furi_record_open(RECORD_LOADER);

    return app;
}

static void ozzyzero_app_free(OzzyzeroApp* app) {
    if(app->terminal) {
        ozzy_terminal_free(app->terminal);
    }
    furi_record_close(RECORD_LOADER);
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    free(app);
}

int32_t ozzyzero_app(void* p) {
    UNUSED(p);
    OzzyzeroApp* app = ozzyzero_app_alloc();

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->input_queue, &event, 100) == FuriStatusOk) {
            if(app->screen == OzzyScreenMain) {
                handle_main_input(app, &event);
            } else if(app->screen == OzzyScreenSub) {
                handle_sub_input(app, &event);
            } else if(app->screen == OzzyScreenTerminal && app->terminal) {
                ozzy_terminal_handle_input(app->terminal, &event);
                if(ozzy_terminal_wants_exit(app->terminal)) {
                    app->screen = OzzyScreenSub;
                }
            }
        }
        view_port_update(app->view_port);
    }

    ozzyzero_app_free(app);
    return 0;
}
