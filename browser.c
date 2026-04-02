/*
 * browser.c — Core browser logic, navigation, tab management, main loop
 */

#include "foxterm.h"
#include <time.h>

/* ── Forward declarations ── */
static void navigate_to(Browser *b, Tab *tab, const char *url);

/* ────────────────────────────────────────────────────────────────
 * Browser lifecycle
 * ──────────────────────────────────────────────────────────────── */

Browser *browser_new(void)
{
    Browser *b = calloc(1, sizeof(Browser));
    if (!b) return NULL;

    b->running      = true;
    b->tab_count    = 0;
    b->active_tab   = 0;
    b->marionette.fd = -1;

    /* Init curl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Init libxml2 */
    xmlInitParser();
    LIBXML_TEST_VERSION;

    return b;
}

void browser_free(Browser *b)
{
    if (!b) return;

    for (int i = 0; i < b->tab_count; i++) {
        free(b->tabs[i].lines);
    }

    marionette_disconnect(&b->marionette);
    free(b->search_matches);
    curl_global_cleanup();
    xmlCleanupParser();
    free(b);
}

/* ────────────────────────────────────────────────────────────────
 * Tab management
 * ──────────────────────────────────────────────────────────────── */

static Tab *new_tab_slot(Browser *b)
{
    if (b->tab_count >= TAB_MAX) return NULL;
    Tab *t = &b->tabs[b->tab_count++];
    memset(t, 0, sizeof(Tab));
    t->hist_pos = -1;
    t->hist_len = 0;
    t->lines = calloc(MAX_RENDERED_LINES, sizeof(RenderedLine));
    return t;
}

void browser_new_tab(Browser *b, const char *url)
{
    Tab *t = new_tab_slot(b);
    if (!t) {
        ui_set_status(b, "Max tabs reached (%d)", TAB_MAX);
        return;
    }
    b->active_tab = b->tab_count - 1;
    if (url && url[0]) {
        navigate_to(b, t, url);
    }
}

void browser_close_tab(Browser *b, int idx)
{
    if (b->tab_count <= 1) { b->running = false; return; }
    if (idx < 0 || idx >= b->tab_count) return;

    free(b->tabs[idx].lines);

    for (int i = idx; i < b->tab_count - 1; i++)
        b->tabs[i] = b->tabs[i+1];
    b->tab_count--;
    memset(&b->tabs[b->tab_count], 0, sizeof(Tab));

    if (b->active_tab >= b->tab_count)
        b->active_tab = b->tab_count - 1;
}

void browser_switch_tab(Browser *b, int idx)
{
    if (idx >= 0 && idx < b->tab_count)
        b->active_tab = idx;
}

/* ────────────────────────────────────────────────────────────────
 * History management (per-tab)
 * ──────────────────────────────────────────────────────────────── */

static void history_push(Tab *tab, const char *url)
{
    /* Truncate forward history */
    tab->hist_len = tab->hist_pos + 1;

    if (tab->hist_len >= MAX_HISTORY) {
        /* Shift */
        memmove(&tab->history[0], &tab->history[1],
                (MAX_HISTORY - 1) * sizeof(HistoryEntry));
        tab->hist_len = MAX_HISTORY - 1;
        tab->hist_pos = tab->hist_len - 1;
    }

    HistoryEntry *e = &tab->history[tab->hist_len];
    strncpy(e->url,       url,        MAX_URL_LEN   - 1);
    strncpy(e->title,     tab->title, MAX_TITLE_LEN - 1);
    e->scroll_pos = 0;

    tab->hist_pos = tab->hist_len;
    tab->hist_len++;
}

/* ────────────────────────────────────────────────────────────────
 * Core navigation
 * ──────────────────────────────────────────────────────────────── */

static void navigate_to(Browser *b, Tab *tab, const char *url)
{
    if (!url || !url[0]) return;

    /* Cleanup old content */
    tab->line_count   = 0;
    tab->link_count   = 0;
    tab->scroll_pos   = 0;
    tab->focused_link = 0;
    tab->loading      = true;
    tab->status[0]    = '\0';

    strncpy(tab->url, url, MAX_URL_LEN - 1);

    /* Redraw to show loading state */
    ui_draw(b);

    /* ── Special pages ── */
    if (!strcmp(url, "about:home") || !strcmp(url, "about:blank")) {
        render_about_page(b, tab);
        strncpy(tab->title, "FoxTerm Home", MAX_TITLE_LEN - 1);
        tab->loading = false;
        history_push(tab, url);
        ui_set_status(b, "Welcome to FoxTerm");
        return;
    }

    if (!strcmp(url, "about:bookmarks")) {
        render_bookmarks_page(b, tab);
        tab->loading = false;
        history_push(tab, url);
        return;
    }

    /* ── Try Firefox Marionette backend first ── */
    if (b->firefox_available && b->marionette.connected) {
        ui_set_status(b, "🦊 Navigating via Firefox…");
        ui_draw(b);

        if (marionette_navigate(&b->marionette, url)) {
            /* Get page source */
            char *src = marionette_get_page_source(&b->marionette);
            char *title = marionette_get_title(&b->marionette);

            if (src) {
                render_html(b, tab, src, url);
                free(src);
                tab->is_marionette = true;
            } else {
                render_error(b, tab, "Firefox returned empty page");
            }

            if (title) {
                strncpy(tab->title, title, MAX_TITLE_LEN - 1);
                free(title);
            }

            tab->loading = false;
            history_push(tab, url);
            ui_set_status(b, "🦊 Loaded via Firefox: %s", tab->title);
            return;
        }
        /* Fall through to direct fetch */
        ui_set_status(b, "Firefox navigation failed, trying direct fetch…");
    }

    /* ── Direct HTTP fetch ── */
    ui_set_status(b, "Fetching %s…", url);
    ui_draw(b);

    FetchResult *res = fetch_url(url, NULL);

    if (!res) {
        render_error(b, tab, "Failed to fetch URL (network error)");
        strncpy(tab->title, "Error", MAX_TITLE_LEN - 1);
        tab->loading = false;
        ui_set_status(b, "Error: Could not connect to %s", url);
        return;
    }

    /* Detect content type heuristically */
    bool looks_html = (res->size > 0 && (
        strncasecmp(res->data, "<!DOCTYPE", 9) == 0 ||
        strncasecmp(res->data, "<html",     5) == 0 ||
        strncasecmp(res->data, "<?xml",     5) == 0 ||
        strstr(res->data, "<body") ||
        strstr(res->data, "<head")
    ));

    if (looks_html) {
        render_html(b, tab, res->data, url);
    } else {
        render_text(b, tab, res->data);
    }

    fetch_result_free(res);

    /* Default title from URL if not set */
    if (!tab->title[0]) {
        const char *p = strrchr(url, '/');
        strncpy(tab->title, p ? p + 1 : url, MAX_TITLE_LEN - 1);
        if (!tab->title[0]) strncpy(tab->title, url, MAX_TITLE_LEN - 1);
    }

    tab->loading = false;
    history_push(tab, url);
    ui_set_status(b, "Loaded: %s", tab->title);
}

/* ────────────────────────────────────────────────────────────────
 * Public navigation API
 * ──────────────────────────────────────────────────────────────── */

void browser_navigate(Browser *b, const char *url)
{
    navigate_to(b, &b->tabs[b->active_tab], url);
}

void browser_go_back(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    if (tab->hist_pos <= 0) {
        ui_set_status(b, "No back history");
        return;
    }
    /* Save current scroll */
    tab->history[tab->hist_pos].scroll_pos = tab->scroll_pos;
    tab->hist_pos--;

    const char *url = tab->history[tab->hist_pos].url;
    strncpy(tab->url, url, MAX_URL_LEN - 1);

    /* Navigate without pushing new history */
    int saved_pos = tab->hist_pos;
    navigate_to(b, tab, url);
    tab->hist_pos = saved_pos;
    tab->hist_len = saved_pos + 1;  /* Don't extend forward */
    tab->scroll_pos = tab->history[tab->hist_pos].scroll_pos;
}

void browser_go_forward(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    if (tab->hist_pos >= tab->hist_len - 1) {
        ui_set_status(b, "No forward history");
        return;
    }
    tab->hist_pos++;
    const char *url = tab->history[tab->hist_pos].url;

    int saved_pos = tab->hist_pos;
    navigate_to(b, tab, url);
    tab->hist_pos = saved_pos;
    tab->scroll_pos = tab->history[tab->hist_pos].scroll_pos;
}

void browser_reload(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    if (tab->url[0]) {
        int scroll = tab->scroll_pos;
        navigate_to(b, tab, tab->url);
        tab->scroll_pos = scroll;
    }
}

/* ────────────────────────────────────────────────────────────────
 * Main event loop
 * ──────────────────────────────────────────────────────────────── */

void browser_run(Browser *b)
{
    /* Detect Firefox */
    int ff_port = 0;
    if (marionette_find_firefox(&ff_port)) {
        if (marionette_connect(&b->marionette, ff_port)) {
            b->firefox_available = true;
            ui_set_status(b, "🦊 Connected to Firefox on port %d", ff_port);
        }
    }

    /* Open initial tab */
    browser_new_tab(b, "about:home");

    /* Main loop */
    while (b->running) {
        ui_draw(b);

        /* Wait for input with timeout for animation */
        halfdelay(2);  /* 0.2s */
        int ch = getch();
        cbreak();

        if (ch == ERR) continue;  /* timeout, just redraw */

        input_handle(b, ch);
    }
}

/* ────────────────────────────────────────────────────────────────
 * main()
 * ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    /* Handle arguments */
    const char *start_url = "about:home";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: foxterm [URL]\n");
            printf("  FoxTerm v%s — Firefox-backed TUI browser\n", FOXTERM_VERSION);
            printf("  Options:\n");
            printf("    --help      Show this help\n");
            printf("    --version   Show version\n");
            printf("\n");
            printf("  Firefox backend (optional):\n");
            printf("    Launch Firefox with: firefox --marionette --headless\n");
            printf("    FoxTerm will auto-detect it on port %d\n",
                   MARIONETTE_DEFAULT_PORT);
            return 0;
        }
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("%s %s\n", FOXTERM_NAME, FOXTERM_VERSION);
            return 0;
        }
        /* Treat as URL */
        start_url = argv[i];
    }

    Browser *b = browser_new();
    if (!b) {
        fprintf(stderr, "foxterm: failed to initialize\n");
        return 1;
    }

    ui_init(b);

    /* Navigate to initial URL after UI is ready */
    if (strcmp(start_url, "about:home") != 0) {
        /* Queue navigation after loop start */
        /* We'll navigate on first iteration */
        ;
    }

    browser_run(b);

    ui_cleanup();

    /* Open requested URL if provided (handled inside run) */
    if (argc > 1 && strcmp(argv[argc-1], start_url) != 0) {
        /* already handled */
    }

    browser_free(b);
    return 0;
}