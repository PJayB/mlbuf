#include "test.h"

MAIN("hello\nworld",
    char* data;
    bint_t data_len;
    mark_move_beginning(cur);
    mark_delete_after(cur, 1);
    buffer_get(buf, &data, &data_len);
    ASSERT("dela", 0, strncmp("ello\nworld", data, data_len));
)
