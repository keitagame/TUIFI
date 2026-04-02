/*
 * input.c — keyboard event handling
 */

#include "foxterm.h"

/* ── Scroll helpers ── */
static void scroll_down(Browser *b, int n)
{
    Tab *tab = &b->tabs[b->active_tab];
    int max_scroll = tab->line_count - (b->term_rows - 3);
    if (max_scroll < 0) max_scroll = 0;
    tab->scroll_pos += n;
    if (tab->scroll_pos > max_scroll) tab->scroll_pos = max_scroll;
}

static void scroll_up(Browser *b, int n)
{
    Tab *tab = &b->tabs[b->active_tab];
    tab->scroll_pos -= n;
    if (tab->scroll_pos < 0) tab->scroll_pos = 0;
}

/* ── Focus next/prev link ── */
static void focus_next_link(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    if (tab->link_count == 0) return;

    int start = tab->focused_link;
    int content_rows = b->term_rows - 3;

    for (int attempt = 0; attempt < tab->line_count; attempt++) {
        start = (start + 1) % tab->link_count;
        /* Find a line with this link_id */
        for (int i = 0; i < tab->line_count; i++) {
            if (tab->lines[i].is_link && tab->lines[i].link_id == start) {
                tab->focused_link = start;
                /* Scroll to keep link visible */
                if (i < tab->scroll_pos) {
                    tab->scroll_pos = i;
                } else if (i >= tab->scroll_pos + content_rows - 1) {
                    tab->scroll_pos = i - content_rows + 2;
                    if (tab->scroll_pos < 0) tab->scroll_pos = 0;
                }
                return;
            }
        }
    }
}

static void focus_prev_link(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    if (tab->link_count == 0) return;

    int start = tab->focused_link;
    int content_rows = b->term_rows - 3;

    for (int attempt = 0; attempt < tab->link_count; attempt++) {
        start = (start - 1 + tab->link_count) % tab->link_count;
        for (int i = tab->line_count - 1; i >= 0; i--) {
            if (tab->lines[i].is_link && tab->lines[i].link_id == start) {
                tab->focused_link = start;
                if (i < tab->scroll_pos) {
                    tab->scroll_pos = i;
                } else if (i >= tab->scroll_pos + content_rows - 1) {
                    tab->scroll_pos = i - content_rows + 2;
                    if (tab->scroll_pos < 0) tab->scroll_pos = 0;
                }
                return;
            }
        }
    }
}

/* ── Search ── */
static void search_update(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    free(b->search_matches);
    b->search_matches = NULL;
    b->search_match_count = 0;
    b->search_pos = 0;

    if (!b->search_buf[0]) return;

    int *matches = malloc(tab->line_count * sizeof(int));
    if (!matches) return;
    int count = 0;

    for (int i = 0; i < tab->line_count; i++) {
        if (strcasestr(tab->lines[i].text, b->search_buf)) {
            matches[count++] = i;
        }
    }

    b->search_matches     = matches;
    b->search_match_count = count;

    if (count > 0) {
        /* Jump to first match */
        int content_rows = b->term_rows - 3;
        tab->scroll_pos = matches[0];
        if (tab->scroll_pos + content_rows > tab->line_count)
            tab->scroll_pos = tab->line_count - content_rows;
        if (tab->scroll_pos < 0) tab->scroll_pos = 0;
    }
}

static void search_next(Browser *b)
{
    if (b->search_match_count == 0) return;
    b->search_pos = (b->search_pos + 1) % b->search_match_count;
    int line = b->search_matches[b->search_pos];
    int content_rows = b->term_rows - 3;
    Tab *tab = &b->tabs[b->active_tab];
    tab->scroll_pos = line - content_rows / 2;
    if (tab->scroll_pos < 0) tab->scroll_pos = 0;
}

static void search_prev(Browser *b)
{
    if (b->search_match_count == 0) return;
    b->search_pos = (b->search_pos - 1 + b->search_match_count) % b->search_match_count;
    int line = b->search_matches[b->search_pos];
    int content_rows = b->term_rows - 3;
    Tab *tab = &b->tabs[b->active_tab];
    tab->scroll_pos = line - content_rows / 2;
    if (tab->scroll_pos < 0) tab->scroll_pos = 0;
}

/* ────────────────────────────────────────────────────────────────
 * URL bar input
 * ──────────────────────────────────────────────────────────────── */

void input_url_bar(Browser *b, int ch)
{
    int len = (int)strlen(b->url_buf);

    switch (ch) {
    case '\n': case KEY_ENTER: {
        b->url_bar_active = false;
        curs_set(0);
        /* Auto-add scheme if missing */
        char nav_url[MAX_URL_LEN];
        if (!strstr(b->url_buf, "://") &&
            !str_startswith(b->url_buf, "about:") &&
            b->url_buf[0]) {
            /* Check if it looks like a search query */
            bool has_dot = strchr(b->url_buf, '.') != NULL;
            bool has_space = strchr(b->url_buf, ' ') != NULL;
            if (has_space || !has_dot) {
                /* Search with DuckDuckGo */
                char *encoded = url_encode(b->url_buf);
                snprintf(nav_url, sizeof(nav_url),
                         "https://duckduckgo.com/?q=%s", encoded ? encoded : "");
                free(encoded);
            } else {
                snprintf(nav_url, sizeof(nav_url), "https://%s", b->url_buf);
            }
        } else {
            strncpy(nav_url, b->url_buf, MAX_URL_LEN - 1);
        }
        browser_navigate(b, nav_url);
        break;
    }

    case 27: /* Escape */
        b->url_bar_active = false;
        curs_set(0);
        break;

    case KEY_BACKSPACE: case 127: case 8:
        if (b->url_cursor > 0) {
            memmove(b->url_buf + b->url_cursor - 1,
                    b->url_buf + b->url_cursor,
                    len - b->url_cursor + 1);
            b->url_cursor--;
        }
        break;

    case KEY_DC:
        if (b->url_cursor < len) {
            memmove(b->url_buf + b->url_cursor,
                    b->url_buf + b->url_cursor + 1,
                    len - b->url_cursor);
        }
        break;

    case KEY_LEFT:
        if (b->url_cursor > 0) b->url_cursor--;
        break;

    case KEY_RIGHT:
        if (b->url_cursor < len) b->url_cursor++;
        break;

    case KEY_HOME: case 1: /* Ctrl+A */
        b->url_cursor = 0;
        break;

    case KEY_END: case 5: /* Ctrl+E */
        b->url_cursor = len;
        break;

    case 21: /* Ctrl+U — clear */
        b->url_buf[0] = '\0';
        b->url_cursor = 0;
        break;

    case 11: /* Ctrl+K — kill to end */
        b->url_buf[b->url_cursor] = '\0';
        break;

    default:
        if (ch >= 32 && ch < 256 && len < MAX_URL_LEN - 1) {
            memmove(b->url_buf + b->url_cursor + 1,
                    b->url_buf + b->url_cursor,
                    len - b->url_cursor + 1);
            b->url_buf[b->url_cursor++] = (char)ch;
        }
        break;
    }
}

/* ────────────────────────────────────────────────────────────────
 * Search bar input
 * ──────────────────────────────────────────────────────────────── */

void input_search(Browser *b, int ch)
{
    int len = (int)strlen(b->search_buf);

    switch (ch) {
    case '\n': case KEY_ENTER:
        if (!b->search_buf[0]) {
            b->search_active = false;
        } else {
            search_next(b);
        }
        break;

    case 27: /* Escape */
        b->search_active = false;
        b->search_buf[0] = '\0';
        free(b->search_matches);
        b->search_matches = NULL;
        b->search_match_count = 0;
        break;

    case KEY_BACKSPACE: case 127: case 8:
        if (len > 0) {
            b->search_buf[--len] = '\0';
            search_update(b);
        }
        break;

    default:
        if (ch >= 32 && ch < 256 && len < MAX_SEARCH_LEN - 1) {
            b->search_buf[len]   = (char)ch;
            b->search_buf[len+1] = '\0';
            search_update(b);
        }
        break;
    }
}

/* ────────────────────────────────────────────────────────────────
 * Main input handler
 * ──────────────────────────────────────────────────────────────── */

void input_handle(Browser *b, int ch)
{
    /* Delegate to active input mode */
    if (b->url_bar_active)  { input_url_bar(b, ch); return; }
    if (b->search_active)   { input_search(b, ch);  return; }

    Tab *tab = &b->tabs[b->active_tab];

    switch (ch) {

    /* ── Quit ── */
    case 'q': case 'Q':
        b->running = false;
        break;

    /* ── URL bar ── */
    case 'g': case 'o': case 'l':
        b->url_bar_active = true;
        strncpy(b->url_buf, tab->url, MAX_URL_LEN - 1);
        b->url_cursor = (int)strlen(b->url_buf);
        curs_set(1);
        break;

    /* ── Search ── */
    case '/':
        b->search_active = true;
        b->search_buf[0] = '\0';
        curs_set(1);
        break;

    case 'n':
        search_next(b);
        break;
    case 'N':
        search_prev(b);
        break;

    /* ── Navigation ── */
    case '\n': case KEY_ENTER:
        if (tab->link_count > 0 && tab->focused_link < tab->link_count) {
            const char *href = tab->links[tab->focused_link].url;
            if (href[0]) {
                tab->links[tab->focused_link].visited = true;
                browser_navigate(b, href);
            }
        }
        break;

    case KEY_BTAB:  /* Shift+Tab */
        focus_prev_link(b);
        break;

    case '\t': case KEY_DOWN:
        focus_next_link(b);
        break;

    case KEY_UP:
        focus_prev_link(b);
        break;

    /* Scroll */
    case ' ': case KEY_NPAGE:
        scroll_down(b, b->term_rows - 4);
        break;

    case 'b': case KEY_PPAGE:
        if (b->url_bar_active) break;
        scroll_up(b, b->term_rows - 4);
        break;

    case 'j':
        scroll_down(b, 3);
        break;

    case 'k':
        scroll_up(b, 3);
        break;

    case KEY_HOME: case 'G':
        if (ch == KEY_HOME)
            tab->scroll_pos = 0;
        else
            tab->scroll_pos = tab->line_count - (b->term_rows - 3);
        if (tab->scroll_pos < 0) tab->scroll_pos = 0;
        break;

    case KEY_END:
        tab->scroll_pos = tab->line_count - (b->term_rows - 3);
        if (tab->scroll_pos < 0) tab->scroll_pos = 0;
        break;

    /* ── Back / Forward with Alt+arrow ── */
    case KEY_LEFT:
        browser_go_back(b);
        break;
    case KEY_RIGHT:
        browser_go_forward(b);
        break;

    /* ── Reload ── */
    case 'r': case KEY_F(5):
        browser_reload(b);
        break;

    /* ── Tabs ── */
    case 't':
        browser_new_tab(b, "about:home");
        break;

    case 'w':
        browser_close_tab(b, b->active_tab);
        break;

    case '[':
        browser_switch_tab(b, (b->active_tab - 1 + b->tab_count) % b->tab_count);
        break;

    case ']':
        browser_switch_tab(b, (b->active_tab + 1) % b->tab_count);
        break;

    /* Tab shortcuts 1-9 */
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9': {
        int idx = ch - '1';
        if (idx < b->tab_count) browser_switch_tab(b, idx);
        break;
    }

    /* ── Bookmarks ── */
    case 'B': {
        browser_new_tab(b, "about:bookmarks");
        break;
    }

    //case 'b':
     //   /* Add bookmark for current page */
     //   if (b->bookmark_count < MAX_BOOKMARKS && tab->url[0]) {
    //        Bookmark *bk = &b->bookmarks[b->bookmark_count++];
    //        strncpy(bk->url,   tab->url,   MAX_URL_LEN - 1);
      //      strncpy(bk->title, tab->title, MAX_TITLE_LEN - 1);
     //       ui_set_status(b, "★ Bookmarked: %s", tab->title[0] ? tab->title : tab->url);
     //   }
     //   break;

    /* ── Help ── */
    case '?': case KEY_F(1):
        b->help_open = !b->help_open;
        break;

    case 27: /* Escape */
        b->help_open  = false;
        b->menu_open  = false;
        b->search_active = false;
        break;

    /* ── Resize ── */
    case KEY_RESIZE:
        ui_resize(b);
        break;

    default:
        break;
    }
}