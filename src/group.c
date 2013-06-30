//
//  Copyright (C) 2013  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "tree.h"
#include "phase.h"
#include "common.h"

#include <assert.h>

typedef struct group group_t;

struct group {
   group_t  *next;
   netid_t   first;
   unsigned  length;
};

typedef struct {
   group_t *groups;
} group_nets_ctx_t;

static void group_add(group_nets_ctx_t *ctx, netid_t first, unsigned length)
{
   printf("group_add first=%d len=%d\n", first, length);
}

static void group_ref(tree_t target, group_nets_ctx_t *ctx, int start, int n)
{
   tree_t decl = tree_ref(target);
   printf("ref %s start=%d n=%d\n", istr(tree_ident(decl)), start, n);

   if (tree_kind(decl) != T_SIGNAL_DECL)
      return;

   netid_t first = NETID_INVALID;
   unsigned len = 0;
   const int nnets = tree_nets(decl);
   assert((n == -1) | (start + n <= nnets));
   for (int i = start; i < (n == -1 ? nnets : start + n); i++) {
      netid_t nid = tree_net(decl, i);
      if (first == NETID_INVALID) {
         first = nid;
         len   = 1;
      }
      else if (nid == first + len)
         ++len;
      else {
         group_add(ctx, first, len);
         first = nid;
         len   = 1;
      }
   }

   assert(first != NETID_INVALID);
   group_add(ctx, first, len);
}

static int64_t rebase_index(type_t array_type, int dim, int64_t value)
{
   int64_t low, high;
   range_bounds(type_dim(array_type, dim), &low, &high);
   return value - low;
}

static void group_array_ref(tree_t target, group_nets_ctx_t *ctx)
{
   tree_t value = tree_value(target);

   switch (tree_kind(value)) {
   case T_REF:
      {
         type_t type = tree_type(value);
         if (type_kind(type) == T_UARRAY)
            return;

         const int width  = type_width(type);
         const int stride = type_width(type_elem(type));

         assert(tree_params(target) == 1);
         tree_t index = tree_value(tree_param(target, 0));

         int64_t offset = -1;
         if (tree_kind(index) == T_LITERAL)
            offset = stride * rebase_index(type, 0, assume_int(index));

         printf("array ref width=%d stride=%d offset=%d\n",
                width, stride, (int)offset);

         if (offset == -1) {
            for (int i = 0; i < width; i += stride)
               group_ref(value, ctx, i, stride);
         }
         else
            group_ref(value, ctx, offset, stride);
      }
      break;

   case T_ARRAY_REF:
   case T_ARRAY_SLICE:
      {
         // We could handle this better but for now just map each
         // net to a single group
         while (tree_kind(value) != T_REF)
            value = tree_value(value);

         tree_t decl = tree_ref(value);
         if (tree_kind(decl) != T_SIGNAL_DECL)
            return;

         const int nnets = tree_nets(decl);
         for (int i = 0; i < nnets; i++)
            group_add(ctx, tree_net(decl, i), 1);
      }
      break;

   default:
      assert(false);
   }
}

static void group_array_slice(tree_t target, group_nets_ctx_t *ctx)
{
   tree_t value = tree_value(target);
   type_t type  = tree_type(value);

   //const int width  = type_width(type);
   const int stride = type_width(type_elem(type));

   range_t slice = tree_range(target);

   const bool folded =
      (tree_kind(slice.left) == T_LITERAL)
      && (tree_kind(slice.right) == T_LITERAL);

   printf("array slice stride=%d folded=%d\n", stride, folded);

   switch (tree_kind(value)) {
   case T_REF:
      if (folded) {
         int64_t low, high;
         range_bounds(slice, &low, &high);

         const int64_t low0 = rebase_index(type, 0, low);

         group_ref(value, ctx, low0, high - low + 1);
      }
      else {
         tree_t decl = tree_ref(value);

         const int nnets = tree_nets(decl);
         for (int i = 0; i < nnets; i++)
            group_add(ctx, tree_net(decl, i), 1);
      }
      break;

   default:
      assert(false);
   }
}

static void group_nets_visit_fn(tree_t t, void *_ctx)
{
   group_nets_ctx_t *ctx = _ctx;

   fmt_loc(stdout, tree_loc(t));

   tree_t target = tree_target(t);

   switch (tree_kind(target)) {
   case T_REF:
      group_ref(target, ctx, 0, -1);
      break;

   case T_ARRAY_REF:
      group_array_ref(target, ctx);
      break;

   case T_ARRAY_SLICE:
      group_array_slice(target, ctx);
      break;

   default:
      assert(false);
   }
}

static void ungroup_proc_params(tree_t t, void *_ctx)
{
   // Ungroup any signal that is passed to a procedure as in general we
   // cannot guarantee anything about the procedure's behaviour

   group_nets_ctx_t *ctx = _ctx;

   const int nparams = tree_params(t);
   for (int i = 0; i < nparams; i++) {
      tree_t value = tree_value(tree_param(t, i));

      while (tree_kind(value) != T_REF) {
         switch (tree_kind(value)) {
         case T_ARRAY_REF:
         case T_ARRAY_SLICE:
            value = tree_value(value);
            break;

         default:
            return;
         }
      }

      tree_t decl = tree_ref(value);

      if (tree_kind(decl) != T_SIGNAL_DECL)
         return;

      printf("ungroup nets of %s - pcall\n", istr(tree_ident(decl)));

      const int nnets = tree_nets(decl);
      for (int i = 0; i < nnets; i++)
         group_add(ctx, tree_net(decl, i), 1);
   }
}

void group_nets(tree_t top)
{
   group_nets_ctx_t ctx = {
      .groups = NULL
   };
   tree_visit_only(top, group_nets_visit_fn, &ctx, T_SIGNAL_ASSIGN);
   tree_visit_only(top, ungroup_proc_params, &ctx, T_PCALL);
}