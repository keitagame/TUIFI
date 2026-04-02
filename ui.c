/*
 * ui.c — ncurses TUI rendering for FoxTerm
 */

#include "foxterm.h"
#include <stdarg.h>
#include <wchar.h>
#include <locale.h>

/* ────────────────────────────────────────────────────────────────
 * Initialization
 * ──────────────────────────────────────────────────────────────── */

void ui_init(Browser *b)
{
    setlocale(LC_ALL, "");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    set_escdelay(50);
    curs_set(0);
    nodelay(stdscr, FALSE);

    if (has_colors()) {
        start_color();
        use_default_colors();

        /* Firefox-inspired dark theme */
        /* Background: dark charcoal (#1a1a2e → dark)  */
        /* Accent:     Firefox orange (#ff9400)         */
        /* Link:       bright blue                       */

        init_pair(CLR_NORMAL,       COLOR_WHITE,   -1);
        init_pair(CLR_HEADER,       COLOR_BLACK,   COLOR_CYAN);
        init_pair(CLR_STATUS,       COLOR_BLACK,   COLOR_BLUE);
        init_pair(CLR_LINK,         COLOR_CYAN,    -1);
        init_pair(CLR_LINK_VISITED, COLOR_MAGENTA, -1);
        init_pair(CLR_LINK_HOVER,   COLOR_BLACK,   COLOR_CYAN);
        init_pair(CLR_INPUT_BAR,    COLOR_BLACK,   COLOR_WHITE);
        init_pair(CLR_HIGHLIGHT,    COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CLR_H1,           COLOR_YELLOW,  -1);
        init_pair(CLR_H2,           COLOR_CYAN,    -1);
        init_pair(CLR_H3,           COLOR_GREEN,   -1);
        init_pair(CLR_CODE,         COLOR_GREEN,   -1);
        init_pair(CLR_QUOTE,        COLOR_CYAN,    -1);
        init_pair(CLR_ERROR,        COLOR_RED,     -1);
        init_pair(CLR_SUCCESS,      COLOR_GREEN,   -1);
        init_pair(CLR_TAB_ACTIVE,   COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CLR_TAB_INACTIVE, COLOR_WHITE,   COLOR_BLACK);
        init_pair(CLR_PROGRESS,     COLOR_BLACK,   COLOR_GREEN);
        init_pair(CLR_MENU,         COLOR_WHITE,   COLOR_BLACK);
        init_pair(CLR_MENU_SEL,     COLOR_BLACK,   COLOR_WHITE);
    }

    getmaxyx(stdscr, b->term_rows, b->term_cols);
    refresh();
}

void ui_cleanup(void)
{
    endwin();
}

void ui_resize(Browser *b)
{
    endwin();
    refresh();
    clear();
    getmaxyx(stdscr, b->term_rows, b->term_cols);
}

/* ────────────────────────────────────────────────────────────────
 * Status bar message
 * ──────────────────────────────────────────────────────────────── */

void ui_set_status(Browser *b, const char *fmt, ...)
{
    Tab *tab = &b->tabs[b->active_tab];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tab->status, MAX_STATUS_LEN, fmt, ap);
    va_end(ap);
}

/* ────────────────────────────────────────────────────────────────
 * Draw tab bar (row 0)
 * ──────────────────────────────────────────────────────────────── */

void ui_draw_tabs(Browser *b)
{
    int row = 0;
    move(row, 0);
    attron(COLOR_PAIR(CLR_TAB_INACTIVE));
    for (int c = 0; c < b->term_cols; c++) addch(' ');
    attroff(COLOR_PAIR(CLR_TAB_INACTIVE));

    /* Logo */
    attron(COLOR_PAIR(CLR_TAB_ACTIVE) | A_BOLD);
    mvprintw(row, 1, " 🦊 FoxTerm ");
    attroff(COLOR_PAIR(CLR_TAB_ACTIVE) | A_BOLD);

    int x = 13;
    for (int i = 0; i < b->tab_count && x < b->term_cols - 4; i++) {
        Tab *t = &b->tabs[i];
        char label[32];
        const char *title = t->title[0] ? t->title : (t->url[0] ? t->url : "New Tab");
        snprintf(label, sizeof(label), " %.18s ", title);

        if (i == b->active_tab) {
            attron(COLOR_PAIR(CLR_TAB_ACTIVE) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CLR_TAB_INACTIVE));
        }
        mvprintw(row, x, "%s", label);
        x += (int)strlen(label);
        if (i == b->active_tab) {
            attroff(COLOR_PAIR(CLR_TAB_ACTIVE) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(CLR_TAB_INACTIVE));
        }

        /* Separator */
        attron(COLOR_PAIR(CLR_TAB_INACTIVE) | A_DIM);
        mvprintw(row, x++, "│");
        attroff(COLOR_PAIR(CLR_TAB_INACTIVE) | A_DIM);
    }

    /* Firefox backend indicator */
    if (b->firefox_available) {
        const char *ind = " [FF] ";
        attron(COLOR_PAIR(CLR_SUCCESS) | A_BOLD);
        mvprintw(row, b->term_cols - (int)strlen(ind) - 1, "%s", ind);
        attroff(COLOR_PAIR(CLR_SUCCESS) | A_BOLD);
    }
}

/* ────────────────────────────────────────────────────────────────
 * Draw URL bar (row 1)
 * ──────────────────────────────────────────────────────────────── */

void ui_draw_urlbar(Browser *b)
{
    int row = 1;
    Tab *tab = &b->tabs[b->active_tab];

    /* Navigation buttons */
    attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);
    mvprintw(row, 0, " ◀ ▶ ↺ ");
    attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);

    int btn_width = 7;
    int bar_width = b->term_cols - btn_width - 2;

    /* URL input area */
    if (b->url_bar_active) {
        attron(COLOR_PAIR(CLR_INPUT_BAR) | A_BOLD);
        /* Draw URL bar background */
        for (int c = btn_width; c < b->term_cols - 2; c++)
            mvaddch(row, c, ' ');
        /* Show cursor and text */
        int vis_start = 0;
        int cursor = b->url_cursor;
        if (cursor > bar_width - 2) vis_start = cursor - (bar_width - 2);
        mvprintw(row, btn_width, "%.*s", bar_width - 1,
                 b->url_buf + vis_start);
        /* Show cursor */
        int cur_col = btn_width + (cursor - vis_start);
        if (cur_col < b->term_cols - 2) {
            mvchgat(row, cur_col, 1, A_REVERSE, CLR_INPUT_BAR, NULL);
        }
        curs_set(1);
        attroff(COLOR_PAIR(CLR_INPUT_BAR) | A_BOLD);
    } else {
        /* Show current URL */
        attron(COLOR_PAIR(CLR_INPUT_BAR));
        for (int c = btn_width; c < b->term_cols - 2; c++)
            mvaddch(row, c, ' ');

        /* Protocol icon */
        const char *url = tab->url;
        const char *icon = "  ";
        if (str_startswith(url, "https://")) icon = "🔒";
        else if (str_startswith(url, "http://")) icon = "⚠ ";
        else if (str_startswith(url, "about:")) icon = "ℹ ";

        int disp_width = bar_width - 3;
        mvprintw(row, btn_width, "%s %-*.*s",
                 icon, disp_width, disp_width, url);
        attroff(COLOR_PAIR(CLR_INPUT_BAR));
        curs_set(0);
    }

    /* Loading indicator */
    if (tab->loading) {
        static const char *spinner[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
        static int spin_idx = 0;
        attron(COLOR_PAIR(CLR_PROGRESS) | A_BOLD);
        mvprintw(row, b->term_cols - 3, "%s", spinner[(spin_idx++) % 10]);
        attroff(COLOR_PAIR(CLR_PROGRESS) | A_BOLD);
    }
}

/* ────────────────────────────────────────────────────────────────
 * Draw content area (rows 2..term_rows-2)
 * ──────────────────────────────────────────────────────────────── */

void ui_draw_content(Browser *b)
{
    Tab *tab = &b->tabs[b->active_tab];
    int content_rows = b->term_rows - 3;  /* tab + urlbar + statusbar */
    int start_row = 2;

    /* Clear content area */
    for (int r = start_row; r < start_row + content_rows; r++) {
        move(r, 0);
        clrtoeol();
    }

    if (tab->loading) {
        /* Loading animation */
        int mid = start_row + content_rows / 2;
        const char *msg = "  Loading…";
        attron(COLOR_PAIR(CLR_H2) | A_BOLD);
        mvprintw(mid, (b->term_cols - (int)strlen(msg)) / 2, "%s", msg);
        attroff(COLOR_PAIR(CLR_H2) | A_BOLD);
        return;
    }

    /* Find focused link row */
    int focused_link = tab->focused_link;

    for (int i = 0; i < content_rows; i++) {
        int line_idx = tab->scroll_pos + i;
        int screen_row = start_row + i;

        if (line_idx >= tab->line_count) break;

        RenderedLine *l = &tab->lines[line_idx];

        /* Determine if this line contains the focused link */
        bool is_focused = (l->is_link && l->link_id == focused_link);

        /* Indentation */
        int col = l->indent * 2;

        /* HR: draw full-width line */
        if (l->type == ELEM_HR) {
            attron(COLOR_PAIR(CLR_NORMAL) | A_DIM);
            move(screen_row, 0);
            for (int c = 0; c < b->term_cols; c++) addch(ACS_HLINE);
            attroff(COLOR_PAIR(CLR_NORMAL) | A_DIM);
            continue;
        }

        /* Blank line */
        if (l->type == ELEM_BLANK) continue;

        /* Choose attributes */
        int attr = l->attr;
        int clr  = l->color_pair;

        if (is_focused) {
            clr  = CLR_LINK_HOVER;
            attr = A_BOLD | A_REVERSE;
        } else if (l->is_link) {
            /* Check if visited */
            if (l->link_id < tab->link_count && tab->links[l->link_id].visited)
                clr = CLR_LINK_VISITED;
            else
                clr = CLR_LINK;
            attr |= A_UNDERLINE;
        }

        /* Search highlight */
        if (b->search_active && b->search_buf[0]) {
            /* Will be handled per-char — for now just use normal */
        }

        /* Pre: monospace style */
        if (l->type == ELEM_PRE || l->type == ELEM_CODE) {
            attr |= A_DIM;
            clr = CLR_CODE;
        }

        /* Blockquote: left bar */
        if (l->indent > 0) {
            attron(COLOR_PAIR(CLR_QUOTE) | A_DIM);
            for (int d = 0; d < l->indent; d++) {
                mvaddch(screen_row, d * 2, ACS_VLINE);
                mvaddch(screen_row, d * 2 + 1, ' ');
            }
            attroff(COLOR_PAIR(CLR_QUOTE) | A_DIM);
        }

        /* Draw text */
        attron(COLOR_PAIR(clr) | attr);
        int avail = b->term_cols - col;
        if (avail < 1) avail = 1;

        char display[MAX_LINE_LEN + 1];
        int tlen = (int)strlen(l->text);
        if (tlen > avail) tlen = avail;
        memcpy(display, l->text, tlen);
        display[tlen] = '\0';

        mvprintw(screen_row, col, "%s", display);
        attroff(COLOR_PAIR(clr) | attr);

        /* Search highlight overlay */
        if (b->search_buf[0] && b->search_active) {
            char *found = strcasestr(l->text, b->search_buf);
            if (found) {
                int off = (int)(found - l->text) + col;
                int slen = (int)strlen(b->search_buf);
                if (off < b->term_cols) {
                    mvchgat(screen_row, off, slen,
                            A_BOLD | A_REVERSE, CLR_HIGHLIGHT, NULL);
                }
            }
        }
    }

    /* Scroll indicator */
    if (tab->line_count > content_rows) {
        int sb_height = content_rows;
        int sb_pos    = (int)((long)tab->scroll_pos * sb_height / tab->line_count);
        int sb_size   = (int)((long)content_rows * sb_height / tab->line_count);
        if (sb_size < 1) sb_size = 1;

        for (int r = 0; r < sb_height; r++) {
            int sc_row = start_row + r;
            if (r >= sb_pos && r < sb_pos + sb_size) {
                attron(COLOR_PAIR(CLR_H2) | A_BOLD);
                mvaddch(sc_row, b->term_cols - 1, ACS_BLOCK);
                attroff(COLOR_PAIR(CLR_H2) | A_BOLD);
            } else {
                attron(COLOR_PAIR(CLR_NORMAL) | A_DIM);
                mvaddch(sc_row, b->term_cols - 1, ACS_VLINE);
                attroff(COLOR_PAIR(CLR_NORMAL) | A_DIM);
            }
        }
    }
}

/* ────────────────────────────────────────────────────────────────
 * Status bar (bottom row)
 * ──────────────────────────────────────────────────────────────── */

void ui_draw_statusbar(Browser *b)
{
    int row = b->term_rows - 1;
    Tab *tab = &b->tabs[b->active_tab];

    move(row, 0);
    attron(COLOR_PAIR(CLR_STATUS));
    for (int c = 0; c < b->term_cols; c++) addch(' ');

    if (b->search_active) {
        /* Search bar */
        attroff(COLOR_PAIR(CLR_STATUS));
        attron(COLOR_PAIR(CLR_INPUT_BAR) | A_BOLD);
        mvprintw(row, 0, " / %s", b->search_buf);
        if (b->search_match_count > 0) {
            mvprintw(row, b->term_cols - 20, " Match %d/%d ",
                     b->search_pos + 1, b->search_match_count);
        } else if (b->search_buf[0]) {
            mvprintw(row, b->term_cols - 14, " Not found  ");
        }
        attroff(COLOR_PAIR(CLR_INPUT_BAR) | A_BOLD);
        curs_set(1);
    } else {
        /* Normal status */
        const char *msg = tab->status[0] ? tab->status
            : "Press '?' for help  |  'g' to open URL  |  'q' to quit";

        mvprintw(row, 1, "%-*.*s",
                 b->term_cols - 20,
                 b->term_cols - 20,
                 msg);

        /* Line / total indicator */
        char pos_str[32];
        int pct = tab->line_count > 0
            ? (int)((long)(tab->scroll_pos + (b->term_rows - 3)) * 100 / tab->line_count)
            : 100;
        if (pct > 100) pct = 100;
        snprintf(pos_str, sizeof(pos_str), " %d%% ", pct);
        mvprintw(row, b->term_cols - (int)strlen(pos_str) - 1, "%s", pos_str);

        attroff(COLOR_PAIR(CLR_STATUS));
    }
}

/* ────────────────────────────────────────────────────────────────
 * Help overlay
 * ──────────────────────────────────────────────────────────────── */

void ui_draw_help(Browser *b)
{
    int rows = b->term_rows;
    int cols = b->term_cols;
    int h = 22, w = 60;
    int y = (rows - h) / 2;
    int x = (cols - w) / 2;

    /* Shadow */
    attron(COLOR_PAIR(CLR_NORMAL) | A_DIM);
    for (int r = y+1; r < y+h+1; r++)
        mvhline(r, x+2, ' ', w+2);
    attroff(COLOR_PAIR(CLR_NORMAL) | A_DIM);

    /* Box */
    attron(COLOR_PAIR(CLR_MENU));
    for (int r = y; r < y+h; r++) {
        mvhline(r, x, ' ', w);
    }
    box(newwin(h, w, y, x), 0, 0);

    /* Title */
    attron(COLOR_PAIR(CLR_TAB_ACTIVE) | A_BOLD);
    mvprintw(y, x + (w - 20) / 2, "  🦊 FoxTerm Help  ");
    attroff(COLOR_PAIR(CLR_TAB_ACTIVE) | A_BOLD);

    const char *items[] = {
        "  g/o       Open URL bar",
        "  Enter     Follow link",
        "  Tab/↓↑    Navigate links",
        "  Alt+←/→   Back / Forward",
        "  r/F5      Reload",
        "  /         Find in page",
        "  n/N       Next/prev match",
        "  t         New tab",
        "  w         Close tab",
        "  1-9       Switch tab",
        "  [/]       Prev/Next tab",
        "  b         Bookmark page",
        "  B         Show bookmarks",
        "  ?         Help (this)",
        "  q         Quit",
        NULL
    };

    attron(COLOR_PAIR(CLR_NORMAL));
    for (int i = 0; items[i]; i++) {
        mvprintw(y + 2 + i, x + 1, "%-*s", w - 2, items[i]);
    }
    attroff(COLOR_PAIR(CLR_NORMAL));

    /* Footer */
    attron(COLOR_PAIR(CLR_H2) | A_DIM);
    mvprintw(y + h - 2, x + (w - 24) / 2, "  Press any key to close  ");
    attroff(COLOR_PAIR(CLR_H2) | A_DIM);

    attroff(COLOR_PAIR(CLR_MENU));
}

/* ────────────────────────────────────────────────────────────────
 * Main draw entry point
 * ──────────────────────────────────────────────────────────────── */

void ui_draw(Browser *b)
{
    clear();
    ui_draw_tabs(b);
    ui_draw_urlbar(b);
    ui_draw_content(b);
    ui_draw_statusbar(b);

    if (b->help_open) ui_draw_help(b);

    refresh();
}