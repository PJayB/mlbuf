#include "test.h"

MAIN("hello\nworld",
    bline_t* line;
    size_t col;

    buffer_get_bline_col(buf, 0, &line, &col);
    ASSERT("0line", buf->first_line, line);
    ASSERT("0col", 0, col);

    buffer_get_bline_col(buf, 1, &line, &col);
    ASSERT("1line", buf->first_line, line);
    ASSERT("1col", 1, col);

    buffer_get_bline_col(buf, 5, &line, &col);
    ASSERT("5line", buf->first_line, line);
    ASSERT("5col", 5, col);

    buffer_get_bline_col(buf, 6, &line, &col);
    ASSERT("6line", buf->first_line->next, line);
    ASSERT("6col", 0, col);

    buffer_get_bline_col(buf, 99, &line, &col);
    ASSERT("oobline", buf->first_line->next, line);
    ASSERT("oobcol", 5, col);
)

