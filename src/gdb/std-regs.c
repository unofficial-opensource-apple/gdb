/* Builtin frame register, for GDB, the GNU debugger.

   Copyright 2002 Free Software Foundation, Inc.

   Contributed by Red Hat.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include "defs.h"
#include "builtin-regs.h"
#include "frame.h"
#include "gdbtypes.h"
#include "value.h"
#include "gdb_string.h"

/* Types that describe the various builtin registers.  */

static struct type *builtin_type_frame_reg;

/* Constructors for those types.  */

static void
build_builtin_type_frame_reg (void)
{
  /* $frame.  */
  if (builtin_type_frame_reg == NULL)
    {
#if 0
      struct frame
      {
	void *base;
      };
#endif
      builtin_type_frame_reg = init_composite_type ("frame", TYPE_CODE_STRUCT);
      append_composite_type_field (builtin_type_frame_reg, "base",
				   builtin_type_void_data_ptr);
    }
}

static struct value *
value_of_builtin_frame_reg (struct frame_info *frame)
{
  struct value *val;
  char *buf;
  build_builtin_type_frame_reg ();
  val = allocate_value (builtin_type_frame_reg);
  VALUE_LVAL (val) = not_lval;
  buf = VALUE_CONTENTS_RAW (val);
  memset (buf, TYPE_LENGTH (VALUE_TYPE (val)), 0);
  /* frame.base.  */
  if (frame != NULL)
    ADDRESS_TO_POINTER (builtin_type_void_data_ptr, buf,
			get_frame_base (frame));
  buf += TYPE_LENGTH (builtin_type_void_data_ptr);
  /* frame.XXX.  */
  return val;
}

static struct value *
value_of_builtin_frame_fp_reg (struct frame_info *frame)
{
#ifdef FP_REGNUM
  if (FP_REGNUM >= 0)
    return value_of_register (FP_REGNUM, frame);
#endif
  {
    struct value *val = allocate_value (builtin_type_void_data_ptr);
    char *buf = VALUE_CONTENTS_RAW (val);
    if (frame == NULL)
      memset (buf, TYPE_LENGTH (VALUE_TYPE (val)), 0);
    else
      ADDRESS_TO_POINTER (builtin_type_void_data_ptr, buf,
			  get_frame_base (frame));
    return val;
  }
}

static struct value *
value_of_builtin_frame_pc_reg (struct frame_info *frame)
{
#ifdef PC_REGNUM
  if (PC_REGNUM >= 0)
    return value_of_register (PC_REGNUM, frame);
#endif
  {
    struct value *val = allocate_value (builtin_type_void_data_ptr);
    char *buf = VALUE_CONTENTS_RAW (val);
    if (frame == NULL)
      memset (buf, TYPE_LENGTH (VALUE_TYPE (val)), 0);
    else
      ADDRESS_TO_POINTER (builtin_type_void_data_ptr, buf,
			  get_frame_pc (frame));
    return val;
  }
}

static struct value *
value_of_builtin_frame_sp_reg (struct frame_info *frame)
{
#ifdef SP_REGNUM
  if (SP_REGNUM >= 0)
    return value_of_register (SP_REGNUM, frame);
#endif
  error ("Standard register ``$sp'' is not available for this target");
}

static struct value *
value_of_builtin_frame_ps_reg (struct frame_info *frame)
{
#ifdef PS_REGNUM
  if (PS_REGNUM >= 0)
    return value_of_register (PS_REGNUM, frame);
#endif
  error ("Standard register ``$ps'' is not available for this target");
}

void
_initialize_frame_reg (void)
{
  /* FIXME: cagney/2002-02-08: At present the local builtin types
     can't be initialized using _initialize*() or gdbarch.  Due mainly
     to non-multi-arch targets, GDB initializes things piece meal and,
     as a consequence can leave these types NULL.  */
  REGISTER_GDBARCH_SWAP (builtin_type_frame_reg);

  /* Frame based $fp, $pc, $sp and $ps.  These only come into play
     when the target does not define its own version of these
     registers.  */
  add_builtin_reg ("fp", value_of_builtin_frame_fp_reg);
  add_builtin_reg ("pc", value_of_builtin_frame_pc_reg);
  add_builtin_reg ("sp", value_of_builtin_frame_sp_reg);
  add_builtin_reg ("ps", value_of_builtin_frame_ps_reg);

  /* NOTE: cagney/2002-04-05: For moment leave the $frame / $gdbframe
     / $gdb.frame disabled.  It isn't yet clear which of the many
     options is the best.  */
  if (0)
    add_builtin_reg ("frame", value_of_builtin_frame_reg);
}
