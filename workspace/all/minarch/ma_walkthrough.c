#include "ma_internal.h"
#include "ma_frontend_opts.h"
#include "ma_game.h"
#include "ma_walkthrough.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Walkthrough viewer
//
// The file is read once (lazily, on first open), normalized to clean UTF-8 and
// wrapped into display lines for the current font and screen width. Anything
// persisted (reading position, bookmarks) is a byte offset into the normalized
// text rather than a line number, so it survives re-wrapping when the text size
// changes or the same SD card is used on a device with a different screen.

#define WALKTHROUGH_EXT       "txt"
#define WALKTHROUGH_MAX_SIZE  (16 * 1024 * 1024) // the largest GameFAQs guides are ~2MB
#define WALKTHROUGH_TAB_WIDTH 4
#define WALKTHROUGH_MAX_DISPLAY_PATHS 8
#define WALKTHROUGH_MAX_BOOKMARKS 100
#define WALKTHROUGH_TOAST_MS  1500

enum {
	TEXT_SIZE_SMALL,
	TEXT_SIZE_MEDIUM,
	TEXT_SIZE_LARGE,
	TEXT_SIZE_COUNT,
};

typedef struct {
	uint32_t start; // byte offset into wt.text
	uint32_t len;   // in bytes, excluding the newline
} WalkthroughLine;

static struct {
	char path[MAX_PATH];       // matched walkthrough file, empty if there is none
	char title[MAX_PATH];      // file name without directory and extension
	char state_path[MAX_PATH]; // where position and bookmarks are kept

	char* text; // normalized, NUL terminated
	uint32_t size;
	int loaded; // 0: not yet, 1: ok, -1: failed

	WalkthroughLine* lines;
	int line_count;
	int line_cap;
	TTF_Font* layout_font; // what `lines` was wrapped for
	int layout_width;

	TTF_Font* advance_font; // what `advances` was measured for
	int advances[256];

	// persisted
	uint32_t position; // offset of the top visible line
	int text_size;
	uint32_t bookmarks[WALKTHROUGH_MAX_BOOKMARKS]; // offsets, sorted ascending
	int bookmark_count;

	// transient feedback shown in the header, eg. "Bookmark added"
	const char* toast;
	uint32_t toast_until;
} wt;

///////////////////////////////
// lifecycle

void Walkthrough_init(void) {
	Walkthrough_free();
	memset(&wt, 0, sizeof(wt));
	wt.text_size = TEXT_SIZE_MEDIUM;

	if (!Game_findSidecar(core.walkthroughs_dir, WALKTHROUGH_EXT, wt.path)) {
		LOG_info("No walkthrough file found\n");
		return;
	}
	LOG_info("Found walkthrough: %s\n", wt.path);

	char* name = strrchr(wt.path, '/');
	strcpy(wt.title, name ? name + 1 : wt.path);
	char* ext = strrchr(wt.title, '.');
	if (ext) *ext = '\0';

	// same per-game directory the menu uses for save slots, see Menu_init
	char emu_name[256];
	getEmuName(game.path, emu_name);
	snprintf(wt.state_path, sizeof(wt.state_path), SHARED_USERDATA_PATH "/.minui/%s/%s.walkthrough", emu_name, game.name);
}

void Walkthrough_free(void) {
	free(wt.text);
	free(wt.lines);
	wt.text = NULL;
	wt.lines = NULL;
	wt.size = 0;
	wt.line_count = 0;
	wt.line_cap = 0;
	wt.layout_font = NULL;
	wt.loaded = 0;
}

bool Walkthrough_available(void) {
	return wt.path[0] != '\0';
}

///////////////////////////////
// persisted state

// keeps bookmarks sorted and unique, returns 0 if there was no room
static int Walkthrough_addBookmark(uint32_t offset) {
	if (offset > wt.size) return 1; // stale, the file got shorter
	int i = 0;
	while (i < wt.bookmark_count && wt.bookmarks[i] < offset) i++;
	if (i < wt.bookmark_count && wt.bookmarks[i] == offset) return 1;
	if (wt.bookmark_count == WALKTHROUGH_MAX_BOOKMARKS) return 0;
	memmove(&wt.bookmarks[i + 1], &wt.bookmarks[i], (wt.bookmark_count - i) * sizeof(uint32_t));
	wt.bookmarks[i] = offset;
	wt.bookmark_count += 1;
	return 1;
}

static void Walkthrough_removeBookmark(int i) {
	memmove(&wt.bookmarks[i], &wt.bookmarks[i + 1], (wt.bookmark_count - i - 1) * sizeof(uint32_t));
	wt.bookmark_count -= 1;
}

static void Walkthrough_readState(void) {
	FILE* file = fopen(wt.state_path, "r");
	if (!file) return;

	char line[256];
	while (fgets(line, sizeof(line), file)) {
		unsigned long value;
		if (sscanf(line, "position=%lu", &value) == 1) wt.position = value;
		else if (sscanf(line, "text_size=%lu", &value) == 1 && value < TEXT_SIZE_COUNT) wt.text_size = value;
		else if (sscanf(line, "bookmark=%lu", &value) == 1) Walkthrough_addBookmark(value);
	}
	fclose(file);

	// the file may have been replaced with a shorter one
	if (wt.position > wt.size) wt.position = wt.size;
}

static void Walkthrough_writeState(void) {
	// .minui/<EMU> normally exists already, but don't count on it (eg. fresh desktop sdcard)
	char dir[MAX_PATH];
	strcpy(dir, wt.state_path);
	char* slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		char* parent = strrchr(dir, '/');
		if (parent) {
			*parent = '\0';
			mkdir(dir, 0755);
			*parent = '/';
		}
		mkdir(dir, 0755);
	}

	FILE* file = fopen(wt.state_path, "w");
	if (!file) {
		LOG_error("Couldn't write walkthrough state: %s\n", wt.state_path);
		return;
	}
	fprintf(file, "position=%u\n", wt.position);
	fprintf(file, "text_size=%i\n", wt.text_size);
	for (int i = 0; i < wt.bookmark_count; i++) {
		fprintf(file, "bookmark=%u\n", wt.bookmarks[i]);
	}
	fclose(file);
}

///////////////////////////////
// loading

static int isValidUTF8(const unsigned char* s, size_t n) {
	size_t i = 0;
	while (i < n) {
		unsigned char c = s[i];
		int len;
		if (c < 0x80) len = 1;
		else if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
		else if ((c & 0xF0) == 0xE0) len = 3;
		else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
		else return 0;
		if (i + len > n) return 0;
		for (int j = 1; j < len; j++) {
			if ((s[i + j] & 0xC0) != 0x80) return 0;
		}
		i += len;
	}
	return 1;
}

// Windows-1252 punctuation that commonly shows up in old text files, as ascii
static const char* cp1252ToAscii(unsigned char c) {
	switch (c) {
		case 0x82: return ",";
		case 0x84: return ",,";
		case 0x85: return "...";
		case 0x8B: return "<";
		case 0x9B: return ">";
		case 0x91: case 0x92: return "'";
		case 0x93: case 0x94: return "\"";
		case 0x95: return "*";
		case 0x96: case 0x97: return "-";
		case 0x99: return "(TM)";
		default: return "?";
	}
}

// Normalize to UTF-8 with \n line endings, tabs expanded and control characters
// dropped. Text that isn't valid UTF-8 is assumed to be Windows-1252/Latin-1.
static char* normalizeText(const unsigned char* in, size_t n, uint32_t* out_size) {
	if (n >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) { // BOM
		in += 3;
		n -= 3;
	}
	int legacy = !isValidUTF8(in, n);
	if (legacy) LOG_info("Walkthrough is not UTF-8, reading as Windows-1252\n");

	// worst case: every byte is a tab
	size_t cap = n * WALKTHROUGH_TAB_WIDTH + 1;
	char* out = malloc(cap);
	if (!out) return NULL;

	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = in[i];
		if (c == '\r') {
			out[o++] = '\n';
			if (i + 1 < n && in[i + 1] == '\n') i++;
		}
		else if (c == '\n') out[o++] = '\n';
		else if (c == '\t') {
			for (int j = 0; j < WALKTHROUGH_TAB_WIDTH; j++) out[o++] = ' ';
		}
		else if (c < 0x20 || c == 0x7F) continue; // eg. form feeds
		else if (c < 0x80 || !legacy) out[o++] = c;
		else if (c < 0xA0) {
			const char* s = cp1252ToAscii(c);
			while (*s) out[o++] = *s++;
		}
		else { // latin-1 maps directly onto U+00A0..U+00FF
			out[o++] = 0xC0 | (c >> 6);
			out[o++] = 0x80 | (c & 0x3F);
		}
	}
	out[o] = '\0';
	*out_size = o;
	return out;
}

static int Walkthrough_load(void) {
	if (wt.loaded) return wt.loaded > 0;
	wt.loaded = -1;

	FILE* file = fopen(wt.path, "rb");
	if (!file) {
		LOG_error("Couldn't open walkthrough: %s\n", wt.path);
		return 0;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size < 0 || size > WALKTHROUGH_MAX_SIZE) {
		LOG_error("Walkthrough too large: %s (%li bytes)\n", wt.path, size);
		fclose(file);
		return 0;
	}

	unsigned char* raw = malloc(size + 1);
	if (!raw) {
		fclose(file);
		return 0;
	}
	size_t read = fread(raw, 1, size, file);
	fclose(file);

	wt.text = normalizeText(raw, read, &wt.size);
	free(raw);
	if (!wt.text) return 0;

	LOG_info("Loaded walkthrough: %s (%u bytes)\n", wt.path, wt.size);
	Walkthrough_readState();
	wt.loaded = 1;
	return 1;
}

///////////////////////////////
// layout

static uint32_t decodeUTF8(const char* s, size_t n, int* len) {
	const unsigned char* u = (const unsigned char*)s;
	if (u[0] < 0x80 || n < 2) { *len = 1; return u[0]; }
	if ((u[0] & 0xE0) == 0xC0) { *len = 2; return ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); }
	if ((u[0] & 0xF0) == 0xE0 && n >= 3) { *len = 3; return ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); }
	if (n >= 4) { *len = 4; return ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); }
	*len = 1;
	return '?';
}

static int glyphAdvance(TTF_Font* f, uint32_t cp) {
	int advance = 0;
	if (cp < 256) return wt.advances[cp];
	if (cp > 0xFFFF || TTF_GlyphMetrics(f, (Uint16)cp, NULL, NULL, NULL, NULL, &advance) != 0) return wt.advances['?'];
	return advance;
}

// Summing advances ignores kerning, but measuring every candidate line with
// TTF_SizeUTF8 is far too slow for a multi-megabyte guide on device.
static void measureAdvances(TTF_Font* f) {
	if (wt.advance_font == f) return;
	for (int cp = 0; cp < 256; cp++) {
		int advance = 0;
		if (cp >= 0x20 && TTF_GlyphMetrics(f, cp, NULL, NULL, NULL, NULL, &advance) != 0) advance = 0;
		wt.advances[cp] = advance;
	}
	wt.advance_font = f;
}

static void pushLine(uint32_t start, uint32_t len) {
	if (wt.line_count == wt.line_cap) {
		int cap = wt.line_cap ? wt.line_cap * 2 : 1024;
		WalkthroughLine* lines = realloc(wt.lines, cap * sizeof(WalkthroughLine));
		if (!lines) return; // out of memory, drop the line rather than crash
		wt.lines = lines;
		wt.line_cap = cap;
	}
	wt.lines[wt.line_count++] = (WalkthroughLine){ start, len };
}

// eg. "=====" or "-----", which plain text guides use as separators
static int isRule(uint32_t start, uint32_t end) {
	const char* t = wt.text;
	if (end - start < 3 || (unsigned char)t[start] >= 0x80 || isalnum((unsigned char)t[start]) || t[start] == ' ') return 0;
	for (uint32_t i = start + 1; i < end; i++) {
		if (t[i] != t[start]) return 0;
	}
	return 1;
}

// word wrap one source line (start..end, exclusive of the newline)
static void wrapLine(TTF_Font* f, int max_width, uint32_t start, uint32_t end) {
	const char* t = wt.text;
	if (start == end) {
		pushLine(start, 0);
		return;
	}
	if (isRule(start, end)) {
		// wrapping a separator just makes noise, clip it to the screen instead
		int advance = MAX(1, glyphAdvance(f, (unsigned char)t[start]));
		pushLine(start, MIN(end - start, (uint32_t)(max_width / advance)));
		return;
	}
	while (start < end) {
		int width = 0;
		int seen_word = 0;
		uint32_t i = start;
		uint32_t brk = 0; // just past the last space that follows a word
		while (i < end) {
			int len;
			uint32_t cp = decodeUTF8(t + i, end - i, &len);
			int advance = glyphAdvance(f, cp);
			if (width + advance > max_width && i > start) break;
			width += advance;
			i += len;
			if (cp == ' ') {
				if (seen_word) brk = i;
			}
			else seen_word = 1;
		}
		if (i >= end) {
			pushLine(start, end - start);
			return;
		}
		uint32_t split = brk > start ? brk : i; // otherwise a word wider than the screen, hard break it
		pushLine(start, split - start);
		start = split;
		while (start < end && t[start] == ' ') start++;
	}
}

static void Walkthrough_layout(TTF_Font* f, int max_width) {
	if (wt.layout_font == f && wt.layout_width == max_width) return;

	measureAdvances(f);
	wt.line_count = 0;
	uint32_t p = 0;
	while (p < wt.size) {
		uint32_t eol = p;
		while (eol < wt.size && wt.text[eol] != '\n') eol++;
		wrapLine(f, max_width, p, eol);
		p = eol + 1;
	}
	wt.layout_font = f;
	wt.layout_width = max_width;
	LOG_info("Walkthrough laid out into %i lines\n", wt.line_count);
}

// index of the display line containing byte offset `position`
static int Walkthrough_lineAt(uint32_t position) {
	int lo = 0;
	int hi = wt.line_count - 1;
	if (hi < 0) return 0;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (wt.lines[mid].start <= position) lo = mid;
		else hi = mid - 1;
	}
	return lo;
}

///////////////////////////////
// viewer

static TTF_Font* Walkthrough_font(void) {
	switch (wt.text_size) {
		case TEXT_SIZE_SMALL: return font.tiny;
		case TEXT_SIZE_LARGE: return font.medium;
		default: return font.small;
	}
}

static Uint32 Walkthrough_accentColor(void) {
	SDL_Color c = uintToColour(THEME_COLOR2_255);
	return SDL_MapRGB(screen->format, c.r, c.g, c.b);
}

static void Walkthrough_toast(const char* message) {
	wt.toast = message;
	wt.toast_until = SDL_GetTicks() + WALKTHROUGH_TOAST_MS;
}

static void Walkthrough_blitText(TTF_Font* f, const char* text, SDL_Color color, int x, int y) {
	// nothing to draw for blank lines (TTF errors on empty strings)
	const char* c = text;
	while (*c == ' ') c++;
	if (!*c) return;

	SDL_Surface* surface = TTF_RenderUTF8_Blended(f, text, color);
	if (!surface) return;
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){ x, y });
	SDL_FreeSurface(surface);
}

static void Walkthrough_blitLine(TTF_Font* f, const WalkthroughLine* line, int x, int y) {
	static char buffer[4096];
	uint32_t len = line->len < sizeof(buffer) ? line->len : sizeof(buffer) - 1;
	memcpy(buffer, wt.text + line->start, len);
	buffer[len] = '\0';
	Walkthrough_blitText(f, buffer, COLOR_WHITE, x, y);
}

// index into wt.bookmarks of a bookmark on display line `line`, or -1
static int Walkthrough_bookmarkOnLine(int line) {
	for (int i = 0; i < wt.bookmark_count; i++) {
		if (Walkthrough_lineAt(wt.bookmarks[i]) == line) return i;
	}
	return -1;
}

static void Walkthrough_toggleBookmark(int top) {
	int i = Walkthrough_bookmarkOnLine(top);
	if (i >= 0) {
		Walkthrough_removeBookmark(i);
		Walkthrough_toast("Bookmark removed");
	}
	else if (Walkthrough_addBookmark(wt.lines[top].start)) {
		Walkthrough_toast("Bookmark added");
	}
	else {
		Walkthrough_toast("Too many bookmarks");
		return;
	}
	Walkthrough_writeState();
}

// display line of the closest bookmark before (direction < 0) or after `top`, or -1
static int Walkthrough_adjacentBookmark(int top, int direction) {
	int found = -1;
	for (int i = 0; i < wt.bookmark_count; i++) {
		int line = Walkthrough_lineAt(wt.bookmarks[i]);
		if (direction < 0 && line < top) found = line; // sorted, so keep the last one
		if (direction > 0 && line > top) return line;
	}
	return found;
}

// the source line a bookmark points into, for the bookmark list
static void Walkthrough_bookmarkLabel(uint32_t offset, char* out, size_t out_size) {
	uint32_t start = Walkthrough_lineAt(offset) < wt.line_count ? wt.lines[Walkthrough_lineAt(offset)].start : offset;
	while (start < wt.size && wt.text[start] == ' ') start++;
	uint32_t end = start;
	while (end < wt.size && wt.text[end] != '\n' && end - start < out_size - 1) end++;
	// don't cut a multi-byte character in half
	while (end > start && end < wt.size && ((unsigned char)wt.text[end] & 0xC0) == 0x80) end--;
	if (end == start) {
		snprintf(out, out_size, "(blank line)");
		return;
	}
	memcpy(out, wt.text + start, end - start);
	out[end - start] = '\0';
}

// returns the display line to jump to, or -1 to stay put
static int Walkthrough_bookmarkList(int top) {
	if (!wt.bookmark_count) {
		Menu_message("No bookmarks yet.\n\nPress X while reading to bookmark\nthe line at the top of the screen.", (char*[]){ "B","BACK", NULL });
		GFX_setMode(MODE_MAIN); // Menu_message switches back to MODE_MENU
		return -1;
	}

	// start on the bookmark at or just above where we are
	int selected = 0;
	for (int i = 0; i < wt.bookmark_count; i++) {
		if (Walkthrough_lineAt(wt.bookmarks[i]) <= top) selected = i;
	}

	int row_height = SCALE1(BUTTON_SIZE);
	int list_y = SCALE1(PADDING + PILL_SIZE);
	int max_rows = MAX(1, (screen->h - list_y - SCALE1(PADDING + PILL_SIZE + PADDING / 2)) / row_height);
	int start = 0;
	int result = -1;
	int dirty = 1;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		int count = wt.bookmark_count;
		if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justPressed(BTN_A)) {
			result = Walkthrough_lineAt(wt.bookmarks[selected]);
			break;
		}
		else if (PAD_justPressed(BTN_X)) {
			Walkthrough_removeBookmark(selected);
			Walkthrough_writeState();
			if (!wt.bookmark_count) break;
			if (selected >= wt.bookmark_count) selected = wt.bookmark_count - 1;
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_UP)) {
			selected = (selected - 1 + count) % count;
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_DOWN)) {
			selected = (selected + 1) % count;
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_L1)) {
			selected = MAX(0, selected - max_rows);
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_RIGHT) || PAD_justRepeated(BTN_R1)) {
			selected = MIN(count - 1, selected + max_rows);
			dirty = 1;
		}

		// keep the selection on screen
		if (selected < start) start = selected;
		if (selected >= start + max_rows) start = selected - max_rows + 1;
		start = MAX(0, MIN(start, wt.bookmark_count - max_rows));

		PWR_update(&dirty, NULL, Menu_beforeSleep, Menu_afterSleep);

		if (dirty) {
			GFX_clear(screen);

			char header[64];
			snprintf(header, sizeof(header), "Bookmarks (%i)", wt.bookmark_count);
			Walkthrough_blitText(font.tiny, header, COLOR_LIGHT_TEXT, SCALE1(PADDING), SCALE1(PADDING) + (SCALE1(PILL_SIZE) - TTF_FontHeight(font.tiny)) / 2);

			int percent_w = 0;
			TTF_SizeUTF8(font.small, "100%", &percent_w, NULL);
			int label_x = SCALE1(PADDING + BUTTON_MARGIN * 2) + percent_w;
			int label_w = screen->w - label_x - SCALE1(PADDING + BUTTON_MARGIN);

			for (int i = start, row = 0; i < wt.bookmark_count && row < max_rows; i++, row++) {
				int y = list_y + row * row_height;
				SDL_Color color = COLOR_WHITE;
				if (i == selected) {
					GFX_blitPillDark(ASSET_BUTTON, screen, &(SDL_Rect){ SCALE1(PADDING), y, screen->w - SCALE1(PADDING * 2), row_height });
					color = uintToColour(THEME_COLOR5_255);
				}

				char percent[8];
				snprintf(percent, sizeof(percent), "%u%%", (unsigned)((uint64_t)wt.bookmarks[i] * 100 / (MAX(1, wt.size)))); // MAX isn't parenthesized
				int w = 0;
				TTF_SizeUTF8(font.small, percent, &w, NULL);
				Walkthrough_blitText(font.small, percent, color, label_x - SCALE1(BUTTON_MARGIN) - w, y + SCALE1(1));

				char label[256];
				char truncated[256];
				Walkthrough_bookmarkLabel(wt.bookmarks[i], label, sizeof(label));
				GFX_truncateText(font.small, label, truncated, label_w, 0);
				Walkthrough_blitText(font.small, truncated, color, label_x, y + SCALE1(1));
			}

			GFX_blitButtonGroup((char*[]){ "X","DELETE", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "A","GO TO", "B","BACK", NULL }, 1, screen, 1);

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();

		hdmimon();
	}
	return result;
}

static void Walkthrough_view(void) {
	GFX_setMode(MODE_MAIN);

	int dirty = 1;
	int top = -1; // resolved from wt.position once laid out
	wt.toast = NULL;

	while (1) {
		GFX_startFrame();
		PAD_poll();

		// geometry, recomputed every frame since the text size can change
		TTF_Font* f = Walkthrough_font();
		int line_height = TTF_FontLineSkip(f);
		int scrollbar_width = SCALE1(2);
		int text_x = SCALE1(PADDING);
		int text_y = SCALE1(PADDING + PILL_SIZE);
		int text_w = screen->w - text_x - SCALE1(PADDING) - scrollbar_width - SCALE1(PADDING) / 2;
		int text_h = screen->h - text_y - SCALE1(PADDING + PILL_SIZE + PADDING / 2);
		int visible = MAX(1, text_h / line_height);

		Walkthrough_layout(f, text_w);
		if (top < 0) top = Walkthrough_lineAt(wt.position);
		int max_top = MAX(0, wt.line_count - visible);
		int page = MAX(1, visible - 1); // keep a line of context when paging
		int prev_top = top;

		if (PAD_justPressed(BTN_B)) break;
		else if (PAD_justRepeated(BTN_UP)) top -= 1;
		else if (PAD_justRepeated(BTN_DOWN)) top += 1;
		else if (PAD_justRepeated(BTN_L1) || PAD_justRepeated(BTN_LEFT)) top -= page;
		else if (PAD_justRepeated(BTN_R1) || PAD_justRepeated(BTN_RIGHT)) top += page;
		else if (PAD_justPressed(BTN_L2) || PAD_justPressed(BTN_R2)) {
			int line = Walkthrough_adjacentBookmark(top, PAD_justPressed(BTN_L2) ? -1 : 1);
			if (line >= 0) top = line;
			else Walkthrough_toast(wt.bookmark_count ? "No more bookmarks" : "No bookmarks yet");
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_X)) {
			Walkthrough_toggleBookmark(top);
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_Y)) {
			wt.position = wt.lines[top].start;
			int line = Walkthrough_bookmarkList(top);
			if (line >= 0) top = line;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_SELECT)) {
			// re-wrap at the new size, keeping the same text at the top
			wt.text_size = (wt.text_size + 1) % TEXT_SIZE_COUNT;
			top = -1;
			dirty = 1;
			continue;
		}

		top = MAX(0, MIN(top, max_top));
		if (top != prev_top) dirty = 1;
		wt.position = wt.lines[top].start;

		if (wt.toast && SDL_GetTicks() >= wt.toast_until) {
			wt.toast = NULL;
			dirty = 1;
		}

		PWR_update(&dirty, NULL, Menu_beforeSleep, Menu_afterSleep);

		if (dirty) {
			GFX_clear(screen);
			Uint32 accent = Walkthrough_accentColor();

			// header: title, feedback and how far through we are
			char percent[16];
			snprintf(percent, sizeof(percent), "%i%%", max_top ? (top * 100) / max_top : 100);
			int percent_w = 0;
			TTF_SizeUTF8(font.tiny, percent, &percent_w, NULL);
			int header_y = SCALE1(PADDING) + (SCALE1(PILL_SIZE) - TTF_FontHeight(font.tiny)) / 2;
			Walkthrough_blitText(font.tiny, percent, COLOR_LIGHT_TEXT, screen->w - SCALE1(PADDING) - percent_w, header_y);

			int title_w = screen->w - SCALE1(PADDING * 3) - percent_w;
			if (wt.toast) {
				int toast_w = 0;
				TTF_SizeUTF8(font.tiny, wt.toast, &toast_w, NULL);
				Walkthrough_blitText(font.tiny, wt.toast, COLOR_WHITE, (screen->w - toast_w) / 2, header_y);
				title_w = (screen->w - toast_w) / 2 - SCALE1(PADDING * 2);
			}
			char title[MAX_PATH];
			GFX_truncateText(font.tiny, wt.title, title, title_w, 0);
			Walkthrough_blitText(font.tiny, title, COLOR_LIGHT_TEXT, SCALE1(PADDING), header_y);

			// body
			for (int i = 0; i < visible && top + i < wt.line_count; i++) {
				Walkthrough_blitLine(f, &wt.lines[top + i], text_x, text_y + i * line_height);
			}

			// bookmarked lines get a mark in the left margin
			for (int i = 0; i < wt.bookmark_count; i++) {
				int row = Walkthrough_lineAt(wt.bookmarks[i]) - top;
				if (row < 0 || row >= visible) continue;
				int mark_h = TTF_FontHeight(f) * 3 / 4;
				int mark_y = text_y + row * line_height + (TTF_FontHeight(f) - mark_h) / 2;
				SDL_FillRect(screen, &(SDL_Rect){ (text_x - SCALE1(3)) / 2, mark_y, SCALE1(3), mark_h }, accent);
			}

			// scrollbar, with a tick for each bookmark
			if (max_top) {
				int track_x = screen->w - SCALE1(PADDING) - scrollbar_width;
				int thumb_h = MAX(SCALE1(8), text_h * visible / wt.line_count);
				int thumb_y = text_y + (text_h - thumb_h) * top / max_top;
				SDL_FillRect(screen, &(SDL_Rect){ track_x, text_y, scrollbar_width, text_h }, SDL_MapRGB(screen->format, TRIAD_GRAY));
				for (int i = 0; i < wt.bookmark_count; i++) {
					int tick_y = text_y + (int)((int64_t)(text_h - SCALE1(2)) * Walkthrough_lineAt(wt.bookmarks[i]) / (MAX(1, wt.line_count - 1)));
					SDL_FillRect(screen, &(SDL_Rect){ track_x - SCALE1(2), tick_y, scrollbar_width + SCALE1(4), SCALE1(2) }, accent);
				}
				SDL_FillRect(screen, &(SDL_Rect){ track_x, thumb_y, scrollbar_width, thumb_h }, SDL_MapRGB(screen->format, TRIAD_WHITE));
			}

			// paging (L1/R1, left/right) and jumping between bookmarks (L2/R2) are in the readme
			int marked = Walkthrough_bookmarkOnLine(top) >= 0;
			GFX_blitButtonGroup((char*[]){ "X", marked ? "UNMARK" : "MARK", "Y","MARKS", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "SELECT","SIZE", "B","BACK", NULL }, 1, screen, 1);

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();

		hdmimon();
	}

	Walkthrough_writeState();
	GFX_setMode(MODE_MENU);
}

static void Walkthrough_showMissing(void) {
	char paths[SIDECAR_MAX_PATHS][MAX_PATH];
	int count = 0;
	Game_getSidecarPaths(core.walkthroughs_dir, WALKTHROUGH_EXT, paths, &count);

	char message[(WALKTHROUGH_MAX_DISPLAY_PATHS + 2) * MAX_PATH] = "No walkthrough file found, looked for:\n";
	int shown = 0;
	for (int i = 0; i < count && shown < WALKTHROUGH_MAX_DISPLAY_PATHS; i++) {
		// eg. the rom's own extension shows up twice
		int duplicate = 0;
		for (int j = 0; j < i && !duplicate; j++) duplicate = !strcmp(paths[i], paths[j]);
		if (duplicate) continue;

		// skip the sdcard prefix, it's the same for every path
		const char* p = paths[i];
		if (!strncmp(p, SDCARD_PATH, strlen(SDCARD_PATH))) p += strlen(SDCARD_PATH);
		strcat(message, "\n");
		strcat(message, p);
		shown++;
	}
	Menu_messageWithFont(message, (char*[]){ "B","BACK", NULL }, font.small);
}

int OptionWalkthrough_openMenu(MenuList* list, int i) {
	if (!Walkthrough_available()) {
		Walkthrough_showMissing();
	}
	else if (!Walkthrough_load()) {
		Menu_message("Couldn't read walkthrough file.", (char*[]){ "B","BACK", NULL });
	}
	else if (wt.size == 0) {
		Menu_message("Walkthrough file is empty.", (char*[]){ "B","BACK", NULL });
	}
	else {
		Walkthrough_view();
	}
	return MENU_CALLBACK_NOP;
}
