/*
 * render.c — HTML → TUI renderer
 *
 * Walks the libxml2 DOM tree and produces a flat list of RenderedLines
 * that the UI can display with ncurses.
 */

#include "foxterm.h"

/* ── Render context ── */
typedef struct {
    Browser      *b;
    Tab          *tab;
    const char   *base_url;
    RenderedLine *lines;
    int           count;
    int           cap;
    int           indent;
    int           list_depth;
    int           list_counter[8];  /* for ordered lists */
    bool          in_pre;
    bool          in_code;
    bool          in_head;
    bool          in_script;
    bool          in_style;
    char          cur_href[MAX_URL_LEN];
    bool          in_link;
} RCtx;

/* ── Ensure capacity ── */
static void rctx_grow(RCtx *r)
{
    if (r->count >= r->cap) {
        r->cap = r->cap ? r->cap * 2 : 256;
        r->lines = realloc(r->lines, r->cap * sizeof(RenderedLine));
    }
}

/* ── Append a rendered line ── */
static void emit(RCtx *r, ElemType type, int color, int attr,
                 const char *text, const char *href)
{
    rctx_grow(r);
    RenderedLine *l = &r->lines[r->count++];
    memset(l, 0, sizeof(*l));
    l->type       = type;
    l->color_pair = color;
    l->attr       = attr;
    l->indent     = r->indent;
    l->is_link    = (href && href[0]);

    if (text) {
        strncpy(l->text, text, MAX_LINE_LEN - 1);
    }
    if (href) {
        strncpy(l->href, href, MAX_URL_LEN - 1);
        l->link_id = r->tab->link_count;
    }
}

/* ── Emit blank line ── */
static void emit_blank(RCtx *r)
{
    if (r->count > 0 && r->lines[r->count-1].type == ELEM_BLANK) return;
    emit(r, ELEM_BLANK, CLR_NORMAL, 0, "", NULL);
}

/* ── Emit HR ── */
static void emit_hr(RCtx *r)
{
    emit(r, ELEM_HR, CLR_NORMAL, A_DIM, NULL, NULL);
}

/* ── Word-wrap text and emit ── */
static void emit_text_wrapped(RCtx *r, const char *text, int color, int attr,
                               ElemType type, const char *href, int width)
{
    if (!text || !*text) return;

    int avail = width - r->indent * 2 - 2;
    if (avail < 10) avail = 10;

    /* Normalise whitespace if not in <pre> */
    char *clean = NULL;
    if (!r->in_pre) {
        clean = malloc(strlen(text) + 1);
        if (!clean) return;
        int j = 0;
        bool ws = true;  /* skip leading space */
        for (const char *p = text; *p; p++) {
            if (isspace((unsigned char)*p)) {
                if (!ws) { clean[j++] = ' '; ws = true; }
            } else {
                clean[j++] = *p;
                ws = false;
            }
        }
        if (j > 0 && clean[j-1] == ' ') j--;
        clean[j] = '\0';
        if (!clean[0]) { free(clean); return; }
        text = clean;
    }

    /* Split into lines of `avail` characters */
    int len = (int)strlen(text);
    int pos = 0;
    while (pos < len) {
        /* Find break point */
        int end = pos + avail;
        if (end >= len) {
            end = len;
        } else {
            /* Back up to last space */
            int bp = end;
            while (bp > pos && !isspace((unsigned char)text[bp])) bp--;
            if (bp > pos) end = bp;
        }

        char linebuf[MAX_LINE_LEN];
        int chunk = end - pos;
        if (chunk >= MAX_LINE_LEN) chunk = MAX_LINE_LEN - 1;
        memcpy(linebuf, text + pos, chunk);
        linebuf[chunk] = '\0';

        /* Register link */
        if (href && href[0]) {
            if (r->tab->link_count < MAX_LINKS) {
                Link *lk = &r->tab->links[r->tab->link_count];
                strncpy(lk->url,  href,    MAX_URL_LEN - 1);
                strncpy(lk->text, linebuf, 255);
                lk->visited = false;
                r->tab->link_count++;
            }
        }

        emit(r, type, color, attr, linebuf, href);

        /* Skip whitespace after break */
        pos = end;
        while (pos < len && isspace((unsigned char)text[pos])) pos++;
    }

    free(clean);
}

/* ── Collect all text content of a node ── */
static void collect_text(xmlNode *node, char *buf, size_t cap)
{
    size_t used = strlen(buf);
    for (xmlNode *cur = node; cur; cur = cur->next) {
        if (cur->type == XML_TEXT_NODE && cur->content) {
            size_t clen = strlen((char *)cur->content);
            if (used + clen < cap - 1) {
                strcat(buf, (char *)cur->content);
                used += clen;
            }
        }
        collect_text(cur->children, buf, cap);
    }
}

/* ── Walk DOM tree ── */
static void walk(RCtx *r, xmlNode *node, int term_cols)
{
    for (xmlNode *cur = node; cur; cur = cur->next) {

        if (cur->type == XML_COMMENT_NODE) continue;

        if (cur->type == XML_TEXT_NODE) {
            if (r->in_head || r->in_script || r->in_style) continue;
            if (!cur->content) continue;

            const char *txt = (const char *)cur->content;

            if (r->in_pre) {
                /* Emit pre text verbatim, split on newlines */
                const char *line_start = txt;
                for (const char *p = txt; ; p++) {
                    if (*p == '\n' || *p == '\0') {
                        int len = (int)(p - line_start);
                        char linebuf[MAX_LINE_LEN];
                        if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
                        memcpy(linebuf, line_start, len);
                        linebuf[len] = '\0';
                        emit(r, ELEM_PRE, CLR_CODE, A_DIM, linebuf, NULL);
                        if (!*p) break;
                        line_start = p + 1;
                    }
                }
            } else {
                int color = r->in_link ? CLR_LINK : CLR_NORMAL;
                int attr  = r->in_link ? A_UNDERLINE : 0;
                const char *href = r->in_link ? r->cur_href : NULL;
                emit_text_wrapped(r, txt, color, attr, ELEM_TEXT, href, term_cols);
            }
            continue;
        }

        if (cur->type != XML_ELEMENT_NODE) {
            walk(r, cur->children, term_cols);
            continue;
        }

        const char *tag = (const char *)cur->name;
        if (!tag) continue;

        /* ── Block-level elements ── */
        if (!strcasecmp(tag, "head"))   { r->in_head = true;   walk(r, cur->children, term_cols); r->in_head   = false; continue; }
        if (!strcasecmp(tag, "script")) { r->in_script = true; walk(r, cur->children, term_cols); r->in_script = false; continue; }
        if (!strcasecmp(tag, "style"))  { r->in_style = true;  walk(r, cur->children, term_cols); r->in_style  = false; continue; }
        if (!strcasecmp(tag, "noscript")) continue;

        if (!strcasecmp(tag, "h1") || !strcasecmp(tag, "h2") ||
            !strcasecmp(tag, "h3") || !strcasecmp(tag, "h4") ||
            !strcasecmp(tag, "h5") || !strcasecmp(tag, "h6")) {
            emit_blank(r);
            char tbuf[MAX_LINE_LEN] = {0};
            collect_text(cur->children, tbuf, sizeof(tbuf));
            str_trim(tbuf);
            int level = tag[1] - '0';
            int clr   = (level == 1) ? CLR_H1 : (level == 2) ? CLR_H2 : CLR_H3;
            int at    = (level <= 2) ? (A_BOLD) : A_BOLD;
            /* Add prefix decoration */
            char decorated[MAX_LINE_LEN];
            if (level == 1)
                snprintf(decorated, sizeof(decorated), "══ %s ══", tbuf);
            else if (level == 2)
                snprintf(decorated, sizeof(decorated), "── %s", tbuf);
            else
                snprintf(decorated, sizeof(decorated), "  %s", tbuf);
            ElemType et = (level == 1) ? ELEM_H1 : (level == 2) ? ELEM_H2 : ELEM_H3;
            emit(r, et, clr, at, decorated, NULL);
            emit_blank(r);
            continue;
        }

        if (!strcasecmp(tag, "p") || !strcasecmp(tag, "div") ||
            !strcasecmp(tag, "section") || !strcasecmp(tag, "article") ||
            !strcasecmp(tag, "main") || !strcasecmp(tag, "header") ||
            !strcasecmp(tag, "footer") || !strcasecmp(tag, "aside")) {
            emit_blank(r);
            walk(r, cur->children, term_cols);
            emit_blank(r);
            continue;
        }

        if (!strcasecmp(tag, "br")) {
            emit(r, ELEM_BLANK, CLR_NORMAL, 0, "", NULL);
            continue;
        }

        if (!strcasecmp(tag, "hr")) {
            emit_blank(r);
            emit_hr(r);
            emit_blank(r);
            continue;
        }

        if (!strcasecmp(tag, "blockquote")) {
            emit_blank(r);
            r->indent++;
            walk(r, cur->children, term_cols);
            r->indent--;
            emit_blank(r);
            continue;
        }

        if (!strcasecmp(tag, "pre")) {
            emit_blank(r);
            r->in_pre = true;
            walk(r, cur->children, term_cols);
            r->in_pre = false;
            emit_blank(r);
            continue;
        }

        if (!strcasecmp(tag, "code") || !strcasecmp(tag, "kbd") ||
            !strcasecmp(tag, "samp") || !strcasecmp(tag, "tt")) {
            r->in_code = true;
            walk(r, cur->children, term_cols);
            r->in_code = false;
            continue;
        }

        if (!strcasecmp(tag, "ul") || !strcasecmp(tag, "ol")) {
            emit_blank(r);
            r->indent++;
            int depth = r->list_depth;
            if (depth < 8) {
                r->list_counter[depth] = 0;
                r->list_depth++;
            }
            walk(r, cur->children, term_cols);
            if (depth < 8) r->list_depth--;
            r->indent--;
            emit_blank(r);
            continue;
        }

        if (!strcasecmp(tag, "li")) {
            char prefix[16];
            int depth = r->list_depth > 0 ? r->list_depth - 1 : 0;
            r->list_counter[depth]++;
            /* Alternate bullet styles by depth */
            const char *bullets[] = {"• ", "◦ ", "▪ ", "▸ "};
            snprintf(prefix, sizeof(prefix), "%s", bullets[depth % 4]);

            /* Collect text for the list item */
            char lbuf[MAX_LINE_LEN] = {0};
            collect_text(cur->children, lbuf, sizeof(lbuf));
            str_trim(lbuf);

            char line[MAX_LINE_LEN];
            snprintf(line, sizeof(line), "%s%s", prefix, lbuf);
            emit(r, ELEM_LI, CLR_NORMAL, 0, line, NULL);

            /* Also walk children for nested lists */
            walk(r, cur->children, term_cols);
            continue;
        }

        /* ── Anchor ── */
        if (!strcasecmp(tag, "a")) {
            xmlChar *href_attr = xmlGetProp(cur, (xmlChar *)"href");
            if (href_attr) {
                char *resolved = url_resolve(r->base_url, (char *)href_attr);
                strncpy(r->cur_href, resolved ? resolved : (char *)href_attr,
                        MAX_URL_LEN - 1);
                free(resolved);
                xmlFree(href_attr);
                r->in_link = true;
            }
            walk(r, cur->children, term_cols);
            r->in_link = false;
            r->cur_href[0] = '\0';
            continue;
        }

        /* ── Images ── */
        if (!strcasecmp(tag, "img")) {
            xmlChar *alt = xmlGetProp(cur, (xmlChar *)"alt");
            xmlChar *src = xmlGetProp(cur, (xmlChar *)"src");
            char altbuf[256] = "[img]";
            if (alt && alt[0]) snprintf(altbuf, sizeof(altbuf), "[img: %s]", (char *)alt);
            char imgline[MAX_LINE_LEN];
            if (src) {
                char *resolved = url_resolve(r->base_url, (char *)src);
                snprintf(imgline, sizeof(imgline), "🖼  %s", altbuf);
                free(resolved);
            } else {
                snprintf(imgline, sizeof(imgline), "🖼  %s", altbuf);
            }
            emit(r, ELEM_IMG, CLR_NORMAL, A_DIM, imgline, NULL);
            if (alt) xmlFree(alt);
            if (src) xmlFree(src);
            continue;
        }

        /* ── Table ── */
        if (!strcasecmp(tag, "table")) {
            emit_blank(r);
            walk(r, cur->children, term_cols);
            emit_blank(r);
            continue;
        }
        if (!strcasecmp(tag, "tr")) {
            walk(r, cur->children, term_cols);
            continue;
        }
        if (!strcasecmp(tag, "td") || !strcasecmp(tag, "th")) {
            char tbuf[MAX_LINE_LEN] = {0};
            collect_text(cur->children, tbuf, sizeof(tbuf));
            str_trim(tbuf);
            int attr = !strcasecmp(tag, "th") ? A_BOLD : 0;
            char cell[MAX_LINE_LEN];
            snprintf(cell, sizeof(cell), "│ %s", tbuf);
            emit(r, ELEM_TABLE_CELL, CLR_NORMAL, attr, cell, NULL);
            continue;
        }

        /* ── Inline formatting ── */
        if (!strcasecmp(tag, "b") || !strcasecmp(tag, "strong")) {
            /* Walk children; text will pick up normal color but bold */
            walk(r, cur->children, term_cols);
            continue;
        }

        if (!strcasecmp(tag, "i") || !strcasecmp(tag, "em") ||
            !strcasecmp(tag, "cite") || !strcasecmp(tag, "q")) {
            walk(r, cur->children, term_cols);
            continue;
        }

        /* ── Title (in head) ── */
        if (!strcasecmp(tag, "title")) {
            char tbuf[MAX_TITLE_LEN] = {0};
            collect_text(cur->children, tbuf, sizeof(tbuf));
            str_trim(tbuf);
            strncpy(r->tab->title, tbuf, MAX_TITLE_LEN - 1);
            continue;
        }

        /* Default: recurse */
        walk(r, cur->children, term_cols);
    }
}

/* ────────────────────────────────────────────────────────────────
 * Public: render_html
 * ──────────────────────────────────────────────────────────────── */
void render_html(Browser *b, Tab *tab, const char *html, const char *base_url)
{
    tab->line_count = 0;
    tab->link_count = 0;
    tab->scroll_pos = 0;
    tab->focused_link = 0;

    if (!tab->lines) {
        tab->lines = calloc(MAX_RENDERED_LINES, sizeof(RenderedLine));
    }
    if (!tab->lines) return;

    /* Parse with libxml2 HTML parser */
    htmlParserCtxtPtr ctxt = htmlCreateMemoryParserCtxt(html, (int)strlen(html));
    if (!ctxt) { render_error(b, tab, "HTML parse failed"); return; }

    htmlCtxtUseOptions(ctxt,
        HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
        HTML_PARSE_NOWARNING | HTML_PARSE_NOBLANKS);

    htmlParseDocument(ctxt);
    xmlDoc *doc = ctxt->myDoc;
    htmlFreeParserCtxt(ctxt);

    if (!doc) { render_error(b, tab, "Empty document"); return; }

    RCtx r = {0};
    r.b        = b;
    r.tab      = tab;
    r.base_url = base_url ? base_url : "";
    r.cap      = 1024;
    r.lines    = malloc(r.cap * sizeof(RenderedLine));

    xmlNode *root = xmlDocGetRootElement(doc);
    walk(&r, root, b->term_cols);

    xmlFreeDoc(doc);

    /* Copy to tab */
    int copy_count = r.count < MAX_RENDERED_LINES ? r.count : MAX_RENDERED_LINES;
    memcpy(tab->lines, r.lines, copy_count * sizeof(RenderedLine));
    tab->line_count = copy_count;

    free(r.lines);
}

/* ── Render plain text ── */
void render_text(Browser *b, Tab *tab, const char *text)
{
    tab->line_count = 0;
    tab->link_count = 0;
    tab->scroll_pos = 0;

    if (!tab->lines)
        tab->lines = calloc(MAX_RENDERED_LINES, sizeof(RenderedLine));
    if (!tab->lines) return;

    const char *p = text;
    int idx = 0;
    while (*p && idx < MAX_RENDERED_LINES) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;
        char linebuf[MAX_LINE_LEN];
        memcpy(linebuf, p, len);
        linebuf[len] = '\0';

        RenderedLine *l = &tab->lines[idx++];
        memset(l, 0, sizeof(*l));
        l->type       = ELEM_TEXT;
        l->color_pair = CLR_NORMAL;
        strncpy(l->text, linebuf, MAX_LINE_LEN - 1);

        p = nl ? nl + 1 : p + strlen(p);
    }
    tab->line_count = idx;
}

/* ── Error page ── */
void render_error(Browser *b, Tab *tab, const char *msg)
{
    tab->line_count = 0;
    tab->link_count = 0;
    tab->scroll_pos = 0;

    if (!tab->lines)
        tab->lines = calloc(MAX_RENDERED_LINES, sizeof(RenderedLine));
    if (!tab->lines) return;

    const char *lines[] = {
        "",
        "  ╔══════════════════════════════════════╗",
        "  ║         FoxTerm — Page Error         ║",
        "  ╚══════════════════════════════════════╝",
        "",
        NULL
    };

    int idx = 0;
    for (int i = 0; lines[i]; i++) {
        RenderedLine *l = &tab->lines[idx++];
        memset(l, 0, sizeof(*l));
        l->type = ELEM_H1;
        l->color_pair = CLR_ERROR;
        l->attr = A_BOLD;
        strncpy(l->text, lines[i], MAX_LINE_LEN - 1);
    }

    /* Error message */
    RenderedLine *l = &tab->lines[idx++];
    memset(l, 0, sizeof(*l));
    l->type = ELEM_TEXT;
    l->color_pair = CLR_ERROR;
    char errbuf[MAX_LINE_LEN];
    snprintf(errbuf, sizeof(errbuf), "  Error: %s", msg ? msg : "Unknown error");
    strncpy(l->text, errbuf, MAX_LINE_LEN - 1);

    tab->line_count = idx;
    strncpy(tab->title, "Error", MAX_TITLE_LEN - 1);
}

/* ── About page ── */
void render_about_page(Browser *b, Tab *tab)
{
    tab->line_count = 0;
    tab->link_count = 0;
    tab->scroll_pos = 0;

    if (!tab->lines)
        tab->lines = calloc(MAX_RENDERED_LINES, sizeof(RenderedLine));
    if (!tab->lines) return;

    strncpy(tab->title, "About FoxTerm", MAX_TITLE_LEN - 1);

    const char *art[] = {
        "",
        "  ███████╗ ██████╗ ██╗  ██╗████████╗███████╗██████╗ ███╗   ███╗",
        "  ██╔════╝██╔═══██╗╚██╗██╔╝╚══██╔══╝██╔════╝██╔══██╗████╗ ████║",
        "  █████╗  ██║   ██║ ╚███╔╝    ██║   █████╗  ██████╔╝██╔████╔██║",
        "  ██╔══╝  ██║   ██║ ██╔██╗    ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║",
        "  ██║     ╚██████╔╝██╔╝ ██╗   ██║   ███████╗██║  ██║██║ ╚═╝ ██║",
        "  ╚═╝      ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝",
        "",
        "         Firefox-Backed TUI Web Browser  —  v" FOXTERM_VERSION,
        "",
        "  ─────────────────────── KEYBOARD SHORTCUTS ───────────────────────",
        "",
        "  Navigation:",
        "    g / o       Open URL bar",
        "    Enter       Follow focused link",
        "    Tab / ↓     Next link",
        "    Shift+Tab   Previous link",
        "    Alt+←       Back       Alt+→  Forward",
        "    r / F5      Reload",
        "    Escape      Cancel / close panel",
        "",
        "  Tabs:",
        "    t           New tab",
        "    w           Close current tab",
        "    1-9         Switch to tab N",
        "    [ / ]       Previous / Next tab",
        "",
        "  Search & Bookmarks:",
        "    /           Find in page",
        "    n / N       Next / prev search match",
        "    b           Add bookmark",
        "    B           Show bookmarks",
        "",
        "  Misc:",
        "    ?           This help / about page",
        "    q / Q       Quit FoxTerm",
        "",
        "  ─────────────────────── FIREFOX BACKEND ───────────────────────────",
        "",
        "  FoxTerm can use Firefox as its rendering engine via the Marionette",
        "  remote control protocol. Launch Firefox with:",
        "",
        "    firefox --marionette --headless",
        "",
        "  Then FoxTerm will automatically connect on port 2828.",
        "",
        NULL
    };

    int idx = 0;
    for (int i = 0; art[i] && idx < MAX_RENDERED_LINES; i++) {
        RenderedLine *l = &tab->lines[idx++];
        memset(l, 0, sizeof(*l));
        bool is_art = (i >= 1 && i <= 7);
        l->type       = is_art ? ELEM_H1 : ELEM_TEXT;
        l->color_pair = is_art ? CLR_H1 : CLR_NORMAL;
        l->attr       = is_art ? A_BOLD : 0;
        strncpy(l->text, art[i], MAX_LINE_LEN - 1);
    }
    tab->line_count = idx;
}

/* ── Bookmarks page ── */
void render_bookmarks_page(Browser *b, Tab *tab)
{
    tab->line_count = 0;
    tab->link_count = 0;
    tab->scroll_pos = 0;

    if (!tab->lines)
        tab->lines = calloc(MAX_RENDERED_LINES, sizeof(RenderedLine));
    if (!tab->lines) return;

    strncpy(tab->title, "Bookmarks", MAX_TITLE_LEN - 1);

    int idx = 0;

    auto void addline(ElemType t, int c, int a, const char *txt, const char *href);
    void addline(ElemType t, int c, int a, const char *txt, const char *href) {
        if (idx >= MAX_RENDERED_LINES) return;
        RenderedLine *l = &tab->lines[idx++];
        memset(l, 0, sizeof(*l));
        l->type = t; l->color_pair = c; l->attr = a;
        if (txt) strncpy(l->text, txt, MAX_LINE_LEN - 1);
        if (href) {
            strncpy(l->href, href, MAX_URL_LEN - 1);
            l->is_link = true;
            l->link_id = tab->link_count;
            if (tab->link_count < MAX_LINKS) {
                Link *lk = &tab->links[tab->link_count++];
                strncpy(lk->url,  href, MAX_URL_LEN - 1);
                strncpy(lk->text, txt ? txt : "", 255);
            }
        }
    }

    addline(ELEM_H1, CLR_H1, A_BOLD, "══ Bookmarks ══", NULL);
    addline(ELEM_BLANK, CLR_NORMAL, 0, "", NULL);

    if (b->bookmark_count == 0) {
        addline(ELEM_TEXT, CLR_NORMAL, A_DIM, "  No bookmarks yet. Press 'b' on any page to add one.", NULL);
    } else {
        for (int i = 0; i < b->bookmark_count; i++) {
            char line[MAX_LINE_LEN];
            snprintf(line, sizeof(line), "  ★ %s", b->bookmarks[i].title[0]
                     ? b->bookmarks[i].title : b->bookmarks[i].url);
            addline(ELEM_LINK, CLR_LINK, A_UNDERLINE, line, b->bookmarks[i].url);
            char urlline[MAX_LINE_LEN];
            snprintf(urlline, sizeof(urlline), "    %s", b->bookmarks[i].url);
            addline(ELEM_TEXT, CLR_NORMAL, A_DIM, urlline, NULL);
            addline(ELEM_BLANK, CLR_NORMAL, 0, "", NULL);
        }
    }

    tab->line_count = idx;
}