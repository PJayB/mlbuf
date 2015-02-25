#include "test.h"

MAIN("hello\nworld",
    size_t i;
    srule_t* srule1;
    srule_t* srule2;

    srule1 = srule_new_single("world", sizeof("world")-1, 1, 2);
    srule2 = srule_new_multi("lo", sizeof("lo")-1, "wo", sizeof("wo")-1, 3, 4);

    buffer_add_srule(buf, srule1);
    for (i = 0; i < buf->first_line->char_count; i++) {
        ASSERT("line1fg", 0, buf->first_line->char_styles[i].fg);
        ASSERT("line1bg", 0, buf->first_line->char_styles[i].bg);
    }
    for (i = 0; i < buf->first_line->next->char_count; i++) {
        ASSERT("line2fg", 1, buf->first_line->next->char_styles[i].fg);
        ASSERT("line2bg", 2, buf->first_line->next->char_styles[i].bg);
    }

    buffer_remove_srule(buf, srule1);
    buffer_add_srule(buf, srule2);
    for (i = 0; i < buf->first_line->char_count; i++) {
        ASSERT("line1fg_m", (i == 3 || i == 4 ? 3 : 0), buf->first_line->char_styles[i].fg);
        ASSERT("line1bg_m", (i == 3 || i == 4 ? 4 : 0), buf->first_line->char_styles[i].bg);
    }
    for (i = 0; i < buf->first_line->next->char_count; i++) {
        ASSERT("line2fg_m", (i == 0 || i == 1 ? 3 : 0), buf->first_line->next->char_styles[i].fg);
        ASSERT("line2bg_m", (i == 0 || i == 1 ? 4 : 0), buf->first_line->next->char_styles[i].bg);
    }

    srule_destroy(srule1);
    srule_destroy(srule2);
)
