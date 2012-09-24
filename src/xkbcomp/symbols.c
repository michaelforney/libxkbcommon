/************************************************************
 * Copyright (c) 1994 by Silicon Graphics Computer Systems, Inc.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of Silicon Graphics not be
 * used in advertising or publicity pertaining to distribution
 * of the software without specific prior written permission.
 * Silicon Graphics makes no representation about the suitability
 * of this software for any purpose. It is provided "as is"
 * without any express or implied warranty.
 *
 * SILICON GRAPHICS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL SILICON
 * GRAPHICS BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION  WITH
 * THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 ********************************************************/

/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2012 Ran Benita <ran234@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Daniel Stone <daniel@fooishbar.org>
 *         Ran Benita <ran234@gmail.com>
 */

#include "xkbcomp-priv.h"
#include "text.h"
#include "expr.h"
#include "action.h"
#include "vmod.h"
#include "keycodes.h"
#include "include.h"
#include "keysym.h"

enum key_repeat {
    KEY_REPEAT_UNDEFINED = 0,
    KEY_REPEAT_YES = 1,
    KEY_REPEAT_NO = 2,
};

enum group_field {
    GROUP_FIELD_SYMS = (1 << 0),
    GROUP_FIELD_ACTS = (1 << 1),
    GROUP_FIELD_TYPE = (1 << 2),
};

enum key_field {
    KEY_FIELD_REPEAT    = (1 << 0),
    KEY_FIELD_TYPE_DFLT = (1 << 1),
    KEY_FIELD_GROUPINFO = (1 << 2),
    KEY_FIELD_VMODMAP   = (1 << 3),
};

typedef struct {
    enum group_field defined;
    darray(struct xkb_level) levels;
    xkb_atom_t type;
} GroupInfo;

typedef struct _KeyInfo {
    enum key_field defined;
    unsigned file_id;
    enum merge_mode merge;

    unsigned long name; /* the 4 chars of the key name, as long */

    darray(GroupInfo) groups;

    enum key_repeat repeat;
    xkb_mod_mask_t vmodmap;
    xkb_atom_t dfltType;

    enum xkb_range_exceed_type out_of_range_group_action;
    xkb_layout_index_t out_of_range_group_number;
} KeyInfo;

static void
ClearLevelInfo(struct xkb_level *leveli)
{
    if (leveli->num_syms > 1)
        free(leveli->u.syms);
}

static void
InitGroupInfo(GroupInfo *groupi)
{
    memset(groupi, 0, sizeof(*groupi));
}

static void
ClearGroupInfo(GroupInfo *groupi)
{
    struct xkb_level *leveli;
    darray_foreach(leveli, groupi->levels)
        ClearLevelInfo(leveli);
    darray_free(groupi->levels);
}

static void
CopyGroupInfo(GroupInfo *to, const GroupInfo *from)
{
    xkb_level_index_t j;
    to->defined = from->defined;
    to->type = from->type;
    darray_init(to->levels);
    darray_copy(to->levels, from->levels);
    for (j = 0; j < darray_size(to->levels); j++)
        if (darray_item(from->levels, j).num_syms > 1)
            darray_item(to->levels, j).u.syms =
                memdup(darray_item(from->levels, j).u.syms,
                       darray_item(from->levels, j).num_syms,
                       sizeof(xkb_keysym_t));
}

static void
InitKeyInfo(KeyInfo *keyi, unsigned file_id)
{
    static const char dflt_key_name[XKB_KEY_NAME_LENGTH] = "*";

    memset(keyi, 0, sizeof(*keyi));
    keyi->file_id = file_id;
    keyi->merge = MERGE_OVERRIDE;
    keyi->name = KeyNameToLong(dflt_key_name);
    keyi->out_of_range_group_action = RANGE_WRAP;
}

static void
ClearKeyInfo(KeyInfo *keyi)
{
    GroupInfo *groupi;
    darray_foreach(groupi, keyi->groups)
        ClearGroupInfo(groupi);
    darray_free(keyi->groups);
}

/***====================================================================***/

typedef struct _ModMapEntry {
    enum merge_mode merge;
    bool haveSymbol;
    int modifier;
    union {
        unsigned long keyName;
        xkb_keysym_t keySym;
    } u;
} ModMapEntry;

typedef struct _SymbolsInfo {
    char *name;         /* e.g. pc+us+inet(evdev) */
    int errorCount;
    unsigned file_id;
    enum merge_mode merge;
    xkb_layout_index_t explicit_group;
    darray(KeyInfo) keys;
    KeyInfo dflt;
    VModInfo vmods;
    ActionsInfo *actions;
    darray_xkb_atom_t group_names;
    darray(ModMapEntry) modMaps;

    struct xkb_keymap *keymap;
} SymbolsInfo;

static void
InitSymbolsInfo(SymbolsInfo *info, struct xkb_keymap *keymap,
                unsigned file_id, ActionsInfo *actions)
{
    memset(info, 0, sizeof(*info));
    info->keymap = keymap;
    info->file_id = file_id;
    info->merge = MERGE_OVERRIDE;
    InitKeyInfo(&info->dflt, file_id);
    InitVModInfo(&info->vmods, keymap);
    info->actions = actions;
    info->explicit_group = XKB_LAYOUT_INVALID;
}

static void
ClearSymbolsInfo(SymbolsInfo * info)
{
    KeyInfo *keyi;
    free(info->name);
    darray_foreach(keyi, info->keys)
        ClearKeyInfo(keyi);
    darray_free(info->keys);
    darray_free(info->group_names);
    darray_free(info->modMaps);
    ClearKeyInfo(&info->dflt);
}

static bool
MergeGroups(SymbolsInfo *info, GroupInfo *into, GroupInfo *from, bool clobber,
            bool report, xkb_layout_index_t group, unsigned long key_name)
{
    xkb_level_index_t i, levels_in_both;

    /* First find the type of the merged group. */
    if (into->type != from->type) {
        if (from->type == XKB_ATOM_NONE) {
        }
        else if (into->type == XKB_ATOM_NONE) {
            into->type = from->type;
        }
        else {
            xkb_atom_t use = (clobber ? from->type : into->type);
            xkb_atom_t ignore = (clobber ? into->type : from->type);

            if (report)
                log_warn(info->keymap->ctx,
                         "Multiple definitions for group %d type of key %s; "
                         "Using %s, ignoring %s\n",
                         group + 1, LongKeyNameText(key_name),
                         xkb_atom_text(info->keymap->ctx, use),
                         xkb_atom_text(info->keymap->ctx, ignore));

            into->type = use;
        }
    }
    into->defined |= (from->defined & GROUP_FIELD_TYPE);

    /* Now look at the levels. */

    if (darray_empty(from->levels)) {
        InitGroupInfo(from);
        return true;
    }

    if (darray_empty(into->levels)) {
        from->type = into->type;
        *into = *from;
        InitGroupInfo(from);
        return true;
    }

    /* Merge the actions and syms. */
    levels_in_both = MIN(darray_size(into->levels), darray_size(from->levels));
    for (i = 0; i < levels_in_both; i++) {
        struct xkb_level *intoLevel = &darray_item(into->levels, i);
        struct xkb_level *fromLevel = &darray_item(from->levels, i);

        if (fromLevel->action.type == ACTION_TYPE_NONE) {
        }
        else if (intoLevel->action.type == ACTION_TYPE_NONE) {
            intoLevel->action = fromLevel->action;
        }
        else {
            union xkb_action *use, *ignore;
            use = (clobber ? &fromLevel->action : &intoLevel->action);
            ignore = (clobber ? &intoLevel->action : &fromLevel->action);

            if (report)
                log_warn(info->keymap->ctx,
                         "Multiple actions for level %d/group %u on key %s; "
                         "Using %s, ignoring %s\n",
                         i + 1, group + 1, LongKeyNameText(key_name),
                         ActionTypeText(use->type),
                         ActionTypeText(ignore->type));

            intoLevel->action = *use;
        }

        if (fromLevel->num_syms == 0) {
        }
        else if (intoLevel->num_syms == 0) {
            intoLevel->num_syms = fromLevel->num_syms;
            if (fromLevel->num_syms > 1)
                intoLevel->u.syms = fromLevel->u.syms;
            else
                intoLevel->u.sym = fromLevel->u.sym;
            fromLevel->num_syms = 0;
        }
        else {
            if (report)
                log_warn(info->keymap->ctx,
                         "Multiple symbols for level %d/group %u on key %s; "
                         "Using %s, ignoring %s\n",
                         i + 1, group + 1, LongKeyNameText(key_name),
                         (clobber ? "from" : "to"),
                         (clobber ? "to" : "from"));

            if (clobber) {
                ClearLevelInfo(intoLevel);
                intoLevel->num_syms = fromLevel->num_syms;
                if (fromLevel->num_syms > 1)
                    intoLevel->u.syms = fromLevel->u.syms;
                else
                    intoLevel->u.sym = fromLevel->u.sym;
                fromLevel->num_syms = 0;
            }
        }
    }
    /* If @from has extra levels, get them as well. */
    for (i = levels_in_both; i < darray_size(from->levels); i++) {
        darray_append(into->levels, darray_item(from->levels, i));
        darray_item(from->levels, i).num_syms = 0;
    }
    into->defined |= (from->defined & GROUP_FIELD_ACTS);
    into->defined |= (from->defined & GROUP_FIELD_SYMS);

    return true;
}

static bool
UseNewKeyField(enum key_field field, enum key_field old, enum key_field new,
               bool clobber, bool report, enum key_field *collide)
{
    if (!(old & field))
        return (new & field);

    if (new & field) {
        if (report)
            *collide |= field;

        if (clobber)
            return true;
    }

    return false;
}

static bool
MergeKeys(SymbolsInfo *info, KeyInfo *into, KeyInfo *from)
{
    xkb_layout_index_t i;
    xkb_layout_index_t groups_in_both;
    enum key_field collide = 0;
    bool clobber, report;
    int verbosity = xkb_context_get_log_verbosity(info->keymap->ctx);

    if (from->merge == MERGE_REPLACE) {
        ClearKeyInfo(into);
        *into = *from;
        InitKeyInfo(from, info->file_id);
        return true;
    }

    clobber = (from->merge != MERGE_AUGMENT);
    report = (verbosity > 9 ||
              (into->file_id == from->file_id && verbosity > 0));

    groups_in_both = MIN(darray_size(into->groups),
                         darray_size(from->groups));
    for (i = 0; i < groups_in_both; i++)
        MergeGroups(info,
                    &darray_item(into->groups, i),
                    &darray_item(from->groups, i),
                    clobber, report, i, into->name);
    /* If @from has extra groups, just move them to @into. */
    for (i = groups_in_both; i < darray_size(from->groups); i++) {
        darray_append(into->groups, darray_item(from->groups, i));
        InitGroupInfo(&darray_item(from->groups, i));
    }

    if (UseNewKeyField(KEY_FIELD_VMODMAP, into->defined, from->defined,
                       clobber, report, &collide)) {
        into->vmodmap = from->vmodmap;
        into->defined |= KEY_FIELD_VMODMAP;
    }
    if (UseNewKeyField(KEY_FIELD_REPEAT, into->defined, from->defined,
                       clobber, report, &collide)) {
        into->repeat = from->repeat;
        into->defined |= KEY_FIELD_REPEAT;
    }
    if (UseNewKeyField(KEY_FIELD_TYPE_DFLT, into->defined, from->defined,
                       clobber, report, &collide)) {
        into->dfltType = from->dfltType;
        into->defined |= KEY_FIELD_TYPE_DFLT;
    }
    if (UseNewKeyField(KEY_FIELD_GROUPINFO, into->defined, from->defined,
                       clobber, report, &collide)) {
        into->out_of_range_group_action = from->out_of_range_group_action;
        into->out_of_range_group_number = from->out_of_range_group_number;
        into->defined |= KEY_FIELD_GROUPINFO;
    }

    if (collide)
        log_warn(info->keymap->ctx,
                 "Symbol map for key %s redefined; "
                 "Using %s definition for conflicting fields\n",
                 LongKeyNameText(into->name),
                 (clobber ? "first" : "last"));

    ClearKeyInfo(from);
    InitKeyInfo(from, info->file_id);
    return true;
}

static bool
AddKeySymbols(SymbolsInfo *info, KeyInfo *keyi)
{
    unsigned long real_name;
    KeyInfo *iter;

    /*
     * Don't keep aliases in the keys array; this guarantees that
     * searching for keys to merge with by straight comparison (see the
     * following loop) is enough, and we won't get multiple KeyInfo's
     * for the same key because of aliases.
     */
    if (FindKeyNameForAlias(info->keymap, keyi->name, &real_name))
        keyi->name = real_name;

    darray_foreach(iter, info->keys)
        if (iter->name == keyi->name)
            return MergeKeys(info, iter, keyi);

    darray_append(info->keys, *keyi);
    InitKeyInfo(keyi, info->file_id);
    return true;
}

static bool
AddModMapEntry(SymbolsInfo * info, ModMapEntry * new)
{
    ModMapEntry *mm;
    bool clobber;

    clobber = (new->merge != MERGE_AUGMENT);
    darray_foreach(mm, info->modMaps) {
        if (new->haveSymbol && mm->haveSymbol
            && (new->u.keySym == mm->u.keySym)) {
            unsigned use, ignore;
            if (mm->modifier != new->modifier) {
                if (clobber) {
                    use = new->modifier;
                    ignore = mm->modifier;
                }
                else {
                    use = mm->modifier;
                    ignore = new->modifier;
                }
                log_err(info->keymap->ctx,
                        "%s added to symbol map for multiple modifiers; "
                        "Using %s, ignoring %s.\n",
                        KeysymText(new->u.keySym), ModIndexText(use),
                        ModIndexText(ignore));
                mm->modifier = use;
            }
            return true;
        }
        if ((!new->haveSymbol) && (!mm->haveSymbol) &&
            (new->u.keyName == mm->u.keyName)) {
            unsigned use, ignore;
            if (mm->modifier != new->modifier) {
                if (clobber) {
                    use = new->modifier;
                    ignore = mm->modifier;
                }
                else {
                    use = mm->modifier;
                    ignore = new->modifier;
                }
                log_err(info->keymap->ctx,
                        "Key %s added to map for multiple modifiers; "
                        "Using %s, ignoring %s.\n",
                        LongKeyNameText(new->u.keyName), ModIndexText(use),
                        ModIndexText(ignore));
                mm->modifier = use;
            }
            return true;
        }
    }

    darray_append(info->modMaps, *new);
    return true;
}

/***====================================================================***/

static void
MergeIncludedSymbols(SymbolsInfo *into, SymbolsInfo *from,
                     enum merge_mode merge)
{
    unsigned int i;
    KeyInfo *keyi;
    ModMapEntry *mm;
    xkb_atom_t *group_name;
    xkb_layout_index_t group_names_in_both;

    if (from->errorCount > 0) {
        into->errorCount += from->errorCount;
        return;
    }

    if (into->name == NULL) {
        into->name = from->name;
        from->name = NULL;
    }

    group_names_in_both = MIN(darray_size(into->group_names),
                              darray_size(from->group_names));
    for (i = 0; i < group_names_in_both; i++) {
        if (!darray_item(from->group_names, i))
            continue;

        if (merge == MERGE_AUGMENT && darray_item(into->group_names, i))
            continue;

        darray_item(into->group_names, i) = darray_item(from->group_names, i);
    }
    /* If @from has more, get them as well. */
    darray_foreach_from(group_name, from->group_names, group_names_in_both)
        darray_append(into->group_names, *group_name);

    darray_foreach(keyi, from->keys) {
        merge = (merge == MERGE_DEFAULT ? keyi->merge : merge);
        if (!AddKeySymbols(into, keyi))
            into->errorCount++;
    }

    darray_foreach(mm, from->modMaps) {
        mm->merge = (merge == MERGE_DEFAULT ? mm->merge : merge);
        if (!AddModMapEntry(into, mm))
            into->errorCount++;
    }
}

static void
HandleSymbolsFile(SymbolsInfo *info, XkbFile *file, enum merge_mode merge);

static bool
HandleIncludeSymbols(SymbolsInfo *info, IncludeStmt *stmt)
{
    enum merge_mode merge = MERGE_DEFAULT;
    XkbFile *rtrn;
    SymbolsInfo included, next_incl;

    InitSymbolsInfo(&included, info->keymap, info->file_id, info->actions);
    if (stmt->stmt) {
        free(included.name);
        included.name = stmt->stmt;
        stmt->stmt = NULL;
    }

    for (; stmt; stmt = stmt->next_incl) {
        if (!ProcessIncludeFile(info->keymap->ctx, stmt, FILE_TYPE_SYMBOLS,
                                &rtrn, &merge)) {
            info->errorCount += 10;
            ClearSymbolsInfo(&included);
            return false;
        }

        InitSymbolsInfo(&next_incl, info->keymap, rtrn->id, info->actions);
        next_incl.merge = next_incl.dflt.merge = MERGE_OVERRIDE;
        if (stmt->modifier) {
            next_incl.explicit_group = atoi(stmt->modifier) - 1;
            if (next_incl.explicit_group >= XKB_NUM_GROUPS) {
                log_err(info->keymap->ctx,
                        "Cannot set explicit group to %d - must be between 1..%d; "
                        "Ignoring group number\n",
                        next_incl.explicit_group + 1, XKB_NUM_GROUPS);
                next_incl.explicit_group = info->explicit_group;
            }
        }
        else {
            next_incl.explicit_group = info->explicit_group;
        }

        HandleSymbolsFile(&next_incl, rtrn, MERGE_OVERRIDE);

        MergeIncludedSymbols(&included, &next_incl, merge);

        ClearSymbolsInfo(&next_incl);
        FreeXkbFile(rtrn);
    }

    MergeIncludedSymbols(info, &included, merge);
    ClearSymbolsInfo(&included);

    return (info->errorCount == 0);
}

#define SYMBOLS 1
#define ACTIONS 2

static bool
GetGroupIndex(SymbolsInfo *info, KeyInfo *keyi, ExprDef *arrayNdx,
              unsigned what, xkb_layout_index_t *ndx_rtrn)
{
    const char *name = (what == SYMBOLS ? "symbols" : "actions");

    if (arrayNdx == NULL) {
        xkb_layout_index_t i;
        GroupInfo *groupi;
        enum group_field field = (what == SYMBOLS ?
                                  GROUP_FIELD_SYMS : GROUP_FIELD_ACTS);

        darray_enumerate(i, groupi, keyi->groups) {
            if (!(groupi->defined & field)) {
                *ndx_rtrn = i;
                return true;
            }
        }

        if (i >= XKB_NUM_GROUPS) {
            log_err(info->keymap->ctx,
                    "Too many groups of %s for key %s (max %u); "
                    "Ignoring %s defined for extra groups\n",
                    name, LongKeyNameText(keyi->name), XKB_NUM_GROUPS + 1, name);
            return false;
        }

        darray_resize0(keyi->groups, darray_size(keyi->groups) + 1);
        *ndx_rtrn = darray_size(keyi->groups) - 1;
        return true;
    }

    if (!ExprResolveGroup(info->keymap->ctx, arrayNdx, ndx_rtrn)) {
        log_err(info->keymap->ctx,
                "Illegal group index for %s of key %s\n"
                "Definition with non-integer array index ignored\n",
                name, LongKeyNameText(keyi->name));
        return false;
    }

    (*ndx_rtrn)--;
    if (*ndx_rtrn >= darray_size(keyi->groups))
        darray_resize0(keyi->groups, *ndx_rtrn + 1);

    return true;
}

bool
LookupKeysym(const char *str, xkb_keysym_t *sym_rtrn)
{
    xkb_keysym_t sym;

    if (!str || istreq(str, "any") || istreq(str, "nosymbol")) {
        *sym_rtrn = XKB_KEY_NoSymbol;
        return 1;
    }

    if (istreq(str, "none") || istreq(str, "voidsymbol")) {
        *sym_rtrn = XKB_KEY_VoidSymbol;
        return 1;
    }

    sym = xkb_keysym_from_name(str);
    if (sym != XKB_KEY_NoSymbol) {
        *sym_rtrn = sym;
        return 1;
    }

    return 0;
}

static bool
AddSymbolsToKey(SymbolsInfo *info, KeyInfo *keyi, ExprDef *arrayNdx,
                ExprDef *value)
{
    xkb_layout_index_t ndx;
    GroupInfo *groupi;
    xkb_level_index_t nLevels;
    xkb_level_index_t i;
    int j;

    if (!GetGroupIndex(info, keyi, arrayNdx, SYMBOLS, &ndx))
        return false;

    groupi = &darray_item(keyi->groups, ndx);

    if (value == NULL) {
        groupi->defined |= GROUP_FIELD_SYMS;
        return true;
    }

    if (value->op != EXPR_KEYSYM_LIST) {
        log_err(info->keymap->ctx,
                "Expected a list of symbols, found %s; "
                "Ignoring symbols for group %u of %s\n",
                expr_op_type_to_string(value->op), ndx + 1,
                LongKeyNameText(keyi->name));
        return false;
    }

    if (groupi->defined & GROUP_FIELD_SYMS) {
        log_err(info->keymap->ctx,
                "Symbols for key %s, group %u already defined; "
                "Ignoring duplicate definition\n",
                LongKeyNameText(keyi->name), ndx + 1);
        return false;
    }

    nLevels = darray_size(value->value.list.symsMapIndex);
    if (darray_size(groupi->levels) < nLevels)
        darray_resize0(groupi->levels, nLevels);

    groupi->defined |= GROUP_FIELD_SYMS;

    for (i = 0; i < nLevels; i++) {
        unsigned int sym_index;
        struct xkb_level *leveli = &darray_item(groupi->levels, i);

        sym_index = darray_item(value->value.list.symsMapIndex, i);
        leveli->num_syms = darray_item(value->value.list.symsNumEntries, i);
        if (leveli->num_syms > 1)
            leveli->u.syms = calloc(leveli->num_syms, sizeof(*leveli->u.syms));

        for (j = 0; j < leveli->num_syms; j++) {
            char *sym_name = darray_item(value->value.list.syms,
                                         sym_index + j);
            xkb_keysym_t keysym;

            if (!LookupKeysym(sym_name, &keysym)) {
                const char *group_name = "unnamed";

                if (ndx < darray_size(info->group_names) &&
                    darray_item(info->group_names, ndx))
                    group_name = xkb_atom_text(info->keymap->ctx,
                                               darray_item(info->group_names,
                                                           ndx));

                log_warn(info->keymap->ctx,
                         "Could not resolve keysym %s for key %s, group %u (%s), level %u\n",
                         sym_name, LongKeyNameText(keyi->name), ndx + 1,
                         group_name, i);

                ClearLevelInfo(leveli);
                leveli->num_syms = 0;
                break;
            }

            if (leveli->num_syms == 1) {
                if (keysym == XKB_KEY_NoSymbol)
                    leveli->num_syms = 0;
                else
                    leveli->u.sym = keysym;
            }
            else if (leveli->num_syms > 1) {
                leveli->u.syms[j] = keysym;
            }
        }
    }

    return true;
}

static bool
AddActionsToKey(SymbolsInfo *info, KeyInfo *keyi, ExprDef *arrayNdx,
                ExprDef *value)
{
    unsigned int i;
    xkb_layout_index_t ndx;
    GroupInfo *groupi;
    unsigned int nActs;
    ExprDef *act;
    union xkb_action *toAct;

    if (!GetGroupIndex(info, keyi, arrayNdx, ACTIONS, &ndx))
        return false;

    groupi = &darray_item(keyi->groups, ndx);

    if (value == NULL) {
        groupi->defined |= GROUP_FIELD_ACTS;
        return true;
    }

    if (value->op != EXPR_ACTION_LIST) {
        log_wsgo(info->keymap->ctx,
                 "Bad expression type (%d) for action list value; "
                 "Ignoring actions for group %u of %s\n",
                 value->op, ndx, LongKeyNameText(keyi->name));
        return false;
    }

    if (groupi->defined & GROUP_FIELD_ACTS) {
        log_wsgo(info->keymap->ctx,
                 "Actions for key %s, group %u already defined\n",
                 LongKeyNameText(keyi->name), ndx);
        return false;
    }

    nActs = 0;
    for (act = value->value.child; act; act = (ExprDef *) act->common.next)
        nActs++;

    if (darray_size(groupi->levels) < nActs)
        darray_resize0(groupi->levels, nActs);

    groupi->defined |= GROUP_FIELD_ACTS;

    act = value->value.child;
    for (i = 0; i < nActs; i++) {
        toAct = &darray_item(groupi->levels, i).action;

        if (!HandleActionDef(act, info->keymap, toAct, info->actions))
            log_err(info->keymap->ctx,
                    "Illegal action definition for %s; "
                    "Action for group %u/level %u ignored\n",
                    LongKeyNameText(keyi->name), ndx + 1, i + 1);

        act = (ExprDef *) act->common.next;
    }

    return true;
}

static const LookupEntry repeatEntries[] = {
    { "true", KEY_REPEAT_YES },
    { "yes", KEY_REPEAT_YES },
    { "on", KEY_REPEAT_YES },
    { "false", KEY_REPEAT_NO },
    { "no", KEY_REPEAT_NO },
    { "off", KEY_REPEAT_NO },
    { "default", KEY_REPEAT_UNDEFINED },
    { NULL, 0 }
};

static bool
SetSymbolsField(SymbolsInfo *info, KeyInfo *keyi, const char *field,
                ExprDef *arrayNdx, ExprDef *value)
{
    bool ok = true;
    struct xkb_context *ctx = info->keymap->ctx;

    if (istreq(field, "type")) {
        xkb_layout_index_t ndx;
        xkb_atom_t val;

        if (!ExprResolveString(ctx, value, &val))
            log_vrb(ctx, 1,
                    "The type field of a key symbol map must be a string; "
                    "Ignoring illegal type definition\n");

        if (arrayNdx == NULL) {
            keyi->dfltType = val;
            keyi->defined |= KEY_FIELD_TYPE_DFLT;
        }
        else if (!ExprResolveGroup(ctx, arrayNdx, &ndx)) {
            log_err(ctx,
                    "Illegal group index for type of key %s; "
                    "Definition with non-integer array index ignored\n",
                    LongKeyNameText(keyi->name));
            return false;
        }
        else {
            ndx--;
            if (ndx >= darray_size(keyi->groups))
                darray_resize0(keyi->groups, ndx + 1);
            darray_item(keyi->groups, ndx).type = val;
            darray_item(keyi->groups, ndx).defined |= GROUP_FIELD_TYPE;
        }
    }
    else if (istreq(field, "symbols"))
        return AddSymbolsToKey(info, keyi, arrayNdx, value);
    else if (istreq(field, "actions"))
        return AddActionsToKey(info, keyi, arrayNdx, value);
    else if (istreq(field, "vmods") ||
             istreq(field, "virtualmods") ||
             istreq(field, "virtualmodifiers")) {
        xkb_mod_mask_t mask;

        ok = ExprResolveVModMask(info->keymap, value, &mask);
        if (ok) {
            keyi->vmodmap = (mask >> XKB_NUM_CORE_MODS) & 0xffff;
            keyi->defined |= KEY_FIELD_VMODMAP;
        }
        else {
            log_err(info->keymap->ctx,
                    "Expected a virtual modifier mask, found %s; "
                    "Ignoring virtual modifiers definition for key %s\n",
                    expr_op_type_to_string(value->op),
                    LongKeyNameText(keyi->name));
        }
    }
    else if (istreq(field, "locking") ||
             istreq(field, "lock") ||
             istreq(field, "locks")) {
        log_err(info->keymap->ctx,
                "Key behaviors not supported; "
                "Ignoring locking specification for key %s\n",
                LongKeyNameText(keyi->name));
    }
    else if (istreq(field, "radiogroup") ||
             istreq(field, "permanentradiogroup") ||
             istreq(field, "allownone")) {
        log_err(info->keymap->ctx,
                "Radio groups not supported; "
                "Ignoring radio group specification for key %s\n",
                LongKeyNameText(keyi->name));
    }
    else if (istreq_prefix("overlay", field) ||
             istreq_prefix("permanentoverlay", field)) {
        log_err(info->keymap->ctx,
                "Overlays not supported; "
                "Ignoring overlay specification for key %s\n",
                LongKeyNameText(keyi->name));
    }
    else if (istreq(field, "repeating") ||
             istreq(field, "repeats") ||
             istreq(field, "repeat")) {
        unsigned int val;

        ok = ExprResolveEnum(ctx, value, &val, repeatEntries);
        if (!ok) {
            log_err(info->keymap->ctx,
                    "Illegal repeat setting for %s; "
                    "Non-boolean repeat setting ignored\n",
                    LongKeyNameText(keyi->name));
            return false;
        }
        keyi->repeat = val;
        keyi->defined |= KEY_FIELD_REPEAT;
    }
    else if (istreq(field, "groupswrap") ||
             istreq(field, "wrapgroups")) {
        bool set;

        if (!ExprResolveBoolean(ctx, value, &set)) {
            log_err(info->keymap->ctx,
                    "Illegal groupsWrap setting for %s; "
                    "Non-boolean value ignored\n",
                    LongKeyNameText(keyi->name));
            return false;
        }

        if (set)
            keyi->out_of_range_group_action = RANGE_WRAP;
        else
            keyi->out_of_range_group_action = RANGE_SATURATE;

        keyi->defined |= KEY_FIELD_GROUPINFO;
    }
    else if (istreq(field, "groupsclamp") ||
             istreq(field, "clampgroups")) {
        bool set;

        if (!ExprResolveBoolean(ctx, value, &set)) {
            log_err(info->keymap->ctx,
                    "Illegal groupsClamp setting for %s; "
                    "Non-boolean value ignored\n",
                    LongKeyNameText(keyi->name));
            return false;
        }

        if (set)
            keyi->out_of_range_group_action = RANGE_SATURATE;
        else
            keyi->out_of_range_group_action = RANGE_WRAP;

        keyi->defined |= KEY_FIELD_GROUPINFO;
    }
    else if (istreq(field, "groupsredirect") ||
             istreq(field, "redirectgroups")) {
        xkb_layout_index_t grp;

        if (!ExprResolveGroup(ctx, value, &grp)) {
            log_err(info->keymap->ctx,
                    "Illegal group index for redirect of key %s; "
                    "Definition with non-integer group ignored\n",
                    LongKeyNameText(keyi->name));
            return false;
        }

        keyi->out_of_range_group_action = RANGE_REDIRECT;
        keyi->out_of_range_group_number = grp - 1;
        keyi->defined |= KEY_FIELD_GROUPINFO;
    }
    else {
        log_err(info->keymap->ctx,
                "Unknown field %s in a symbol interpretation; "
                "Definition ignored\n",
                field);
        ok = false;
    }

    return ok;
}

static int
SetGroupName(SymbolsInfo *info, ExprDef *arrayNdx, ExprDef *value)
{
    xkb_layout_index_t grp, grp_to_use;
    xkb_atom_t name;

    if (!arrayNdx) {
        log_vrb(info->keymap->ctx, 1,
                "You must specify an index when specifying a group name; "
                "Group name definition without array subscript ignored\n");
        return false;
    }

    if (!ExprResolveGroup(info->keymap->ctx, arrayNdx, &grp)) {
        log_err(info->keymap->ctx,
                "Illegal index in group name definition; "
                "Definition with non-integer array index ignored\n");
        return false;
    }

    if (!ExprResolveString(info->keymap->ctx, value, &name)) {
        log_err(info->keymap->ctx,
                "Group name must be a string; "
                "Illegal name for group %d ignored\n", grp);
        return false;
    }

    grp_to_use = XKB_LAYOUT_INVALID;
    if (info->explicit_group == XKB_LAYOUT_INVALID) {
        grp_to_use = grp - 1;
    }
    else if (grp - 1 == 0) {
        grp_to_use = info->explicit_group;
    }
    else {
        log_warn(info->keymap->ctx,
                 "An explicit group was specified for the '%s' map, "
                 "but it provides a name for a group other than Group1 (%d); "
                 "Ignoring group name '%s'\n",
                 info->name, grp,
                 xkb_atom_text(info->keymap->ctx, name));
        return false;
    }

    if (grp_to_use >= darray_size(info->group_names))
        darray_resize0(info->group_names, grp_to_use + 1);
    darray_item(info->group_names, grp_to_use) = name;
    return true;
}

static int
HandleGlobalVar(SymbolsInfo *info, VarDef *stmt)
{
    const char *elem, *field;
    ExprDef *arrayNdx;
    bool ret;

    if (ExprResolveLhs(info->keymap->ctx, stmt->name, &elem, &field,
                       &arrayNdx) == 0)
        return 0;               /* internal error, already reported */
    if (elem && istreq(elem, "key")) {
        ret = SetSymbolsField(info, &info->dflt, field, arrayNdx,
                              stmt->value);
    }
    else if (!elem && (istreq(field, "name") ||
                       istreq(field, "groupname"))) {
        ret = SetGroupName(info, arrayNdx, stmt->value);
    }
    else if (!elem && (istreq(field, "groupswrap") ||
                       istreq(field, "wrapgroups"))) {
        log_err(info->keymap->ctx,
                "Global \"groupswrap\" not supported; Ignored\n");
        ret = true;
    }
    else if (!elem && (istreq(field, "groupsclamp") ||
                       istreq(field, "clampgroups"))) {
        log_err(info->keymap->ctx,
                "Global \"groupsclamp\" not supported; Ignored\n");
        ret = true;
    }
    else if (!elem && (istreq(field, "groupsredirect") ||
                       istreq(field, "redirectgroups"))) {
        log_err(info->keymap->ctx,
                "Global \"groupsredirect\" not supported; Ignored\n");
        ret = true;
    }
    else if (!elem && istreq(field, "allownone")) {
        log_err(info->keymap->ctx,
                "Radio groups not supported; "
                "Ignoring \"allownone\" specification\n");
        ret = true;
    }
    else {
        ret = SetActionField(info->keymap, elem, field, arrayNdx, stmt->value,
                             info->actions);
    }

    return ret;
}

static bool
HandleSymbolsBody(SymbolsInfo *info, VarDef *def, KeyInfo *keyi)
{
    bool ok = true;
    const char *elem, *field;
    ExprDef *arrayNdx;

    for (; def; def = (VarDef *) def->common.next) {
        if (def->name && def->name->op == EXPR_FIELD_REF) {
            log_err(info->keymap->ctx,
                    "Cannot set a global default value from within a key statement; "
                    "Move statements to the global file scope\n");
            continue;
        }

        if (!def->name) {
            if (!def->value || def->value->op == EXPR_KEYSYM_LIST)
                field = "symbols";
            else
                field = "actions";
            arrayNdx = NULL;
        }
        else {
            ok = ExprResolveLhs(info->keymap->ctx, def->name, &elem, &field,
                                &arrayNdx);
        }

        if (ok)
            ok = SetSymbolsField(info, keyi, field, arrayNdx, def->value);
    }

    return ok;
}

static bool
SetExplicitGroup(SymbolsInfo *info, KeyInfo *keyi)
{
    xkb_layout_index_t i;
    GroupInfo *groupi;
    bool warn = false;

    if (info->explicit_group == XKB_LAYOUT_INVALID)
        return true;

    darray_enumerate_from(i, groupi, keyi->groups, 1) {
        if (groupi->defined) {
            warn = true;
            ClearGroupInfo(groupi);
            InitGroupInfo(groupi);
        }
    }

    if (warn)
        log_warn(info->keymap->ctx,
                 "For the map %s an explicit group specified, "
                 "but key %s has more than one group defined; "
                 "All groups except first one will be ignored\n",
                 info->name, LongKeyNameText(keyi->name));

    darray_resize0(keyi->groups, info->explicit_group + 1);
    if (info->explicit_group > 0) {
        darray_item(keyi->groups, info->explicit_group) =
            darray_item(keyi->groups, 0);
        InitGroupInfo(&darray_item(keyi->groups, 0));
    }

    return true;
}

static int
HandleSymbolsDef(SymbolsInfo *info, SymbolsDef *stmt)
{
    KeyInfo keyi;
    xkb_layout_index_t i;

    keyi = info->dflt;
    darray_init(keyi.groups);
    darray_copy(keyi.groups, info->dflt.groups);
    for (i = 0; i < darray_size(keyi.groups); i++)
        CopyGroupInfo(&darray_item(keyi.groups, i),
                      &darray_item(info->dflt.groups, i));
    keyi.merge = stmt->merge;
    keyi.name = KeyNameToLong(stmt->keyName);

    if (!HandleSymbolsBody(info, (VarDef *) stmt->symbols, &keyi)) {
        info->errorCount++;
        return false;
    }

    if (!SetExplicitGroup(info, &keyi)) {
        info->errorCount++;
        return false;
    }

    if (!AddKeySymbols(info, &keyi)) {
        info->errorCount++;
        return false;
    }

    return true;
}

static bool
HandleModMapDef(SymbolsInfo *info, ModMapDef *def)
{
    ExprDef *key;
    ModMapEntry tmp;
    xkb_mod_index_t ndx;
    bool ok;
    struct xkb_context *ctx = info->keymap->ctx;

    if (!LookupModIndex(ctx, NULL, def->modifier, EXPR_TYPE_INT, &ndx)) {
        log_err(info->keymap->ctx,
                "Illegal modifier map definition; "
                "Ignoring map for non-modifier \"%s\"\n",
                xkb_atom_text(ctx, def->modifier));
        return false;
    }

    ok = true;
    tmp.modifier = ndx;

    for (key = def->keys; key != NULL; key = (ExprDef *) key->common.next) {
        xkb_keysym_t sym;

        if (key->op == EXPR_VALUE && key->value_type == EXPR_TYPE_KEYNAME) {
            tmp.haveSymbol = false;
            tmp.u.keyName = KeyNameToLong(key->value.keyName);
        }
        else if (ExprResolveKeySym(ctx, key, &sym)) {
            tmp.haveSymbol = true;
            tmp.u.keySym = sym;
        }
        else {
            log_err(info->keymap->ctx,
                    "Modmap entries may contain only key names or keysyms; "
                    "Illegal definition for %s modifier ignored\n",
                    ModIndexText(tmp.modifier));
            continue;
        }

        ok = AddModMapEntry(info, &tmp) && ok;
    }
    return ok;
}

static void
HandleSymbolsFile(SymbolsInfo *info, XkbFile *file, enum merge_mode merge)
{
    bool ok;
    ParseCommon *stmt;

    free(info->name);
    info->name = strdup_safe(file->name);

    stmt = file->defs;
    for (stmt = file->defs; stmt; stmt = stmt->next) {
        switch (stmt->type) {
        case STMT_INCLUDE:
            ok = HandleIncludeSymbols(info, (IncludeStmt *) stmt);
            break;
        case STMT_SYMBOLS:
            ok = HandleSymbolsDef(info, (SymbolsDef *) stmt);
            break;
        case STMT_VAR:
            ok = HandleGlobalVar(info, (VarDef *) stmt);
            break;
        case STMT_VMOD:
            ok = HandleVModDef((VModDef *) stmt, info->keymap, merge,
                               &info->vmods);
            break;
        case STMT_MODMAP:
            ok = HandleModMapDef(info, (ModMapDef *) stmt);
            break;
        default:
            log_err(info->keymap->ctx,
                    "Interpretation files may not include other types; "
                    "Ignoring %s\n", stmt_type_to_string(stmt->type));
            ok = false;
            break;
        }

        if (!ok)
            info->errorCount++;

        if (info->errorCount > 10) {
            log_err(info->keymap->ctx, "Abandoning symbols file \"%s\"\n",
                    file->topName);
            break;
        }
    }
}

/**
 * Given a keysym @sym, return a key which generates it, or NULL.
 * This is used for example in a modifier map definition, such as:
 *      modifier_map Lock           { Caps_Lock };
 * where we want to add the Lock modifier to the modmap of the key
 * which matches the keysym Caps_Lock.
 * Since there can be many keys which generates the keysym, the key
 * is chosen first by lowest group in which the keysym appears, than
 * by lowest level and than by lowest key code.
 */
static struct xkb_key *
FindKeyForSymbol(struct xkb_keymap *keymap, xkb_keysym_t sym)
{
    struct xkb_key *key, *ret = NULL;
    xkb_layout_index_t group, min_group = UINT32_MAX;
    xkb_level_index_t level, min_level = UINT16_MAX;

    xkb_foreach_key(key, keymap) {
        for (group = 0; group < key->num_groups; group++) {
            for (level = 0; level < XkbKeyGroupWidth(keymap, key, group);
                 level++) {
                if (XkbKeyNumSyms(key, group, level) != 1 ||
                    (XkbKeySymEntry(key, group, level))[0] != sym)
                    continue;

                /*
                 * If the keysym was found in a group or level > 0, we must
                 * keep looking since we might find a key in which the keysym
                 * is in a lower group or level.
                 */
                if (group < min_group ||
                    (group == min_group && level < min_level)) {
                    ret = key;
                    if (group == 0 && level == 0) {
                        return ret;
                    }
                    else {
                        min_group = group;
                        min_level = level;
                    }
                }
            }
        }
    }

    return ret;
}

static bool
FindNamedType(struct xkb_keymap *keymap, xkb_atom_t name, unsigned *type_rtrn)
{
    unsigned int i;

    for (i = 0; i < keymap->num_types; i++) {
        if (keymap->types[i].name == name) {
            *type_rtrn = i;
            return true;
        }
    }

    return false;
}

/*
 * Find an appropriate type for a group and return its name.
 *
 * Simple recipe:
 * - ONE_LEVEL for width 0/1
 * - ALPHABETIC for 2 shift levels, with lower/upercase keysyms
 * - KEYPAD for keypad keys.
 * - TWO_LEVEL for other 2 shift level keys.
 * and the same for four level keys.
 *
 * FIXME: Decide how to handle multiple-syms-per-level, and do it.
 */
static bool
FindAutomaticType(struct xkb_context *ctx, GroupInfo *groupi,
                  xkb_atom_t *typeNameRtrn, bool *autoType)
{
    xkb_level_index_t width = darray_size(groupi->levels);

    *autoType = false;

#define GET_SYM(level) \
    (darray_item(groupi->levels, level).num_syms == 0 ? \
        XKB_KEY_NoSymbol : \
     darray_item(groupi->levels, level).num_syms == 1 ? \
        darray_item(groupi->levels, level).u.sym : \
     /* num_syms > 1 */ \
        darray_item(groupi->levels, level).u.syms[0])

    if (width == 1 || width == 0) {
        *typeNameRtrn = xkb_atom_intern(ctx, "ONE_LEVEL");
        *autoType = true;
    }
    else if (width == 2) {
        xkb_keysym_t sym0 = GET_SYM(0);
        xkb_keysym_t sym1 = GET_SYM(1);

        if (xkb_keysym_is_lower(sym0) && xkb_keysym_is_upper(sym1)) {
            *typeNameRtrn = xkb_atom_intern(ctx, "ALPHABETIC");
        }
        else if (xkb_keysym_is_keypad(sym0) || xkb_keysym_is_keypad(sym1)) {
            *typeNameRtrn = xkb_atom_intern(ctx, "KEYPAD");
            *autoType = true;
        }
        else {
            *typeNameRtrn = xkb_atom_intern(ctx, "TWO_LEVEL");
            *autoType = true;
        }
    }
    else if (width <= 4) {
        xkb_keysym_t sym0 = GET_SYM(0);
        xkb_keysym_t sym1 = GET_SYM(1);
        xkb_keysym_t sym2 = GET_SYM(2);
        xkb_keysym_t sym3 = (width == 4 ? GET_SYM(3) : XKB_KEY_NoSymbol);

        if (xkb_keysym_is_lower(sym0) && xkb_keysym_is_upper(sym1))
            if (xkb_keysym_is_lower(sym2) && xkb_keysym_is_upper(sym3))
                *typeNameRtrn =
                    xkb_atom_intern(ctx, "FOUR_LEVEL_ALPHABETIC");
            else
                *typeNameRtrn = xkb_atom_intern(ctx,
                                                "FOUR_LEVEL_SEMIALPHABETIC");

        else if (xkb_keysym_is_keypad(sym0) || xkb_keysym_is_keypad(sym1))
            *typeNameRtrn = xkb_atom_intern(ctx, "FOUR_LEVEL_KEYPAD");
        else
            *typeNameRtrn = xkb_atom_intern(ctx, "FOUR_LEVEL");
        /* XXX: why not set autoType here? */
    }
    return width <= 4;
}

static bool
CopySymbolsDef(SymbolsInfo *info, KeyInfo *keyi)
{
    struct xkb_keymap *keymap = info->keymap;
    struct xkb_key *key;
    GroupInfo *groupi;
    const GroupInfo *group0;
    xkb_layout_index_t i;

    /*
     * The name is guaranteed to be real and not an alias (see
     * AddKeySymbols), so 'false' is safe here.
     */
    key = FindNamedKey(keymap, keyi->name, false);
    if (!key) {
        log_vrb(info->keymap->ctx, 5,
                "Key %s not found in keycodes; Symbols ignored\n",
                LongKeyNameText(keyi->name));
        return false;
    }

    /* Find the range of groups we need. */
    key->num_groups = 0;
    darray_enumerate(i, groupi, keyi->groups)
        if (groupi->defined)
            key->num_groups = i + 1;

    if (key->num_groups <= 0)
        return false; /* WSGO */

    darray_resize(keyi->groups, key->num_groups);

    /*
     * If there are empty groups between non-empty ones, fill them with data
     * from the first group.
     * We can make a wrong assumption here. But leaving gaps is worse.
     */
    group0 = &darray_item(keyi->groups, 0);
    darray_foreach_from(groupi, keyi->groups, 1) {
        if (groupi->defined)
            continue;

        CopyGroupInfo(groupi, group0);
    }

    key->groups = calloc(key->num_groups, sizeof(*key->groups));

    /* * Find and assign the groups' types in the keymap. */
    darray_enumerate(i, groupi, keyi->groups) {
        struct xkb_key_type *type;
        struct xkb_group *group = &key->groups[i];
        bool autoType = false;

        /* Find the type of the group, if it is missing. */
        if (groupi->type == XKB_ATOM_NONE) {
            if (keyi->dfltType != XKB_ATOM_NONE)
                groupi->type = keyi->dfltType;
            else if (FindAutomaticType(keymap->ctx, groupi,
                                       &groupi->type, &autoType)) { }
            else
                log_vrb(info->keymap->ctx, 5,
                        "No automatic type for %d levels; "
                        "Using %s for the %s key\n",
                        (int) darray_size(groupi->levels),
                        xkb_atom_text(keymap->ctx, groupi->type),
                        LongKeyNameText(keyi->name));
        }

        /* Find the type in the keymap, if it was defined in xkb_types. */
        if (FindNamedType(keymap, groupi->type, &group->type_index)) {
            if (!autoType || darray_size(groupi->levels) > 2)
                key->groups[i].explicit_type = true;
        }
        else {
            log_vrb(info->keymap->ctx, 3,
                    "Type \"%s\" is not defined; "
                    "Using default type for the %s key\n",
                    xkb_atom_text(keymap->ctx, groupi->type),
                    LongKeyNameText(keyi->name));
            /*
             * Index 0 is guaranteed to contain something, usually
             * ONE_LEVEL or at least some default one-level type.
             */
            group->type_index = 0;
        }

        /* Always have as many levels as the type specifies. */
        type = &keymap->types[group->type_index];
        if (type->num_levels < darray_size(groupi->levels)) {
            struct xkb_level *leveli;

            log_vrb(info->keymap->ctx, 1,
                    "Type \"%s\" has %d levels, but %s has %d levels; "
                    "Ignoring extra symbols\n",
                    xkb_atom_text(keymap->ctx, type->name),
                    type->num_levels,
                    LongKeyNameText(keyi->name),
                    (int) darray_size(groupi->levels));

            darray_foreach_from(leveli, groupi->levels, type->num_levels)
                ClearLevelInfo(leveli);
        }
        darray_resize0(groupi->levels, type->num_levels);
    }

    /* Copy levels. */
    darray_enumerate(i, groupi, keyi->groups) {
        key->groups[i].levels = darray_mem(groupi->levels, 0);
        darray_init(groupi->levels);
    }

    key->out_of_range_group_number = keyi->out_of_range_group_number;
    key->out_of_range_group_action = keyi->out_of_range_group_action;

    if (keyi->defined & KEY_FIELD_VMODMAP) {
        key->vmodmap = keyi->vmodmap;
        key->explicit |= EXPLICIT_VMODMAP;
    }

    if (keyi->repeat != KEY_REPEAT_UNDEFINED) {
        key->repeats = (keyi->repeat == KEY_REPEAT_YES);
        key->explicit |= EXPLICIT_REPEAT;
    }

    darray_foreach(groupi, keyi->groups) {
        if (groupi->defined & GROUP_FIELD_ACTS) {
            key->explicit |= EXPLICIT_INTERP;
            break;
        }
    }

    return true;
}

static bool
CopyModMapDef(SymbolsInfo *info, ModMapEntry *entry)
{
    struct xkb_key *key;
    struct xkb_keymap *keymap = info->keymap;

    if (!entry->haveSymbol) {
        key = FindNamedKey(keymap, entry->u.keyName, true);
        if (!key) {
            log_vrb(info->keymap->ctx, 5,
                    "Key %s not found in keycodes; "
                    "Modifier map entry for %s not updated\n",
                    LongKeyNameText(entry->u.keyName),
                    ModIndexText(entry->modifier));
            return false;
        }
    }
    else {
        key = FindKeyForSymbol(keymap, entry->u.keySym);
        if (!key) {
            log_vrb(info->keymap->ctx, 5,
                    "Key \"%s\" not found in symbol map; "
                    "Modifier map entry for %s not updated\n",
                    KeysymText(entry->u.keySym),
                    ModIndexText(entry->modifier));
            return false;
        }
    }

    key->modmap |= (1 << entry->modifier);
    return true;
}

static bool
CopySymbolsToKeymap(struct xkb_keymap *keymap, SymbolsInfo *info)
{
    KeyInfo *keyi;
    ModMapEntry *mm;
    struct xkb_key *key;

    keymap->symbols_section_name = strdup_safe(info->name);

    keymap->group_names = info->group_names;
    darray_init(info->group_names);

    darray_foreach(keyi, info->keys)
        if (!CopySymbolsDef(info, keyi))
            info->errorCount++;

    if (xkb_context_get_log_verbosity(keymap->ctx) > 3) {
        xkb_foreach_key(key, keymap) {
            if (key->name[0] == '\0')
                continue;

            if (key->num_groups < 1)
                log_info(keymap->ctx,
                         "No symbols defined for %s\n",
                         KeyNameText(key->name));
        }
    }

    darray_foreach(mm, info->modMaps)
        if (!CopyModMapDef(info, mm))
            info->errorCount++;

    /* XXX: If we don't ignore errorCount, things break. */
    return true;
}

bool
CompileSymbols(XkbFile *file, struct xkb_keymap *keymap,
               enum merge_mode merge)
{
    SymbolsInfo info;
    ActionsInfo *actions;

    actions = NewActionsInfo();
    if (!actions)
        return false;

    InitSymbolsInfo(&info, keymap, file->id, actions);
    info.dflt.merge = merge;

    HandleSymbolsFile(&info, file, merge);

    if (darray_empty(info.keys))
        goto err_info;

    if (info.errorCount != 0)
        goto err_info;

    if (!CopySymbolsToKeymap(keymap, &info))
        goto err_info;

    ClearSymbolsInfo(&info);
    FreeActionsInfo(actions);
    return true;

err_info:
    FreeActionsInfo(actions);
    ClearSymbolsInfo(&info);
    return false;
}
