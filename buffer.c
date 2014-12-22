#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mlbuf.h"
#include "utlist.h"

static int _buffer_baction_do(buffer_t* self, bline_t* bline, baction_t* action, int is_redo, size_t* opt_repeat_offset);
static int _buffer_update(buffer_t* self, baction_t* action);
static int _buffer_truncate_undo_stack(buffer_t* self, baction_t* action_from);
static int _buffer_add_to_undo_stack(buffer_t* self, baction_t* action);
static int _buffer_apply_styles(bline_t* start_line, ssize_t line_delta);
static int _buffer_bline_apply_style_single(srule_t* srule, bline_t* bline);
static int _buffer_bline_apply_style_multi(srule_t* srule, bline_t* bline, srule_t** open_rule);
static bline_t* _buffer_bline_new(buffer_t* self);
static int _buffer_bline_free(bline_t* bline, bline_t* maybe_mark_line, size_t col_delta);
static bline_t* _buffer_bline_break(bline_t* bline, size_t col);
static size_t _buffer_bline_insert(bline_t* bline, size_t col, char* data, size_t data_len, int move_marks);
static size_t _buffer_bline_delete(bline_t* bline, size_t col, size_t num_chars);
static size_t _buffer_bline_col_to_index(bline_t* bline, size_t col);
static size_t _buffer_bline_index_to_col(bline_t* bline, size_t index);
static int _buffer_bline_count_chars(bline_t* bline);
static int _srule_multi_find(srule_t* rule, int find_end, bline_t* bline, size_t start_offset, size_t* ret_start, size_t* ret_stop);
static int _srule_multi_find_start(srule_t* rule, bline_t* bline, size_t start_offset, size_t* ret_start, size_t* ret_stop);
static int _srule_multi_find_end(srule_t* rule, bline_t* bline, size_t start_offset, size_t* ret_stop);
static int _baction_destroy(baction_t* action);

// Make a new buffer and return it
buffer_t* buffer_new() {
    buffer_t* buffer;
    bline_t* bline;
    buffer = calloc(1, sizeof(buffer_t));
    bline = _buffer_bline_new(buffer);
    buffer->first_line = bline;
    buffer->last_line = bline;
    buffer->line_count = 1;
    buffer->_mark_counter = 'a';
    return buffer;
}

// Free a buffer
int buffer_destroy(buffer_t* self) {
    bline_t* line;
    bline_t* line_tmp;
    baction_t* action;
    baction_t* action_tmp;
    for (line = self->last_line; line; ) {
        line_tmp = line->prev;
        _buffer_bline_free(line, NULL, 0);
        line = line_tmp;
    }
    if (self->data) free(self->data);
    DL_FOREACH_SAFE(self->actions, action, action_tmp) {
        DL_DELETE(self->actions, action);
        _baction_destroy(action);
    }
    return MLBUF_OK;
}

// Add a mark to this buffer and return it
mark_t* buffer_add_mark(buffer_t* self, bline_t* maybe_line, size_t maybe_col) {
    mark_t* mark;
    mark = calloc(1, sizeof(mark_t));
    if (maybe_line != NULL) {
        mark->bline = maybe_line;
        mark->col = maybe_col;
    } else {
        mark->bline = self->first_line;
        mark->col = 0;
    }
    mark->letter = self->_mark_counter;
    self->_mark_counter += 1;
    if (self->_mark_counter > 'z') {
        self->_mark_counter = 'a';
    }
    DL_APPEND(mark->bline->marks, mark);
    return mark;
}

// Get buffer contents and length
int buffer_get(buffer_t* self, char** ret_data, size_t* ret_data_len) {
    bline_t* bline;
    char* data_cursor;
    size_t alloc_size;
    if (self->is_data_dirty) {
        // Refresh self->data
        alloc_size = self->byte_count + 2;
        self->data = self->data != NULL
            ? realloc(self->data, alloc_size)
            : malloc(alloc_size);
        data_cursor = self->data;
        for (bline = self->first_line; bline != NULL; bline = bline->next) {
            if (bline->data_len > 0) {
                memcpy(data_cursor, bline->data, bline->data_len);
                data_cursor += bline->data_len;
                self->data_len += bline->data_len;
            }
            if (bline->next) {
                *data_cursor = '\n';
                data_cursor += 1;
            }
        }
        *data_cursor = '\0';
        self->data_len = (size_t)(data_cursor - self->data);
        self->is_data_dirty = 0;
    }
    *ret_data = self->data;
    *ret_data_len = self->data_len;
    return MLBUF_OK;
}

// Set buffer contents
int buffer_set(buffer_t* self, char* data, size_t data_len) {
    int rc;
    if ((rc = buffer_delete(self, 0, self->char_count)) != MLBUF_OK) {
        return rc;
    }
    return buffer_insert(self, 0, data, data_len, NULL);
}

// Insert data into buffer
int buffer_insert(buffer_t* self, size_t offset, char* data, size_t data_len, size_t* ret_num_chars) {
    int rc;
    bline_t* start_line;
    size_t start_col;
    bline_t* cur_line;
    size_t cur_col;
    bline_t* new_line;
    char* data_cursor;
    char* data_newline;
    size_t data_remaining_len;
    size_t insert_len;
    size_t num_lines_added;
    char* ins_data;
    size_t ins_data_len;
    size_t ins_data_nchars;
    baction_t* action;

    // Exit early if no data
    if (data_len < 1) {
        return MLBUF_OK;
    }

    // Find start line and col
    if ((rc = buffer_get_bline_col(self, offset, &start_line, &start_col)) != MLBUF_OK) {
        return rc;
    }

    // Insert lines
    data_cursor = data;
    data_remaining_len = data_len;
    cur_line = start_line;
    cur_col = start_col;
    num_lines_added = 0;
    while (data_remaining_len > 0 && (data_newline = memchr(data_cursor, '\n', data_remaining_len)) != NULL) {
        insert_len = (size_t)(data_newline - data_cursor);
        new_line = _buffer_bline_break(cur_line, cur_col);
        num_lines_added += 1;
        if (insert_len > 0) {
            _buffer_bline_insert(cur_line, cur_col, data_cursor, insert_len, 1);
        }
        data_remaining_len -= (data_newline - data_cursor) + 1;
        data_cursor = data_newline + 1;
        cur_line = new_line;
        cur_col = 0;
    }
    if (data_remaining_len > 0) {
        cur_col += _buffer_bline_insert(cur_line, cur_col, data_cursor, data_remaining_len, 1);
    }

    // Get inserted data
    buffer_substr(self, start_line, start_col, cur_line, cur_col, &ins_data, &ins_data_len, &ins_data_nchars);

    // Handle action
    action = calloc(1, sizeof(baction_t));
    action->type = MLBUF_BACTION_TYPE_INSERT;
    action->buffer = self;
    action->start_line = start_line;
    action->start_line_index = start_line->line_index;
    action->start_col = start_col;
    action->maybe_end_line = cur_line;
    action->maybe_end_col = cur_col;
    action->byte_delta = (ssize_t)ins_data_len;
    action->char_delta = (ssize_t)ins_data_nchars;
    action->line_delta = (ssize_t)num_lines_added;
    action->data = ins_data;
    action->data_len = ins_data_len;
    _buffer_update(self, action);
    if (ret_num_chars) *ret_num_chars = ins_data_nchars;

    return MLBUF_OK;
}

// Delete data from buffer
int buffer_delete(buffer_t* self, size_t offset, size_t num_chars) {
    bline_t* start_line;
    size_t start_col;
    bline_t* end_line;
    size_t end_col;
    bline_t* tmp_line;
    bline_t* swap_line;
    bline_t* next_line;
    size_t tmp_len;
    char* del_data;
    size_t del_data_len;
    size_t del_data_nchars;
    size_t num_lines_removed;
    size_t safe_num_chars;
    size_t orig_char_count;
    baction_t* action;

    // Find start/end line and col
    buffer_get_bline_col(self, offset, &start_line, &start_col);
    buffer_get_bline_col(self, offset + num_chars, &end_line, &end_col);

    // Exit early if there is nothing to delete
    if (start_line == end_line && start_col >= end_col) {
        return MLBUF_OK;
    } else if (start_line == self->last_line && start_col == self->last_line->char_count) {
        return MLBUF_OK;
    }

    // Get deleted data
    buffer_substr(self, start_line, start_col, end_line, end_col, &del_data, &del_data_len, &del_data_nchars);

    // Delete suffix starting at start_line:start_col
    safe_num_chars = MLBUF_MIN(num_chars, start_line->char_count - start_col);
    if (safe_num_chars > 0) {
        _buffer_bline_delete(start_line, start_col, safe_num_chars);
    }

    // Copy remaining portion of end_line to start_line:start_col
    orig_char_count = start_line->char_count;
    if (start_line != end_line && (tmp_len = end_line->data_len - _buffer_bline_col_to_index(end_line, end_col)) > 0) {
        _buffer_bline_insert(
            start_line,
            start_col,
            end_line->data + (end_line->data_len - tmp_len),
            tmp_len,
            0
        );
    }

    // Remove lines after start_line thru end_line
    // Relocate marks to end of start_line
    num_lines_removed = 0;
    swap_line = end_line->next;
    next_line = NULL;
    tmp_line = start_line->next;
    while (tmp_line != NULL && tmp_line != swap_line) {
        next_line = tmp_line->next;
        _buffer_bline_free(tmp_line, start_line, orig_char_count);
        num_lines_removed += 1;
        tmp_line = next_line;
    }
    start_line->next = swap_line;
    if (swap_line) swap_line->prev = start_line;


    // Handle action
    action = calloc(1, sizeof(baction_t));
    action->type = MLBUF_BACTION_TYPE_DELETE;
    action->buffer = self;
    action->start_line = start_line;
    action->start_line_index = start_line->line_index;
    action->start_col = start_col;
    action->byte_delta = -1 * (ssize_t)del_data_len;
    action->char_delta = -1 * (ssize_t)del_data_nchars;
    action->line_delta = -1 * (ssize_t)num_lines_removed;
    action->data = del_data;
    action->data_len = del_data_len;
    _buffer_update(self, action);

    return MLBUF_OK;
}

// Return a line given a line_index
int buffer_get_bline(buffer_t* self, size_t line_index, bline_t** ret_bline) {
    bline_t* tmp_line;
    for (tmp_line = self->first_line; tmp_line; tmp_line = tmp_line->next) {
        if (tmp_line->line_index == line_index) {
            *ret_bline = tmp_line;
            return MLBUF_OK;
        }
    }
    return MLBUF_ERR;
}

// Return a line and col for the given offset
int buffer_get_bline_col(buffer_t* self, size_t offset, bline_t** ret_bline, size_t* ret_col) {
    bline_t* tmp_line;
    bline_t* good_line;
    size_t remaining_chars;

    remaining_chars = offset;
    for (tmp_line = self->first_line; tmp_line != NULL; tmp_line = tmp_line->next) {
        if (tmp_line->char_count >= remaining_chars) {
            *ret_bline = tmp_line;
            *ret_col = remaining_chars;
            return MLBUF_OK;
        } else {
            remaining_chars -= (tmp_line->char_count + 1); // Plus 1 for newline
        }
        good_line = tmp_line;
    }

    if (!good_line) good_line = self->first_line;
    *ret_bline = good_line;
    *ret_col = good_line->char_count;
    return MLBUF_OK;
}

// Return an offset given a line and col
int buffer_get_offset(buffer_t* self, bline_t* bline, size_t col, size_t* ret_offset) {
    bline_t* tmp_line;
    size_t offset;

    offset = 0;
    for (tmp_line = self->first_line; tmp_line != bline->next; tmp_line = tmp_line->next) {
        if (tmp_line == bline) {
            offset = MLBUF_MIN(self->char_count, offset + col);
            break;
        } else {
            offset += tmp_line->char_count + 1; // Plus 1 for newline
        }
    }

    *ret_offset = offset;
    return MLBUF_OK;
}

// Add a style rule to the buffer
int buffer_add_srule(buffer_t* self, srule_t* srule) {
    srule_node_t* node;
    node = calloc(1, sizeof(srule_node_t));
    node->srule = srule;
    if (srule->type == MLBUF_SRULE_TYPE_SINGLE) {
        DL_APPEND(self->single_srules, node);
    } else {
        DL_APPEND(self->multi_srules, node);
    }
    return _buffer_apply_styles(self->first_line, self->line_count - 1);
}

// Remove a style rule from the buffer
int buffer_remove_srule(buffer_t* self, srule_t* srule) {
    int found;
    srule_node_t** head;
    srule_node_t* node;
    srule_node_t* node_tmp;
    if (srule->type == MLBUF_SRULE_TYPE_SINGLE) {
        head = &self->single_srules;
    } else {
        head = &self->multi_srules;
    }
    found = 0;
    DL_FOREACH_SAFE(*head, node, node_tmp) {
        if (node->srule == srule) {
            DL_DELETE(*head, node);
            free(node);
            found = 1;
            break;
        }
    }
    if (!found) return MLBUF_ERR;
    return _buffer_apply_styles(self->first_line, self->line_count - 1);
}

// Print buffer debug info to stream
int buffer_debug_dump(buffer_t* self, FILE* stream) {
    int i;
    size_t j;
    bline_t* bline_tmp;
    srule_node_t* srule_tmp;
    blistener_t* listener_tmp;
    baction_t* action_tmp;
    mark_t* mark_tmp;
    char* mark_str;
    size_t mark_str_len;
    mark_str = NULL;
    mark_str_len = 0;
    fprintf(stream, "first_line=%lu\n", self->first_line->line_index);
    fprintf(stream, "last_line=%lu\n", self->last_line->line_index);
    fprintf(stream, "byte_count=%lu\n", self->byte_count);
    fprintf(stream, "char_count=%lu\n", self->char_count);
    fprintf(stream, "line_count=%lu\n", self->line_count);
    fprintf(stream, "lines:\n");
    for (bline_tmp = self->first_line; bline_tmp; bline_tmp = bline_tmp->next) {
        fprintf(stream, "  %lu\n", bline_tmp->line_index);
        fprintf(stream, "    data=[%.*s]\n", (int)bline_tmp->data_len, bline_tmp->data ? bline_tmp->data : "");
        if (bline_tmp->char_count + 1 > mark_str_len) {
            mark_str = realloc(mark_str, bline_tmp->char_count + 1);
        }
        memset(mark_str, ' ', bline_tmp->char_count + 1);
        DL_FOREACH(bline_tmp->marks, mark_tmp) {
            *(mark_str + mark_tmp->col) = mark_tmp->letter;
        }
        fprintf(stream, "    mark  %.*s\n", (int)(bline_tmp->char_count + 1), mark_str);
        fprintf(stream, "      fg  ");
        if (bline_tmp->char_styles) {
            for (j = 0; j < bline_tmp->char_count; j++) {
                fprintf(stream, "%c", bline_tmp->char_styles[j].fg ? '*' : ' ');
            }
        }
        fprintf(stream, "\n      bg  ");
        if (bline_tmp->char_styles) {
            for (j = 0; j < bline_tmp->char_count; j++) {
                fprintf(stream, "%c", bline_tmp->char_styles[j].bg ? '*' : ' ');
            }
        }
        fprintf(stream, "\n");
    }
    fprintf(stream, "lines_extra:\n");
    for (bline_tmp = self->first_line; bline_tmp; bline_tmp = bline_tmp->next) {
        fprintf(stream, "  %lu\n", bline_tmp->line_index);
        fprintf(stream, "    line_index=%lu\n", bline_tmp->line_index);
        fprintf(stream, "    char_count=%lu\n", bline_tmp->char_count);
        fprintf(stream, "    next=%ld\n", bline_tmp->next ? bline_tmp->next->line_index : -1);
        fprintf(stream, "    prev=%ld\n", bline_tmp->prev ? bline_tmp->prev->line_index : -1);
        fprintf(stream, "    data_cap=%lu\n", bline_tmp->data_cap);
        fprintf(stream, "    char_indexes=");
        if (bline_tmp->char_indexes) {
            for (j = 0; j < bline_tmp->char_count; j++) {
                fprintf(stream, "%lu ", bline_tmp->char_indexes[j]);
            }
        }
        fprintf(stream, "\n    char_styles_cap=%lu\n", bline_tmp->char_styles_cap);
        fprintf(stream, "    char_indexes_cap=%lu\n", bline_tmp->char_indexes_cap);
    }
    fprintf(stream, "single_srules:\n");
    i = 0; DL_FOREACH(self->single_srules, srule_tmp) {
        fprintf(stream, "  %d\n", i);
        fprintf(stream, "    type=%d\n", srule_tmp->srule->type);
        fprintf(stream, "    re=%s\n", srule_tmp->srule->re ? srule_tmp->srule->re : "");
        fprintf(stream, "    re_end=%s\n", srule_tmp->srule->re_end ? srule_tmp->srule->re_end : "");
        fprintf(stream, "    cre=%c\n", srule_tmp->srule->cre ? 'y' : 'n');
        fprintf(stream, "    cre_end=%c\n", srule_tmp->srule->cre_end ? 'y' : 'n');
        fprintf(stream, "    range_a=%c\n", srule_tmp->srule->range_a ? srule_tmp->srule->range_a->letter : ' ');
        fprintf(stream, "    range_b=%c\n", srule_tmp->srule->range_b ? srule_tmp->srule->range_b->letter : ' ');
        fprintf(stream, "    style=%d,%d\n", srule_tmp->srule->style.fg, srule_tmp->srule->style.bg);
        i++;
    }
    fprintf(stream, "multi_srules:\n");
    i = 0; DL_FOREACH(self->multi_srules, srule_tmp) {
        fprintf(stream, "  %d\n", i);
        fprintf(stream, "    type=%d\n", srule_tmp->srule->type);
        fprintf(stream, "    re=%s\n", srule_tmp->srule->re ? srule_tmp->srule->re : "");
        fprintf(stream, "    re_end=%s\n", srule_tmp->srule->re_end ? srule_tmp->srule->re_end : "");
        fprintf(stream, "    cre=%c\n", srule_tmp->srule->cre ? 'y' : 'n');
        fprintf(stream, "    cre_end=%c\n", srule_tmp->srule->cre_end ? 'y' : 'n');
        fprintf(stream, "    range_a=%c\n", srule_tmp->srule->range_a ? srule_tmp->srule->range_a->letter : ' ');
        fprintf(stream, "    range_b=%c\n", srule_tmp->srule->range_b ? srule_tmp->srule->range_b->letter : ' ');
        fprintf(stream, "    style=%d,%d\n", srule_tmp->srule->style.fg, srule_tmp->srule->style.bg);
        i++;
    }
    i = 0; DL_FOREACH(self->listeners, listener_tmp) { i++; }
    fprintf(stream, "listeners=%d\n", i);
    fprintf(stream, "actions:\n");
    i = 0; DL_FOREACH(self->actions, action_tmp) {
        fprintf(stream, "  %d\n", i);
        fprintf(stream, "    type=%d\n", action_tmp->type);
        fprintf(stream, "    start_line_index=%lu\n", action_tmp->start_line_index);
        fprintf(stream, "    start_col=%lu\n", action_tmp->start_col);
        fprintf(stream, "    maybe_end_line_index=%lu\n", action_tmp->maybe_end_line_index);
        fprintf(stream, "    maybe_end_col=%lu\n", action_tmp->maybe_end_col);
        fprintf(stream, "    byte_delta=%ld\n", action_tmp->byte_delta);
        fprintf(stream, "    char_delta=%ld\n", action_tmp->char_delta);
        fprintf(stream, "    data=%.*s\n", (int)action_tmp->data_len, action_tmp->data);
        fprintf(stream, "    line_delta=%ld\n", action_tmp->line_delta);
    }
    fprintf(stream, "action_tail=%c\n", self->action_tail ? 'y' : 'n');
    fprintf(stream, "action_undone=%c\n", self->action_undone ? 'y' : 'n');
    fprintf(stream, "data=%.*s\n", (int)self->data_len, self->data ? self->data : "");
    fprintf(stream, "is_data_dirty=%d\n", self->is_data_dirty);
    if (mark_str) free(mark_str);
    return MLBUF_OK;
}

// Return data from start_line:start_col thru end_line:end_col
int buffer_substr(buffer_t* self, bline_t* start_line, size_t start_col, bline_t* end_line, size_t end_col, char** ret_data, size_t* ret_data_len, size_t* ret_data_nchars) {
    char* data;
    size_t data_len;
    size_t data_size;
    bline_t* tmp_line;
    size_t copy_len;
    size_t copy_index;
    size_t add_len;
    size_t nchars;

    data = calloc(2, sizeof(char));
    data_len = 0;
    data_size = 2;
    nchars = 0;

    for (tmp_line = start_line; tmp_line != end_line->next; tmp_line = tmp_line->next) {
        // Get copy_index + copy_len
        // Also increment nchars
        if (start_line == end_line) {
            copy_index = _buffer_bline_col_to_index(start_line, start_col);
            copy_len = _buffer_bline_col_to_index(start_line, end_col) - copy_index;
            nchars += end_col - start_col;
        } else if (tmp_line == start_line) {
            copy_index = _buffer_bline_col_to_index(start_line, start_col);
            copy_len = tmp_line->data_len - copy_index;
            nchars += start_line->char_count - start_col;
        } else if (tmp_line == end_line) {
            copy_index = 0;
            copy_len = _buffer_bline_col_to_index(end_line, end_col);
            nchars += end_col;
        } else {
            copy_index = 0;
            copy_len = tmp_line->data_len;
            nchars += tmp_line->char_count;
        }

        // Add 1 for newline if not on end_line
        add_len = copy_len + (tmp_line != end_line ? 1 : 0); // Plus 1 for newline
        nchars += (tmp_line != end_line ? 1 : 0);

        // Copy add_len bytes from copy_index into data
        if (add_len > 0) {
            if (data_len + add_len + 1 > data_size) {
                data_size = data_len + add_len + 1; // Plus 1 for nullchar
                data = realloc(data, data_size);
            }
            if (copy_len > 0) {
                memcpy(data + data_len, tmp_line->data + copy_index, copy_len);
                data_len += copy_len;
            }
            if (tmp_line != end_line) {
                *(data + data_len) = '\n';
                data_len += 1;
            }
        }
    }

    *(data + data_len) = '\0';
    *ret_data = data;
    *ret_data_len = data_len;
    *ret_data_nchars = nchars;

    return MLBUF_OK;
}

// Undo an action
int buffer_undo(buffer_t* self) {
    baction_t* action_to_undo;
    bline_t* bline;
    int rc;

    // Find action to undo
    if (self->action_undone) {
        if (self->action_undone == self->actions) {
            return MLBUF_ERR;
        } else if (!self->action_undone->prev) {
            return MLBUF_ERR;
        }
        action_to_undo = self->action_undone->prev;
    } else if (self->action_tail) {
        action_to_undo = self->action_tail;
    } else {
        return MLBUF_ERR;
    }

    // Get line to perform undo on
    bline = NULL;
    buffer_get_bline(self, action_to_undo->start_line_index, &bline);
    if (!bline) {
        return MLBUF_ERR;
    } else if (action_to_undo->start_col > bline->char_count) {
        return MLBUF_ERR;
    }

    // Perform action
    rc = _buffer_baction_do(self, bline, action_to_undo, 0, NULL);

    // Update action_undone
    if (rc == MLBUF_OK) {
        self->action_undone = action_to_undo;
    }
    return rc;
}

// Redo an undone action
int buffer_redo(buffer_t* self) {
    baction_t* action_to_redo;
    bline_t* bline;
    int rc;

    // Find action to undo
    if (!self->action_undone) {
        return MLBUF_ERR;
    }
    action_to_redo = self->action_undone;

    // Get line to perform undo on
    bline = NULL;
    buffer_get_bline(self, action_to_redo->start_line_index, &bline);
    if (!bline) {
        return MLBUF_ERR;
    } else if (action_to_redo->start_col > bline->char_count) {
        return MLBUF_ERR;
    }

    // Perform action
    rc = _buffer_baction_do(self, bline, action_to_redo, 1, NULL);

    // Update action_undone
    if (rc == MLBUF_OK) {
        self->action_undone = self->action_undone->next;
    }
    return rc;
}

int buffer_add_listener(buffer_t* self, blistener_t blistener); // TODO

static int _buffer_baction_do(buffer_t* self, bline_t* bline, baction_t* action, int is_redo, size_t* opt_repeat_offset) {
    int rc;
    size_t col;
    size_t offset;
    self->_is_in_undo = 1;
    col = opt_repeat_offset ? *opt_repeat_offset : action->start_col;
    buffer_get_offset(self, bline, col, &offset);
    if ((action->type == MLBUF_BACTION_TYPE_DELETE && is_redo)
        || (action->type == MLBUF_BACTION_TYPE_INSERT && !is_redo)
    ) {
        rc = buffer_delete(self, offset, (size_t)((is_redo ? -1 : 1) * action->char_delta));
    } else {
        rc = buffer_insert(self, offset, action->data, action->data_len, NULL);
    }
    self->_is_in_undo = 0;
    return rc;
}

static int _buffer_update(buffer_t* self, baction_t* action) {
    bline_t* tmp_line;
    bline_t* last_line;
    size_t new_line_index;

    // Adjust counts
    self->byte_count += action->byte_delta;
    self->char_count += action->char_delta;
    self->line_count += action->line_delta;
    self->is_data_dirty = 1;

    // Renumber lines
    last_line = NULL;
    new_line_index = action->start_line->line_index;
    for (tmp_line = action->start_line->next; tmp_line != NULL; tmp_line = tmp_line->next) {
        tmp_line->line_index = ++new_line_index;
        last_line = tmp_line;
    }
    self->last_line = last_line ? last_line : action->start_line;

    // Restyle from start_line
    _buffer_apply_styles(action->start_line, action->line_delta);

    // Handle undo stack
    if (self->_is_in_undo) {
        _baction_destroy(action);
    } else {
        _buffer_add_to_undo_stack(self, action);
    }

    // TODO raise events

    return MLBUF_OK;
}

static int _buffer_truncate_undo_stack(buffer_t* self, baction_t* action_from) {
    baction_t* action_target;
    baction_t* action_tmp;
    int do_delete;
    self->action_tail = action_from->prev;
    do_delete = 0;
    DL_FOREACH_SAFE(self->actions, action_target, action_tmp) {
        if (!do_delete && action_target == action_from) {
            do_delete = 1;
        }
        if (do_delete) {
            DL_DELETE(self->actions, action_target);
            _baction_destroy(action_target);
        }
    }
    return MLBUF_OK;
}

static int _buffer_add_to_undo_stack(buffer_t* self, baction_t* action) {
    if (self->action_undone) {
        // We are recording an action after an undo has been performed, so we
        // need to chop off the tail of the baction list before recording the
        // new one.
        // TODO could implement multilevel undo here instead
        _buffer_truncate_undo_stack(self, self->action_undone);
        self->action_undone = NULL;
    }

    // Append action to list
    DL_APPEND(self->actions, action);
    self->action_tail = action;
    return MLBUF_OK;
}

static int _buffer_apply_styles(bline_t* start_line, ssize_t line_delta) {
    bline_t* cur_line;
    srule_node_t* srule_node;
    srule_t* open_rule;
    size_t min_nlines;
    size_t styled_nlines;
    int open_rule_ended;
    int already_open;

    // Apply styles starting at start_line
    cur_line = start_line;
    open_rule = NULL;
    // min_nlines, minimum number of lines to style
    //     line_delta  < 0: 2 (start_line + 1)
    //     line_delta == 0: 1 (start_line)
    //     line_delta  > 0: 1 + line_delta (start_line + added lines)
    min_nlines = 1 + (line_delta < 0 ? 1 : line_delta);
    styled_nlines = 0;
    open_rule_ended = 0;
    while (cur_line) {
        if (cur_line->prev && cur_line->prev->eol_rule && !open_rule && !open_rule_ended) {
            // Resume open_rule from previous line
            open_rule = cur_line->prev->eol_rule;
        }
        if (cur_line->char_count > 0) {
            // Reset styles of cur_line
            if (!open_rule_ended) {
                memset(cur_line->char_styles, 0, cur_line->char_count * sizeof(sblock_t));
            }

            if (open_rule) {
                // Apply open_rule to cur_line
                already_open = cur_line->eol_rule == open_rule ? 1 : 0;
                _buffer_bline_apply_style_multi(open_rule, cur_line, &open_rule);
                if (open_rule) {
                    // open_rule is still open
                    if (styled_nlines > min_nlines && already_open) {
                        // We are past min_nlines and styles have not changed; done
                        break;
                    }
                } else {
                    // open_rule ended on this line; resume normal styling on same line
                    open_rule_ended = 1;
                    continue;
                }
            } else {
                // Apply single-line styles to cur_line
                DL_FOREACH(start_line->buffer->single_srules, srule_node) {
                    _buffer_bline_apply_style_single(srule_node->srule, cur_line);
                }

                // Apply multi-line and range styles to cur_line
                if (!open_rule_ended) cur_line->bol_rule = NULL;
                cur_line->eol_rule = NULL;
                DL_FOREACH(start_line->buffer->multi_srules, srule_node) {
                    _buffer_bline_apply_style_multi(srule_node->srule, cur_line, &open_rule);
                    if (open_rule) { // TODO multiple open_rules per line
                        // We have an open_rule; break
                        break;
                    }
                }
            }
        } else if (cur_line->char_count < 1) {
            cur_line->bol_rule = open_rule;
            cur_line->eol_rule = open_rule;
        } // end if cur_line->char_count > 0

        // Done styling cur_line; increment styled_nlines
        styled_nlines += 1;

        // If there is no open_rule and we are past min_nlines, we are done.
        if (!open_rule && (!cur_line->next || !cur_line->next->bol_rule) && styled_nlines > min_nlines) {
            break;
        }

        // Reset open_rule_ended flag
        if (open_rule_ended) open_rule_ended = 0;

        // Continue to next line
        cur_line = cur_line->next;
    } // end while (cur_line)

    return MLBUF_OK;
}

static int _buffer_bline_apply_style_single(srule_t* srule, bline_t* bline) {
    int rc;
    int substrs[3];
    size_t start;
    size_t stop;
    size_t look_offset;
    look_offset = 0;
    while (look_offset < bline->char_count) {
        if ((rc = pcre_exec(srule->cre, NULL, bline->data, bline->data_len, look_offset, 0, substrs, 3)) >= 0) {
            start = _buffer_bline_index_to_col(bline, substrs[0]);
            stop = _buffer_bline_index_to_col(bline, substrs[1]);
            for (; start < stop; start++) {
                bline->char_styles[start] = srule->style;
            }
            look_offset = stop;
        } else {
            break;
        }
    }
    return MLBUF_OK;
}

static int _buffer_bline_apply_style_multi(srule_t* srule, bline_t* bline, srule_t** open_rule) {
    size_t start;
    size_t start_stop;
    size_t end;
    int found_start;
    int found_end;
    int look_offset;

    look_offset = 0;
    do {
        found_start = 0;
        found_end = 0;
        if (*open_rule == NULL) {
            // Look for start and end of rule
            if ((found_start = _srule_multi_find_start(srule, bline, look_offset, &start, &start_stop))) {
                look_offset = start_stop;
                found_end = look_offset < bline->char_count
                    ? _srule_multi_find_end(srule, bline, look_offset, &end)
                    : 0;
                if (found_end) look_offset = end;
            } else {
                return MLBUF_OK; // No match; bail
            }
        } else {
            // Look for end of rule
            start = 0;
            bline->bol_rule = srule;
            found_end = _srule_multi_find_end(srule, bline, look_offset, &end);
        }

        // Set start, end, bol_rule, and eol_rule
        if (!found_end) {
            end = bline->char_count; // Style until eol
            bline->eol_rule = srule; // Set eol_rule
            *open_rule = srule;
        } else if (*open_rule != NULL) {
            *open_rule = NULL;
        }

        // Write styles
        for (; start < end; start++) {
            bline->char_styles[start] = srule->style;
        }

        // Range rules can only match once
        if (srule->type == MLBUF_SRULE_TYPE_RANGE) {
            break;
        }
    } while (found_start && found_end && look_offset < bline->char_count);

    return MLBUF_OK;
}

static bline_t* _buffer_bline_new(buffer_t* self) {
    bline_t* bline;
    bline = calloc(1, sizeof(bline_t));
    bline->buffer = self;
    return bline;
}

static int _buffer_bline_free(bline_t* bline, bline_t* maybe_mark_line, size_t col_delta) {
    mark_t* mark;
    mark_t* mark_tmp;
    if (bline->data) free(bline->data);
    if (bline->char_indexes) free(bline->char_indexes);
    if (bline->char_styles) free(bline->char_styles);
    if (bline->marks) {
        DL_FOREACH_SAFE(bline->marks, mark, mark_tmp) {
            if (maybe_mark_line) {
                MLBUF_MARK_MOVE(mark, maybe_mark_line, mark->col + col_delta, 1);
            } else {
                DL_DELETE(bline->marks, mark);
                free(mark);
            }
        }
    }
    free(bline);
    return MLBUF_OK;
}

static bline_t* _buffer_bline_break(bline_t* bline, size_t col) {
    size_t index;
    size_t len;
    bline_t* new_line;
    bline_t* tmp_line;
    mark_t* mark;
    mark_t* mark_tmp;

    // Make new_line
    new_line = _buffer_bline_new(bline->buffer);

    // Find byte index to break on
    index = _buffer_bline_col_to_index(bline, col);
    len = bline->data_len - index;

    if (len > 0) {
        // Move data to new line
        new_line->data = malloc(len);
        memcpy(new_line->data, bline->data + index, len);
        new_line->data_len = len;
        new_line->data_cap = len;
        _buffer_bline_count_chars(new_line); // Update char widths

        // Truncate orig line
        bline->data_len -= len;
        _buffer_bline_count_chars(bline); // Update char widths
    }

    // Insert new_line in linked list
    tmp_line = bline->next;
    bline->next = new_line;
    new_line->next = tmp_line;
    new_line->prev = bline;
    if (tmp_line) tmp_line->prev = new_line;

    // Move marks at or past col to new_line
    DL_FOREACH_SAFE(bline->marks, mark, mark_tmp) {
        if (mark->col >= col) {
            MLBUF_MARK_MOVE(mark, new_line, mark->col - col, 1);
        }
    }

    return new_line;
}

static size_t _buffer_bline_insert(bline_t* bline, size_t col, char* data, size_t data_len, int move_marks) {
    size_t index;
    mark_t* mark;
    mark_t* mark_tmp;
    size_t orig_char_count;
    size_t num_chars_added;

    // Ensure space for data
    if (!bline->data) {
        bline->data = malloc(data_len);
        bline->data_cap = data_len;
    } else if (bline->data_len + data_len > bline->data_cap) {
        bline->data = realloc(bline->data, bline->data_len + data_len);
        bline->data_cap = bline->data_len + data_len;
    }

    // Find insert point
    index = _buffer_bline_col_to_index(bline, col);

    // Make room for insert data
    if (index < bline->data_len) {
        memmove(bline->data + index + data_len, bline->data + index, bline->data_len - index);
    }
    bline->data_len += data_len;

    // Insert data
    memcpy(bline->data + index, data, data_len);

    // Update chars
    orig_char_count = bline->char_count;
    _buffer_bline_count_chars(bline);
    num_chars_added = bline->char_count - orig_char_count;

    // Move marks at or past col right by num_chars_added
    if (move_marks) {
        DL_FOREACH_SAFE(bline->marks, mark, mark_tmp) {
            if (mark->col >= col) {
                mark->col += num_chars_added;
            }
        }
    }

    return num_chars_added;
}

static size_t _buffer_bline_delete(bline_t* bline, size_t col, size_t num_chars) {
    size_t safe_num_chars;
    size_t index;
    size_t index_end;
    size_t move_len;
    mark_t* mark;
    mark_t* mark_tmp;
    size_t orig_char_count;
    size_t num_chars_deleted;

    // Clamp num_chars
    safe_num_chars = MLBUF_MIN(bline->char_count - col, num_chars);
    if (safe_num_chars != num_chars) {
        MLBUF_DEBUG_PRINTF("num_chars=%lu does not match safe_num_chars=%lu\n", num_chars, safe_num_chars);
    }

    // Nothing to do if safe_num_chars is 0
    if (safe_num_chars < 1) {
        MLBUF_DEBUG_PRINTF("safe_num_chars=%lu lt 1\n", safe_num_chars);
        return MLBUF_OK;
    }

    // Find delete bounds
    index = _buffer_bline_col_to_index(bline, col);
    index_end = _buffer_bline_col_to_index(bline, col + safe_num_chars);
    move_len = (size_t)(bline->data_len - index_end);

    // Shift data
    if (move_len > 0) {
        memmove(bline->data + index, bline->data + index_end, move_len);
    }
    bline->data_len -= index_end - index;

    // Update chars
    orig_char_count = bline->char_count;
    _buffer_bline_count_chars(bline);
    num_chars_deleted = orig_char_count - bline->char_count;

    // Move marks past col left by num_chars_deleted
    DL_FOREACH_SAFE(bline->marks, mark, mark_tmp) {
        if (mark->col > col) {
            mark->col -= num_chars_deleted;
        }
    }

    return num_chars_deleted;
}

static size_t _buffer_bline_col_to_index(bline_t* bline, size_t col) {
    size_t index;
    if (!bline->char_indexes) {
        return 0;
    }
    if (col >= bline->char_count) {
        index = bline->data_len;
    } else {
        index = bline->char_indexes[col];
    }
    return index;
}

static size_t _buffer_bline_index_to_col(bline_t* bline, size_t index) {
    size_t col;
    if (!bline->char_indexes) {
        return 0;
    }
    for (col = 0; col < bline->char_count; col++) {
        if (bline->char_indexes[col] >= index) {
            return col;
        }
    }
    return bline->char_count;
}

static int _buffer_bline_count_chars(bline_t* bline) {
    char* c;
    int char_len;

    // Return early if there is no data
    if (bline->data_len < 1) {
        bline->char_count = 0;
        return MLBUF_OK;
    }

    // Ensure space for char_indexes
    // It should have data_len elements at most
    if (!bline->char_indexes) {
        bline->char_indexes = malloc(bline->data_len * sizeof(size_t));
        bline->char_indexes_cap = bline->data_len;
    } else if (bline->data_len > bline->char_indexes_cap) {
        bline->char_indexes = realloc(bline->char_indexes, bline->data_len * sizeof(size_t));
        bline->char_indexes_cap = bline->data_len;
    }

    // Count utf8 chars
    bline->char_count = 0;
    for (c = bline->data; c < MLBUF_BLINE_DATA_STOP(bline); ) {
        char_len = utf8_char_length(*c);
        bline->char_indexes[bline->char_count] = (size_t)(c - bline->data);
        bline->char_count += 1;
        c += char_len;
    }

    // Ensure space for char_styles
    if (bline->char_count > 0) {
        if (!bline->char_styles) {
            bline->char_styles = calloc(bline->char_count, sizeof(sblock_t));
            bline->char_styles_cap = bline->char_count;
        } else if (bline->char_count > bline->char_styles_cap) {
            bline->char_styles = realloc(bline->char_styles, bline->char_count * sizeof(sblock_t));
            memset(bline->char_styles + bline->char_styles_cap, 0, (bline->char_count - bline->char_styles_cap) * sizeof(sblock_t));
            bline->char_styles_cap = bline->char_count;
        }
    }

    return MLBUF_OK;
}

// Make a new single-line style rule
srule_t* srule_new_single(char* re, size_t re_len, uint16_t fg, uint16_t bg) {
    srule_t* rule;
    const char *re_error;
    int re_erroffset;
    rule = calloc(1, sizeof(srule_t));
    rule->type = MLBUF_SRULE_TYPE_SINGLE;
    rule->style.fg = fg;
    rule->style.bg = bg;
    rule->re = malloc((re_len + 1) * sizeof(char));
    snprintf(rule->re, re_len, "%s", re);
    rule->cre = pcre_compile((const char*)rule->re, PCRE_NO_AUTO_CAPTURE, &re_error, &re_erroffset, NULL);
    if (!rule->cre) {
        // TODO log error
        srule_destroy(rule);
        return NULL;
    }
    return rule;
}

// Make a new multi-line style rule
srule_t* srule_new_multi(char* re, size_t re_len, char* re_end, size_t re_end_len, uint16_t fg, uint16_t bg) {
    srule_t* rule;
    const char *re_error;
    int re_erroffset;
    rule = calloc(1, sizeof(srule_t));
    rule->type = MLBUF_SRULE_TYPE_MULTI;
    rule->style.fg = fg;
    rule->style.bg = bg;
    rule->re = malloc((re_len + 1) * sizeof(char));
    rule->re_end = malloc((re_end_len + 1) * sizeof(char));
    snprintf(rule->re, re_len, "%s", re);
    snprintf(rule->re_end, re_end_len, "%s", re_end);
    rule->cre = pcre_compile((const char*)rule->re, PCRE_NO_AUTO_CAPTURE, &re_error, &re_erroffset, NULL);
    rule->cre_end = pcre_compile((const char*)rule->re_end, PCRE_NO_AUTO_CAPTURE, &re_error, &re_erroffset, NULL);
    if (!rule->cre || !rule->cre_end) {
        // TODO log error
        srule_destroy(rule);
        return NULL;
    }
    return rule;
}

// Make a new range style rule
srule_t* srule_new_range(mark_t* range_a, mark_t* range_b, uint16_t fg, uint16_t bg) {
    srule_t* rule;
    rule = calloc(1, sizeof(srule_t));
    rule->type = MLBUF_SRULE_TYPE_RANGE;
    rule->style.fg = fg;
    rule->style.bg = bg;
    rule->range_a = range_a;
    rule->range_b = range_b;
    return rule;
}

// Free an srule
int srule_destroy(srule_t* srule) {
    if (srule->re) free(srule->re);
    if (srule->re_end) free(srule->re_end);
    if (srule->cre) pcre_free(srule->cre);
    if (srule->cre_end) pcre_free(srule->cre_end);
    free(srule);
    return MLBUF_OK;
}

static int _srule_multi_find(srule_t* rule, int find_end, bline_t* bline, size_t start_offset, size_t* ret_start, size_t* ret_stop) {
    int rc;
    pcre* cre;
    int substrs[3];
    size_t start_index;
    mark_t* mark;

    if (rule->type == MLBUF_SRULE_TYPE_RANGE) {
        mark = mark_is_gt(rule->range_a, rule->range_b)
            ? (find_end ? rule->range_a : rule->range_b)
            : (find_end ? rule->range_b : rule->range_a);
        if (mark->bline == bline && mark->col >= start_offset) {
            *ret_start = mark->col;
            *ret_stop = mark->col;
            return 1;
        }
        return 0;
    }

    // MLBUF_SRULE_TYPE_MULTI
    cre = find_end ? rule->cre_end : rule->cre;
    start_index = _buffer_bline_col_to_index(bline, start_offset);
    if ((rc = pcre_exec(cre, NULL, bline->data, bline->data_len, start_index, 0, substrs, 3)) >= 0) {
        *ret_start = _buffer_bline_index_to_col(bline, substrs[0]);
        *ret_stop = _buffer_bline_index_to_col(bline, substrs[1]);
        return 1;
    }
    return 0;
}

static int _srule_multi_find_start(srule_t* rule, bline_t* bline, size_t start_offset, size_t* ret_start, size_t* ret_stop) {
    return _srule_multi_find(rule, 0, bline, start_offset, ret_start, ret_stop);
}

static int _srule_multi_find_end(srule_t* rule, bline_t* bline, size_t start_offset, size_t* ret_stop) {
    size_t ignore;
    return _srule_multi_find(rule, 1, bline, start_offset, &ignore, ret_stop);
}

static int _baction_destroy(baction_t* action) {
    if (action->data) free(action->data);
    free(action);
    return MLBUF_OK;
}
