#include "draw.h"
#include "font.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *src, int width, int cap, const char *expected)
{
    char *out = malloc(cap > 0 ? (size_t)cap : 1);
    assert(out);
    out[0] = '!';
    assert(glyph_text_elide(src, width, out, cap) == out);
    if (cap <= 0)
        assert(out[0] == '!');
    else {
        assert(memchr(out, '\0', (size_t)cap));
        if (expected)
            assert(strcmp(out, expected) == 0);
    }
    free(out);
}

static void run(void)
{
    const char *ellipsis = "\xe2\x80\xa6";
    check("fits", 10000, 8, "fits");
    check(NULL, 100, 4, "");
    check("", 100, 1, "");
    check("abcdef", 10000, 4, "abc");
    check("\xc3\xa9!", 10000, 2, "");
    check("\xc3\xa9!", 10000, 3, "\xc3\xa9");
    check("\xe2\x82\xac!", 10000, 3, "");
    check("\xe2\x82\xac!", 10000, 4, "\xe2\x82\xac");
    check("\xf0\x9f\x98\x80!", 10000, 4, "");
    check("\xf0\x9f\x98\x80!", 10000, 5, "\xf0\x9f\x98\x80");
    check("abcdefghijklmnopqrstuvwxyz", glyph_text_width("ab\xe2\x80\xa6"),
          16, "ab\xe2\x80\xa6");
    check("abcdef", glyph_text_width(ellipsis) - 1, 16, "");
    for (int cap = 1; cap < 4; cap++)
        check("abcdef", 1, cap, "");
    assert(glyph_text_elide("abc", 10, NULL, 3) == NULL);

    /* Original overflow: four-byte character at the end of a 256-byte buffer. */
    char text[266], expected[255];
    memset(text, 'A', 251);
    memcpy(text + 251, "\xf0\x9f\x98\x80" "0123456789", 15);
    memset(expected, 'A', 251);
    memcpy(expected + 251, ellipsis, 4);
    check(text, glyph_text_width(text) - 1, 256, expected);

    /* Every byte capacity around a character/marker boundary, including
     * incomplete and malformed input, must remain bounded and terminated. */
    const char *samples[] = {
        "", "abcdef", "\xc3\xa9!", "\xe2\x82\xac!", "\xf0\x9f\x98\x80!",
        "\xf0", "A\xe2\x82", "A\xf0(\x8c(", "\xffxyz"
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
        for (int cap = 0; cap <= 9; cap++)
            for (int width = -1; width <= 100; width++)
                check(samples[i], width, cap, NULL);
}

int main(int argc, char **argv)
{
    run();
    if (argc > 1 && argv[1][0]) {
        g_font_ui = font_load(argv[1]);
        assert(g_font_ui);
        run();
    }
    puts("Glyph text bounds: PASS");
    return 0;
}
