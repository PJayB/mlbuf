#include "test.h"

MAIN("lineA\n\nline2\nline3\n",
    char *data;
    bint_t data_len;

    buffer_replace(buf, 0, 0, "b", 1);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl1", 0, strncmp(data, "blineA\n\nline2\nline3\n", data_len));

    buffer_replace(buf, 3, 3, "xe0", 3);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl2", 0, strncmp(data, "blixe0\n\nline2\nline3\n", data_len));

    buffer_replace(buf, 10, 7, "N", 1);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl3", 0, strncmp(data, "blixe0\n\nliNe3\n", data_len));

    buffer_replace(buf, 5, 4, "jerk\nstuff", 10);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl4", 0, strncmp(data, "blixejerk\nstuffiNe3\n", data_len));

    buffer_replace(buf, 9, 99, "X", 1);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl5", 0, strncmp(data, "blixejerkX", data_len));

    buffer_replace(buf, 5, 0, "y\nb", 3);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl6", 0, strncmp(data, "blixey\nbjerkX", data_len));

    buffer_replace(buf, 0, 0, "\n", 1);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl7", 0, strncmp(data, "\nblixey\nbjerkX", data_len));

    buffer_replace(buf, 6, 3, NULL, 0);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl8", 0, strncmp(data, "\nblixejerkX", data_len));

    buffer_replace(buf, 0, 11, "1\n2\n3\n4\n", 8);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpl9", 0, strncmp(data, "1\n2\n3\n4\n", data_len));

    buffer_replace(buf, 2, 6, "five\nsix\nseven\neight\nnine", 25);
    buffer_get(buf, &data, &data_len);
    ASSERT("rpla", 0, strncmp(data, "1\nfive\nsix\nseven\neight\nnine", data_len));
)
