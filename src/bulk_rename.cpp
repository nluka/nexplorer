#include <string>
#include <cassert>
#include <iostream>

#include <shlwapi.h>

#include "common.hpp"
#include "path.hpp"

// for ntest
bool bulk_rename_op::operator!=(bulk_rename_op const &other) const noexcept
{
    return this->before != other.before || !path_equals_exactly(this->after, other.after);
}

// for ntest
std::ostream& operator<<(std::ostream &os, bulk_rename_op const &r)
{
    return os << "B:[" << (r.before ? r.before->path.data() : "nullptr") << "] A:[" << r.after.data() << ']';
}

// for ntest
bool bulk_rename_collision::operator!=(bulk_rename_collision const &other) const noexcept
{
    return
        this->first_rename_pair_idx != other.first_rename_pair_idx ||
        this->last_rename_pair_idx != other.last_rename_pair_idx ||
        this->dest_dirent != other.dest_dirent;
}

// for ntest
std::ostream& operator<<(std::ostream &os, bulk_rename_collision const &c)
{
    return os << "D:[" << (c.dest_dirent ? c.dest_dirent->path.data() : "")
                << "] [" << c.first_rename_pair_idx << ',' << c.last_rename_pair_idx << ']';
}

// for ntest
bool bulk_rename_compiled_pattern::op::operator!=(bulk_rename_compiled_pattern::op const &other) const noexcept
{
    return this->kind != other.kind || this->ch != other.ch;
}

// for ntest
std::ostream& operator<<(std::ostream &os, bulk_rename_compiled_pattern::op const &op)
{
    using op_type = bulk_rename_compiled_pattern::op::type;

    char const *op_str = "";
    switch (op.kind) {
        case op_type::insert_char:    op_str = "insert_char";    break;
        case op_type::insert_name:    op_str = "insert_name";    break;
        case op_type::insert_ext:     op_str = "insert_ext";     break;
        case op_type::insert_size:    op_str = "insert_size";    break;
        case op_type::insert_counter: op_str = "insert_counter"; break;
        default:                      op_str = "unknown_op";     break;
    }

    os << op_str;
    if (op.kind == op_type::insert_char) {
        os << ' ';
        if (op.ch == '\0') os << "NUL";
        else               os << op.ch;
    }

    return os;
}

bulk_rename_compile_pattern_result bulk_rename_compile_pattern(char const *pattern, bool squish_adjacent_spaces) noexcept
{
    assert(pattern != nullptr);

    bulk_rename_compile_pattern_result result = {};
    auto &error = result.error;
    auto &success = result.success;
    auto &compiled = result.compiled_pattern;

    compiled.squish_adjacent_spaces = squish_adjacent_spaces;

    if (pattern[0] == '\0') {
        success = false;
        compiled = {};
        snprintf(error.data(), error.size(), "empty pattern");
        return result;
    }

    u64 const npos = std::string::npos;
    u64 opening_chevron_pos = npos;
    u64 closing_chevron_pos = npos;

    for (u64 i = 0; pattern[i] != '\0'; ++i) {
        char ch = pattern[i];

        if (ch == '\0') {
            break;
        }

        if ((s32)ch <= 31 || (s32)ch == 127 || strchr("\\/\"|?*", ch)) {
            success = false;
            compiled = {};
            snprintf(error.data(), error.size(), "illegal filename character %d at position %zu", ch, i);
            return result;
        }

        bool inside_chevrons = opening_chevron_pos != npos;

        if (ch == '<') {
            if (inside_chevrons) {
                success = false;
                compiled = {};
                snprintf(error.data(), error.size(), "unexpected '<' at position %zu, unclosed '<' at position %zu", i, opening_chevron_pos);
                return result;
            } else {
                opening_chevron_pos = i;
            }
        }
        else if (ch == '>') {
            if (inside_chevrons) {
                closing_chevron_pos = i;
                u64 expr_len = closing_chevron_pos - opening_chevron_pos - 1;

                if (expr_len == 0) {
                    success = false;
                    compiled = {};
                    snprintf(error.data(), error.size(), "empty expression starting at position %zu", opening_chevron_pos);
                    return result;
                }

                auto expr_equals = [&](char const *known_expr) {
                    return StrCmpNIA(pattern + opening_chevron_pos + 1, known_expr, (s32)expr_len) == 0;
                };

                bool known_expression = true;

                if (expr_equals("name")) {
                    bulk_rename_compiled_pattern::op op = {};
                    op.kind = bulk_rename_compiled_pattern::op::type::insert_name;
                    compiled.ops.push_back(op);
                }
                else if (expr_equals("ext")) {
                    bulk_rename_compiled_pattern::op op = {};
                    op.kind = bulk_rename_compiled_pattern::op::type::insert_ext;
                    compiled.ops.push_back(op);
                }
                else if (expr_equals("counter")) {
                    bulk_rename_compiled_pattern::op op = {};
                    op.kind = bulk_rename_compiled_pattern::op::type::insert_counter;
                    compiled.ops.push_back(op);
                }
                else if (expr_equals("bytes")) {
                    bulk_rename_compiled_pattern::op op = {};
                    op.kind = bulk_rename_compiled_pattern::op::type::insert_size;
                    compiled.ops.push_back(op);
                }
                else {
                    known_expression = false;
                }

                if (!known_expression) {
                    success = false;
                    compiled = {};
                    snprintf(error.data(), error.size(), "unknown expression starting at position %zu", opening_chevron_pos);
                    return result;
                }

                // reset
                opening_chevron_pos = npos;
                closing_chevron_pos = npos;
            } else {
                success = false;
                compiled = {};
                snprintf(error.data(), error.size(), "unexpected '>' at position %zu - no preceding '<' found", i);
                return result;
            }
        }
        else if (inside_chevrons) {
            // do nothing
        }
        else {
            // non-special character

            if (squish_adjacent_spaces && i > 0 && pattern[i-1] == ' ' && ch == ' ') {
                // don't insert multiple adjacent spaces
            } else {
                bulk_rename_compiled_pattern::op op = {};
                op.kind = bulk_rename_compiled_pattern::op::type::insert_char;
                op.ch = ch;
                compiled.ops.push_back(op);
            }
        }
    }

    if (opening_chevron_pos != npos && closing_chevron_pos == npos) {
        success = false;
        compiled = {};
        snprintf(error.data(), error.size(), "unclosed '<' at position %zu", opening_chevron_pos);
        return result;
    }

    success = true;
    return result;
}

bulk_rename_transform_result bulk_rename_transform(
    bulk_rename_compiled_pattern compiled_pattern,
    swan_path_t &after,
    char const *name,
    char const *ext,
    s32 counter,
    u64 bytes) noexcept
{
    bulk_rename_transform_result result = {};
    after = {};
    u64 after_insert_idx = 0;

    for (auto const &op : compiled_pattern.ops) {
        u64 space_left = after.max_size() - after_insert_idx;
        char *out = after.data() + after_insert_idx;
        s32 written = 0;

        switch (op.kind) {
            using op_type = bulk_rename_compiled_pattern::op::type;

            case op_type::insert_char: {
                if (after_insert_idx < after.max_size()) {
                    after[after_insert_idx++] = op.ch;
                } else {
                    result.success = false;
                    strncpy(result.error.data(), "not enough space for pattern", result.error.max_size());
                    return result;
                }
                break;
            }
            case op_type::insert_name: {
                u64 len = strlen(name);
                if (len <= space_left) {
                    strcat(out, name);
                    u64 spaces_removed = 0;
                    if (compiled_pattern.squish_adjacent_spaces) {
                        spaces_removed = remove_adjacent_spaces(out, len);
                    }
                    after_insert_idx += len - spaces_removed;
                } else {
                    result.success = false;
                    strncpy(result.error.data(), "not enough space for pattern", result.error.max_size());
                    return result;
                }
                break;
            }
            case op_type::insert_ext: {
                if (ext != nullptr) {
                    u64 len = strlen(ext);
                    if (len <= space_left) {
                        strcat(out, ext);
                        after_insert_idx += len;
                    } else {
                        result.success = false;
                        strncpy(result.error.data(), "not enough space for pattern", result.error.max_size());
                        return result;
                    }
                }
                break;
            }
            case op_type::insert_size: {
                char buffer[21] = {};
                written = snprintf(buffer, lengthof(buffer), "%zu", bytes);
                if (written <= space_left) {
                    strcat(out, buffer);
                    after_insert_idx += written;
                } else {
                    result.success = false;
                    strncpy(result.error.data(), "not enough space for pattern", result.error.max_size());
                    return result;
                }
                break;
            }
            case op_type::insert_counter: {
                char buffer[11] = {};
                written = snprintf(buffer, lengthof(buffer), "%d", counter);
                if (written <= space_left) {
                    strcat(out, buffer);
                    after_insert_idx += written;
                } else {
                    result.success = false;
                    strncpy(result.error.data(), "not enough space for pattern", result.error.max_size());
                    return result;
                }
                break;
            }
        }
    }

    result.success = true;
    return result;
}

void sort_renames_dup_elem_sequences_after_non_dups(std::vector<bulk_rename_op> &renames) noexcept
{
    // sort renames such that adjacently equal renames appear at the end.
    // examples:
    //  [1,2,5,7,2,4] -> [2,2,6,5,4,1]
    //  [0,0,1,5,5,2] -> [5,5,0,0,2,1]
    // (I couldn't figure out how to do it in ascending order... descending will do.)

    std::stable_sort(renames.begin(), renames.end(), [&](bulk_rename_op const &a, bulk_rename_op const &b) {
        s32 cmp = strcmp(a.after.data(), b.after.data());
        if (cmp == 0) {
            return false;
        } else {
            return cmp > 0;
        }
    });
}

std::vector<bulk_rename_collision> bulk_rename_find_collisions(
    std::vector<explorer_window::dirent> &dest,
    std::vector<bulk_rename_op> &renames) noexcept
{
    std::vector<bulk_rename_collision> collisions = {};

    if (renames.empty()) {
        return collisions;
    }

    collisions.reserve(dest.size());

    sort_renames_dup_elem_sequences_after_non_dups(renames);

    static std::vector<explorer_window::dirent *> unaffected_dirents = {};
    unaffected_dirents.clear();
    unaffected_dirents.reserve(dest.size());

    for (auto &dest_dirent : dest) {
        if (!dest_dirent.is_selected) {
            unaffected_dirents.push_back(&dest_dirent);
        }
    }

    u64 const npos = std::string::npos;

    auto find_conflict_in_dest = [&](bulk_rename_op const &rename) -> basic_dirent* {
        for (auto &dirent : unaffected_dirents) {
            if (path_equals_exactly(dirent->basic.path, rename.after)) {
                return &dirent->basic;
            }
        }
        return nullptr;
    };

    auto adj_begin = std::adjacent_find(renames.begin(), renames.end(), [](bulk_rename_op const &r0, bulk_rename_op const &r1) {
        return path_equals_exactly(r0.after, r1.after);
    });

    // handle unique "after"s
    {
        u64 i = 0;
        for (auto it = renames.begin(); it != adj_begin; ++it, ++i) {
            auto conflict = find_conflict_in_dest(*it);
            if (conflict) {
                u64 first = i, last = i;
                collisions.emplace_back(conflict, first, last);
            }
        }
    }
    // handle non-unique "after"s
    {
        ptrdiff_t start_index = std::distance(renames.begin(), adj_begin);

        if (adj_begin != renames.end() && path_equals_exactly(renames.front().after, renames.back().after)) {
            u64 first = (u64)start_index;
            u64 last = renames.size() - 1;
            auto conflict = find_conflict_in_dest(renames[first]);
            collisions.emplace_back(conflict, first, last);
        }
        else {
            u64 i = (u64)start_index + 1;
            u64 first = i - 1, last = npos;

            for (; i < renames.size(); ++i) {
                if (!path_equals_exactly(renames[i].after, renames[first].after)) {
                    last = i-1;
                    auto conflict = find_conflict_in_dest(renames[first]);
                    collisions.emplace_back(conflict, first, last);

                    first = i;
                    last = npos;
                }
            }
        }
    }

    return collisions;
}
