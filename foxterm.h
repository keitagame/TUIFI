#ifndef FOXTERM_H
#define FOXTERM_H

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <json-c/json.h>

/* ── Version ── */
#define FOXTERM_VERSION "1.0.0"
#define FOXTERM_NAME    "FoxTerm"

/* ── Limits ── */
#define MAX_URL_LEN       2048
#define MAX_TITLE_LEN     256
#define MAX_STATUS_LEN    512
#define MAX_HISTORY       64
#define MAX_BOOKMARKS     128
#define MAX_LINKS         1024
#define MAX_LINE_LEN      4096
#define MAX_RENDERED_LINES 65536
#define MAX_SEARCH_LEN    256
#define MAX_INPUT_LEN     2048
#define TAB_MAX           16

/* ── Marionette (Firefox remote protocol) ── */
#define MARIONETTE_DEFAULT_PORT 2828
#define MARIONETTE_TIMEOUT_SEC  5

/* ── Color pairs ── */
#define CLR_NORMAL       1
#define CLR_HEADER       2
#define CLR_STATUS       3
#define CLR_LINK         4
#define CLR_LINK_VISITED 5
#define CLR_LINK_HOVER   6
#define CLR_INPUT_BAR    7
#define CLR_HIGHLIGHT    8
#define CLR_H1           9
#define CLR_H2          10
#define CLR_H3          11
#define CLR_CODE        12
#define CLR_QUOTE       13
#define CLR_ERROR       14
#define CLR_SUCCESS     15
#define CLR_TAB_ACTIVE  16
#define CLR_TAB_INACTIVE 17
#define CLR_PROGRESS    18
#define CLR_MENU        19
#define CLR_MENU_SEL    20

/* ── Rendered element types ── */
typedef enum {
    ELEM_TEXT = 0,
    ELEM_LINK,
    ELEM_H1, ELEM_H2, ELEM_H3, ELEM_H4, ELEM_H5, ELEM_H6,
    ELEM_HR,
    ELEM_CODE,
    ELEM_PRE,
    ELEM_BLOCKQUOTE,
    ELEM_LI,
    ELEM_IMG,
    ELEM_INPUT,
    ELEM_FORM,
    ELEM_TABLE_CELL,
    ELEM_BLANK,
} ElemType;

typedef struct {
    char     text[MAX_LINE_LEN];
    char     href[MAX_URL_LEN];  /* for links */
    ElemType type;
    int      attr;               /* ncurses attributes */
    int      color_pair;
    int      indent;
    bool     is_link;
    int      link_id;            /* index into links[] */
} RenderedLine;

/* ── Link ── */
typedef struct {
    char url[MAX_URL_LEN];
    char text[256];
    bool visited;
} Link;

/* ── History entry ── */
typedef struct {
    char url[MAX_URL_LEN];
    char title[MAX_TITLE_LEN];
    int  scroll_pos;
} HistoryEntry;

/* ── Bookmark ── */
typedef struct {
    char url[MAX_URL_LEN];
    char title[MAX_TITLE_LEN];
    char tag[64];
} Bookmark;

/* ── Tab ── */
typedef struct {
    char            url[MAX_URL_LEN];
    char            title[MAX_TITLE_LEN];
    RenderedLine   *lines;
    int             line_count;
    Link            links[MAX_LINKS];
    int             link_count;
    int             scroll_pos;
    int             focused_link;
    bool            loading;
    bool            is_marionette;  /* page loaded via Firefox */
    char            status[MAX_STATUS_LEN];
    /* history within this tab */
    HistoryEntry    history[MAX_HISTORY];
    int             hist_pos;
    int             hist_len;
} Tab;

/* ── Marionette connection ── */
typedef struct {
    int  fd;           /* TCP socket fd, -1 if not connected */
    int  port;
    int  msg_id;
    bool connected;
    char firefox_pid_str[32];
} MarionetteConn;

/* ── Browser state ── */
typedef struct {
    /* Tabs */
    Tab     tabs[TAB_MAX];
    int     tab_count;
    int     active_tab;

    /* Bookmarks */
    Bookmark bookmarks[MAX_BOOKMARKS];
    int      bookmark_count;

    /* UI state */
    int      term_rows;
    int      term_cols;
    bool     url_bar_active;
    bool     search_active;
    bool     menu_open;
    bool     bookmarks_open;
    bool     help_open;
    bool     running;
    char     url_buf[MAX_URL_LEN];
    char     search_buf[MAX_SEARCH_LEN];
    int      url_cursor;
    int      search_pos;    /* current search match index */
    int     *search_matches;
    int      search_match_count;

    /* Firefox / Marionette */
    MarionetteConn marionette;
    bool           firefox_available;

    /* Windows */
    WINDOW *win_tabs;
    WINDOW *win_urlbar;
    WINDOW *win_content;
    WINDOW *win_status;

    /* Curl */
    CURL   *curl;

    /* Rendering scratch buffer */
    RenderedLine *render_buf;
    int           render_buf_cap;
} Browser;

/* ── Function declarations ── */

/* browser.c */
Browser *browser_new(void);
void     browser_free(Browser *b);
void     browser_run(Browser *b);
void     browser_navigate(Browser *b, const char *url);
void     browser_go_back(Browser *b);
void     browser_go_forward(Browser *b);
void     browser_reload(Browser *b);
void     browser_new_tab(Browser *b, const char *url);
void     browser_close_tab(Browser *b, int idx);
void     browser_switch_tab(Browser *b, int idx);

/* fetch.c */
typedef struct {
    char  *data;
    size_t size;
} FetchResult;

FetchResult *fetch_url(const char *url, const char *user_agent);
void         fetch_result_free(FetchResult *r);

/* marionette.c */
bool marionette_connect(MarionetteConn *m, int port);
void marionette_disconnect(MarionetteConn *m);
bool marionette_navigate(MarionetteConn *m, const char *url);
char *marionette_get_title(MarionetteConn *m);
char *marionette_get_page_source(MarionetteConn *m);
char *marionette_eval(MarionetteConn *m, const char *script);
bool marionette_click_element(MarionetteConn *m, const char *css_selector);
bool marionette_find_firefox(int *port_out);

/* render.c */
void render_html(Browser *b, Tab *tab, const char *html, const char *base_url);
void render_text(Browser *b, Tab *tab, const char *text);
void render_error(Browser *b, Tab *tab, const char *msg);
void render_about_page(Browser *b, Tab *tab);
void render_bookmarks_page(Browser *b, Tab *tab);

/* ui.c */
void ui_init(Browser *b);
void ui_cleanup(void);
void ui_draw(Browser *b);
void ui_draw_tabs(Browser *b);
void ui_draw_urlbar(Browser *b);
void ui_draw_content(Browser *b);
void ui_draw_statusbar(Browser *b);
void ui_draw_menu(Browser *b);
void ui_draw_help(Browser *b);
void ui_draw_bookmarks(Browser *b);
void ui_set_status(Browser *b, const char *fmt, ...);
void ui_resize(Browser *b);

/* input.c */
void input_handle(Browser *b, int ch);
void input_url_bar(Browser *b, int ch);
void input_search(Browser *b, int ch);
void input_menu(Browser *b, int ch);

/* util.c */
char *url_resolve(const char *base, const char *rel);
char *url_encode(const char *s);
char *html_decode_entities(const char *s);
void  str_trim(char *s);
char *str_dup(const char *s);
bool  str_startswith(const char *s, const char *prefix);
void  word_wrap(const char *text, int width, char ***lines_out, int *count_out);
char *xpath_get_text(xmlDoc *doc, const char *xpath_expr);

#endif /* FOXTERM_H */