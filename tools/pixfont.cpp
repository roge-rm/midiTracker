// Converts a pixel TTF into a 1-bit PixFont header (see include/ui_look.h).
//
//   g++ -O2 -o build/pixfont tools/pixfont.cpp $(pkg-config --cflags --libs freetype2)
//   build/pixfont FONT.ttf NAME PIXELS_X64 [--hint] [--fix FILE] [--preview] > NAME.h
//
// The headers in include/fonts/ were made with
//   build/pixfont Silkscreen-Regular.ttf FONT_LABEL 512 > include/fonts/silkscreen_8.h
//   build/pixfont VT323-Regular.ttf FONT_TEXT 768 --hint --fix tools/fonts/vt323_12_fixes.txt > include/fonts/vt323_12.h
// from the TTFs in github.com/google/fonts (ofl/silkscreen, ofl/vt323).
//
// PIXELS_X64 is the em size in 1/64 pixel, so a font drawn on an 80 unit
// grid in a 1000 unit em takes 800 (12.5 px), one on a 125 unit grid 512.
// Glyphs are printable ASCII, plus U+00B7 (middle dot) in the 0x7F slot.
// Rendering is monochrome, so nothing is antialiased. --hint runs the
// font's own hinting, which snaps VT323's stems at 12 px. --fix replaces
// glyphs with hand drawn ones from FILE (see tools/fonts/*_fixes.txt).
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>

struct Fix { int xo, yo; std::vector<std::string> rows; };

// "= C XO YO" starts a glyph, rows of '#' and '.' follow, '#' lines at the
// start of a line that isn't a row are comments.
static std::map<int, Fix> loadFixes(const char* path) {
    std::map<int, Fix> fixes;
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(1);
    }
    char line[256];
    int cur = -1;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '=') {
            char c;
            Fix fx;
            if (sscanf(line, "= %c %d %d", &c, &fx.xo, &fx.yo) == 3) {
                cur = (unsigned char)c;
                fixes[cur] = fx;
            }
        } else if (cur >= 0 && line[0] && strspn(line, "#.") == strlen(line) &&
                   (strchr(line, '.') || fixes[cur].rows.size() || strlen(line) <= 8)) {
            if (line[0] == '#' && line[1] == ' ') continue;
            fixes[cur].rows.push_back(line);
        } else if (!line[0]) {
            cur = -1;
        }
    }
    fclose(f);
    return fixes;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s FONT.ttf NAME PIXELS_X64 [--preview]\n", argv[0]);
        return 1;
    }
    const char* name = argv[2];
    long size64 = strtol(argv[3], nullptr, 10);
    bool preview = false, hint = false;
    std::map<int, Fix> fixes;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--preview") == 0) preview = true;
        else if (strcmp(argv[i], "--hint") == 0) hint = true;
        else if (strcmp(argv[i], "--fix") == 0 && i + 1 < argc) fixes = loadFixes(argv[++i]);
    }

    FT_Library lib;
    FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[1], 0, &face)) {
        fprintf(stderr, "can't open %s\n", argv[1]);
        return 1;
    }
    FT_Set_Char_Size(face, 0, size64, 72, 72);

    struct Glyph { int offset, w, h, xo, yo, adv; };
    std::vector<Glyph> glyphs;
    std::vector<unsigned char> bits;
    int ascent = 0, descent = 0;
    int bitPos = 0;
    unsigned char acc = 0;

    for (int c = 0x20; c <= 0x7F; c++) {
        unsigned long code = c == 0x7F ? 0xB7 : (unsigned long)c;
        Glyph g = {(int)bits.size(), 0, 0, 0, 0, 0};
        if (bitPos) {
            bits.push_back(acc);
            acc = 0;
            bitPos = 0;
            g.offset = (int)bits.size();
        }
        int flags = FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | (hint ? 0 : FT_LOAD_NO_HINTING);
        if (FT_Load_Char(face, code, flags)) {
            glyphs.push_back(g);
            continue;
        }
        FT_GlyphSlot s = face->glyph;
        FT_Bitmap b = s->bitmap;
        g.w = (int)b.width;
        g.h = (int)b.rows;
        g.xo = s->bitmap_left;
        g.yo = -s->bitmap_top;
        g.adv = (int)((s->advance.x + 32) >> 6);
        // A hand fix is drawn into its own buffer in the same 1-bit layout.
        std::vector<unsigned char> fixBuf;
        auto fit = fixes.find(c);
        if (fit != fixes.end()) {
            const Fix& fx = fit->second;
            g.h = (int)fx.rows.size();
            g.w = 0;
            for (auto& r : fx.rows) if ((int)r.size() > g.w) g.w = (int)r.size();
            g.xo = fx.xo;
            g.yo = fx.yo;
            int pitch = (g.w + 7) / 8;
            fixBuf.assign(pitch * g.h, 0);
            for (int y = 0; y < g.h; y++)
                for (int x = 0; x < (int)fx.rows[y].size(); x++)
                    if (fx.rows[y][x] == '#') fixBuf[y * pitch + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
            b.buffer = fixBuf.data();
            b.pitch = pitch;
        }
        if (c == 'H') ascent = -g.yo;
        if (g.h > 0 && g.yo + g.h > descent) descent = g.yo + g.h;
        if (preview) printf("'%c' w%d h%d xo%d yo%d adv%d\n", c, g.w, g.h, g.xo, g.yo, g.adv);
        for (int y = 0; y < g.h; y++) {
            for (int x = 0; x < g.w; x++) {
                int on = (b.buffer[y * b.pitch + (x >> 3)] >> (7 - (x & 7))) & 1;
                if (preview) putchar(on ? '#' : '.');
                acc = (unsigned char)(acc | (on << (7 - bitPos)));
                if (++bitPos == 8) {
                    bits.push_back(acc);
                    acc = 0;
                    bitPos = 0;
                }
            }
            if (preview) putchar('\n');
        }
        glyphs.push_back(g);
    }
    if (bitPos) bits.push_back(acc);
    if (preview) {
        printf("cap %d descent %d bytes %zu\n", ascent, descent, bits.size());
        return 0;
    }

    printf("// Generated by tools/pixfont.cpp from %s, em %ld/64 px. SIL OFL 1.1, see OFL-*.txt.\n",
           strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1], size64);
    printf("#pragma once\n#include \"ui_look.h\"\n\n");
    printf("static const uint8_t %s_bits[] = {", name);
    for (size_t i = 0; i < bits.size(); i++) printf("%s0x%02X,", i % 16 ? "" : "\n    ", bits[i]);
    printf("\n};\n\n");
    printf("static const Look::PixGlyph %s_glyphs[] = {\n", name);
    for (size_t i = 0; i < glyphs.size(); i++) {
        const Glyph& g = glyphs[i];
        printf("    {%d, %d, %d, %d, %d, %d}, // 0x%02zX\n", g.offset, g.w, g.h, g.xo, g.yo, g.adv, i + 0x20);
    }
    printf("};\n\n");
    printf("namespace Look {\nextern const PixFont %s;\nconst PixFont %s = {%s_bits, %s_glyphs, 0x20, 0x7F, %d, %d};\n}\n",
           name, name, name, name, ascent, descent);
    return 0;
}
