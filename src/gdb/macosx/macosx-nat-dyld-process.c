/* Mac OS X support for GDB, the GNU debugger.
   Copyright 1997, 1998, 1999, 2000, 2001, 2002
   Free Software Foundation, Inc.

   Contributed by Apple Computer, Inc.

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

#include "cached-symfile.h"
#include "macosx-nat-dyld-info.h"
#include "macosx-nat-dyld-path.h"
#include "macosx-nat-dyld-io.h"
#include "macosx-nat-dyld.h"
#include "macosx-nat-inferior.h"
#include "macosx-nat-mutils.h"
#include "macosx-nat-dyld-process.h"

#include "defs.h"
#include "inferior.h"
#include "symfile.h"
#include "symtab.h"
#include "gdbcmd.h"
#include "objfiles.h"
#include "mach-o.h"
#include "gdbcore.h"
#include "interpreter.h"
#include "gdb_regex.h"
#include "gdb-stabs.h"

#include <mach-o/nlist.h>
#include <mach-o/loader.h>
#include <mach-o/dyld_debug.h>

#ifdef USE_MMALLOC
#include "mmprivate.h"
#endif

#include <sys/mman.h>
#include <string.h>

extern int dyld_preload_libraries_flag;
extern int dyld_filter_events_flag;
extern int dyld_always_read_from_memory_flag;
extern char *dyld_symbols_prefix;
extern int dyld_load_dyld_symbols_flag;
extern int dyld_load_dyld_shlib_symbols_flag;
extern int dyld_load_cfm_shlib_symbols_flag;
extern int dyld_print_basenames_flag;
extern char *dyld_load_rules;
extern char *dyld_minimal_load_rules;

#if WITH_CFM
extern int inferior_auto_start_cfm_flag;
#endif /* WITH_CFM */

extern macosx_inferior_status *macosx_status;

static int
dyld_print_status()
{
    /* do not print status dots when executing MI */
  return !ui_out_is_mi_like_p (uiout);
}

void dyld_add_inserted_libraries
  (struct dyld_objfile_info *info, const struct dyld_path_info *d)
{
  const char *s1, *s2;

  CHECK_FATAL (info != NULL);
  CHECK_FATAL (d != NULL);

  s1 = d->insert_libraries;
  if (s1 == NULL) { return; }

  while (*s1 != '\0') {

    struct dyld_objfile_entry *e = NULL;

    s2 = strchr (s1, ':');
    if (s2 == NULL) {
      s2 = strchr (s1, '\0');
    }
    CHECK_FATAL (s2 != NULL);

    e = dyld_objfile_entry_alloc (info);

    e->user_name = savestring (s1, (s2 - s1));
    e->reason = dyld_reason_init;

    s1 = s2; 
    while (*s1 == ':') {
      s1++;
    }
  }
}

void dyld_add_image_libraries
  (struct dyld_objfile_info *info, bfd *abfd)
{
  struct mach_o_data_struct *mdata = NULL;
  unsigned int i;

  CHECK_FATAL (info != NULL);

  if (abfd == NULL) { return; }

  if (! bfd_mach_o_valid (abfd)) { return;  }

  mdata = abfd->tdata.mach_o_data;

  if (mdata == NULL)
    {
      dyld_debug ("dyld_add_image_libraries: mdata == NULL\n");
      return;
    }

  for (i = 0; i < mdata->header.ncmds; i++) {
    struct bfd_mach_o_load_command *cmd = &mdata->commands[i];
    switch (cmd->type) {
    case BFD_MACH_O_LC_LOAD_WEAK_DYLIB:
    case BFD_MACH_O_LC_LOAD_DYLINKER:
    case BFD_MACH_O_LC_LOAD_DYLIB: {

      struct dyld_objfile_entry *e = NULL;
      char *name = NULL;

      switch (cmd->type) {
      case BFD_MACH_O_LC_LOAD_DYLINKER: {
	bfd_mach_o_dylinker_command *dcmd = &cmd->command.dylinker;

	name = xmalloc (dcmd->name_len + 1);
            
	bfd_seek (abfd, dcmd->name_offset, SEEK_SET);
	if (bfd_bread (name, dcmd->name_len, abfd) != dcmd->name_len) {
	  warning ("Unable to find library name for LC_LOAD_DYLINKER command; ignoring");
	  xfree (name);
	  continue;
	}
	break;
      }
      case BFD_MACH_O_LC_LOAD_DYLIB:
      case BFD_MACH_O_LC_LOAD_WEAK_DYLIB: {
	bfd_mach_o_dylib_command *dcmd = &cmd->command.dylib;

	name = xmalloc (dcmd->name_len + 1);
            
	bfd_seek (abfd, dcmd->name_offset, SEEK_SET);
	if (bfd_bread (name, dcmd->name_len, abfd) != dcmd->name_len) {
	  warning ("Unable to find library name for LC_LOAD_DYLIB or LC_LOAD_WEAK_DYLIB command; ignoring");
	  xfree (name);
	  continue;
	}
	break;
      }
      default:
	abort ();
      }
      
      if (name[0] == '\0') {
	warning ("No image name specified by LC_LOAD command; ignoring");
	xfree (name);
	name = NULL;
      }

      e = dyld_objfile_entry_alloc (info);

      e->text_name = name;
      e->text_name_valid = 1;
      e->reason = dyld_reason_init;

      switch (cmd->type) {
      case BFD_MACH_O_LC_LOAD_DYLINKER:
	e->prefix = dyld_symbols_prefix;
	break;
      case BFD_MACH_O_LC_LOAD_DYLIB:
	break;
      case BFD_MACH_O_LC_LOAD_WEAK_DYLIB:
	e->reason |= dyld_reason_weak_mask;
	break;
      default:
	abort ();
      };
    }
    default:
      break;
    }
  }
}

void dyld_resolve_filename_image
(const struct macosx_dyld_thread_status *s, struct dyld_objfile_entry *e)
{
  struct mach_header header;
  CHECK_FATAL (e->allocated);
  if (e->image_name_valid) { return; }

  if (! e->dyld_valid) { return; }

  target_read_memory (e->dyld_addr, (char *) &header, 
		      sizeof (struct mach_header));

  switch (header.filetype) {
  case MH_DYLINKER:
  case MH_DYLIB:
    break;
  case MH_BUNDLE:
    break;
  default:
    return;
  }
  e->image_name = dyld_find_dylib_name (header.ncmds, e->dyld_addr);

  if (e->image_name == NULL) {
    dyld_debug ("Unable to determine filename for loaded object "
		"(no LC_ID load command)\n");
  } else {
    dyld_debug ("Determined filename for loaded object from image\n");
    e->image_name_valid = 1;
  }		
}

/* Assuming a Mach header starts at ADDR, and has NCMDS, look for the
   dylib name, and return a malloc'ed string containing the name */

char *
dyld_find_dylib_name (CORE_ADDR addr, unsigned int ncmds)
{
  CORE_ADDR curpos;
  unsigned int i;
  char *image_name = NULL;

  curpos = ((unsigned long) addr) + sizeof (struct mach_header);
  for (i = 0; i < ncmds; i++) {

    struct load_command cmd;
    struct dylib_command dcmd;
    struct dylinker_command dlcmd;
    char name[256];

    target_read_memory (curpos, (char *) &cmd, sizeof (struct load_command));
    if (cmd.cmd == LC_ID_DYLIB) {
      target_read_memory (curpos, (char *) &dcmd, sizeof (struct dylib_command));
      target_read_memory (curpos + dcmd.dylib.name.offset, name, 256);
      image_name = savestring (name, strlen (name));
      break;
    } else if (cmd.cmd == LC_ID_DYLINKER) {
      target_read_memory (curpos, (char *) &dlcmd, sizeof (struct dylinker_command));
      target_read_memory (curpos + dlcmd.name.offset, name, 256);
      image_name = savestring (name, strlen (name));
      break;
    }

    curpos += cmd.cmdsize;
  }
  return image_name;
}

void dyld_resolve_filenames
(const struct macosx_dyld_thread_status *s, struct dyld_objfile_info *new)
{
  unsigned int i;

  CHECK_FATAL (s != NULL);
  CHECK_FATAL (new != NULL);

  for (i = 0; i < new->nents; i++) {
    struct dyld_objfile_entry *e = &new->entries[i];
    if (! e->allocated) { continue; }
    if (e->dyld_name_valid) { continue; }
    dyld_resolve_filename_image (s, e);
  }
}

static CORE_ADDR library_offset (struct dyld_objfile_entry *e)
{
  CHECK_FATAL (e != NULL);
  if (e->image_addr_valid && e->dyld_valid) {
    CHECK_FATAL (e->dyld_addr == ((e->image_addr + e->dyld_slide) & 0xffffffff));
  }

  if (e->dyld_valid) {
    return (unsigned long) e->dyld_addr;
  } else if (e->image_addr_valid) {
    return (unsigned long) e->image_addr;
  } else {
    return 0;
  }
}

unsigned int
dyld_parse_load_level (const char *s)
{
  if (strcmp (s, "all") == 0) {
    return OBJF_SYM_ALL;
  } else if (strcmp (s, "container") == 0) {
    return OBJF_SYM_CONTAINER;
  } else if (strcmp (s, "extern") == 0) {
    return OBJF_SYM_EXTERN;
  } else if (strcmp (s, "none") == 0) {
    return OBJF_SYM_NONE;
  } else {
    warning ("unknown setting \"%s\"; using \"none\"\n", s);
    return OBJF_SYM_NONE;
  }
}

int
dyld_resolve_load_flag (const struct dyld_path_info *d, struct dyld_objfile_entry *e, const char *rules)
{
  const char *name = NULL;
  const char *leaf = NULL;

  char **prules = NULL;
  char **trule = NULL;
  int nrules = 0;
  int crule = 0;

  name = dyld_entry_string (e, 1);

  if (name == NULL)
    return OBJF_SYM_NONE;

  leaf = strrchr (name, '/');
  leaf = ((leaf != NULL) ? leaf : name);

  if (rules != NULL) { 
    prules = buildargv (rules);
    if (prules == NULL) {
      warning("unable to parse load rules");
      return OBJF_SYM_NONE;
    }
  }

  nrules = 0;

  if (prules != NULL) {
    for (trule = prules; *trule != NULL; trule++) {
      nrules++;
    }
  }

  if ((nrules % 3) != 0) {
    warning ("unable to parse load-rules (number of rule clauses must be a multiple of 3)");
    return OBJF_SYM_NONE;
  }
  nrules /= 3;

  for (crule = 0; crule < nrules; crule++) {

    char *matchreason = prules[crule * 3];
    char *matchname = prules[(crule * 3) + 1];
    char *setting = prules[(crule * 3) + 2];

    const char *reason = NULL;
    const char *name = NULL;

    regex_t reasonbuf;
    regex_t namebuf;

    int ret;

    reason = dyld_reason_string (e->reason);

    if (e->objfile) {
      if (e->loaded_from_memory) {
	name = "memory";
      } else {
	name = e->loaded_name;
      }
    } else {
      name = dyld_entry_filename (e, d, 0);
      if (name == NULL) {
	if (! (e->reason & dyld_reason_weak_mask)) {
	  warning ("Unable to resolve \"%s\"; not loading.", name);
	}
	return OBJF_SYM_NONE;
      }
    }

    ret = regcomp (&reasonbuf, matchreason, REG_NOSUB);
    if (ret != 0) {
      warning ("unable to compile regular expression \"%s\"", matchreason);
      continue;
    }
    
    ret = regcomp (&namebuf, matchname, REG_NOSUB);
    if (ret != 0) {
      warning ("unable to compile regular expression \"%s\"", matchreason);
      continue;
    }

    ret = regexec (&reasonbuf, reason, 0, 0, 0);
    if (ret != 0)
      continue;

    ret = regexec (&namebuf, name, 0, 0, 0);
    if (ret != 0)
      continue;

    return dyld_parse_load_level (setting);
  }

  return -1;
}

int dyld_minimal_load_flag (const struct dyld_path_info *d, struct dyld_objfile_entry *e)
{
  int ret = dyld_resolve_load_flag (d, e, dyld_minimal_load_rules);
  return (ret >= 0) ? ret : OBJF_SYM_NONE;
}

int dyld_default_load_flag (const struct dyld_path_info *d, struct dyld_objfile_entry *e)
{
  int ret = dyld_resolve_load_flag (d, e, dyld_load_rules);
  if (ret >= 0)
    return ret;

  if (e->reason != dyld_reason_cfm) {
    if (dyld_load_dyld_shlib_symbols_flag)
      return OBJF_SYM_ALL;
  } else {
    if (dyld_load_cfm_shlib_symbols_flag)
      return OBJF_SYM_ALL;
  }
  
  return OBJF_SYM_NONE;
}

void dyld_load_library (const struct dyld_path_info *d, struct dyld_objfile_entry *e)
{
  int read_from_memory = 0;
  const char *name = NULL;

  CHECK_FATAL (e->allocated);

  if (e->abfd) { return; }
  if (e->loaded_error) { return; }

  if (e->reason & dyld_reason_executable_mask) {
    CHECK_FATAL (e->objfile == symfile_objfile);
  }

  if (e->reason == dyld_reason_cfm) {
    read_from_memory = 1;
  }

  if (dyld_always_read_from_memory_flag) {
    read_from_memory = 1;
  }

  if (! read_from_memory) {
    name = dyld_entry_filename (e, d, 0);
    if (name == NULL) {
      char *s = dyld_entry_string (e, 1);
      warning ("No image filename available for %s.", s);
      xfree (s);
      read_from_memory = 1;
    }
  }

  if (! read_from_memory) {
    CHECK_FATAL (name != NULL);
    e->abfd = symfile_bfd_open_safe (name, 0);
    if (e->abfd == NULL) {
      char *s = dyld_entry_string (e, 1);
      warning ("Unable to read symbols from %s.", s);
      xfree (s);
    }
    e->loaded_name = name;
    e->loaded_from_memory = 0;
  }	
  
  if (read_from_memory && (! e->dyld_valid)) {
      char *s = dyld_entry_string (e, dyld_print_basenames_flag);
      warning ("Unable to read symbols from %s (not yet mapped into memory); skipping", s);
      return;
  }    

  if (read_from_memory && (e->dyld_valid)) {
    CHECK_FATAL (e->abfd == NULL);
    e->abfd = inferior_bfd (name, e->dyld_addr, e->dyld_slide, e->dyld_length);
    e->loaded_memaddr = e->dyld_addr;
    e->loaded_from_memory = 1;
  }

  if (e->abfd == NULL) {
    char *s = dyld_entry_string (e, 1);
    e->loaded_error = 1;
    warning ("Unable to read symbols from %s; skipping.", s);
    xfree (s);
    return;
  }

  if (e->reason & dyld_reason_image_mask) 
    {
      asection *text_sect = bfd_get_section_by_name (e->abfd, "LC_SEGMENT.__TEXT");
      if (text_sect != NULL) {
	e->image_addr = bfd_section_vma (e->abfd, text_sect);
	e->image_addr_valid = 1;
      }
    }

  if (e->reason & dyld_reason_executable_mask) {
    symfile_objfile = e->objfile;
  }
}

void dyld_load_libraries (const struct dyld_path_info *d, struct dyld_objfile_info *result)
{
  unsigned int i;
  CHECK_FATAL (result != NULL);

  for (i = 0; i < result->nents; i++) {
    struct dyld_objfile_entry *e = &result->entries[i];
    if (! e->allocated) { continue; }
    if (e->load_flag < 0) {
      e->load_flag = dyld_default_load_flag (d, e) | dyld_minimal_load_flag (d, e);
    }
    if (e->load_flag) {
      dyld_load_library (d, e);
    }
  }
}

void dyld_symfile_loaded_hook (struct objfile *o)
{
#if WITH_CFM
  if (strstr (o->name, "CarbonCore") == NULL)
    return;

  struct minimal_symbol *hooksym = lookup_minimal_symbol ("gPCFMInfoHooks", NULL, NULL);
  struct minimal_symbol *system = lookup_minimal_symbol ("gPCFMSystemUniverse", NULL, NULL);
  struct minimal_symbol *context = lookup_minimal_symbol ("gPCFMContextUniverse", NULL, NULL);
  struct cfm_parser *parser = &macosx_status->cfm_status.parser;
  CORE_ADDR offset = 0;
 
  if ((hooksym == NULL) || (system == NULL) || (context == NULL))
    return;

  offset = SYMBOL_VALUE_ADDRESS (context) - SYMBOL_VALUE_ADDRESS (system);

  if (offset == 88) {
    parser->version = 3;
    parser->universe_length = 88;
    parser->universe_container_offset = 48;
    parser->universe_connection_offset = 60;
    parser->universe_closure_offset = 72;
    parser->connection_length = 68;
    parser->connection_next_offset = 0;
    parser->connection_container_offset = 28;
    parser->container_length = 176;
    parser->container_address_offset = 24;
    parser->container_length_offset = 28;
    parser->container_fragment_name_offset = 44;
    parser->container_section_count_offset = 100;
    parser->container_sections_offset = 104;
    parser->section_length = 24;
    parser->section_total_length_offset = 12;
    parser->instance_length = 24;
    parser->instance_address_offset = 12;
    macosx_status->cfm_status.breakpoint_offset = 956;
  } else if (offset == 104) {
    parser->version = 2;
    parser->universe_length = 104;
    parser->universe_container_offset = 52;
    parser->universe_connection_offset = 68;
    parser->universe_closure_offset = 84;
    parser->connection_length = 72;
    parser->connection_next_offset = 0;
    parser->connection_container_offset = 32;
    parser->container_length = 176;
    parser->container_address_offset = 28;
    parser->container_length_offset = 36;
    parser->container_fragment_name_offset = 44;
    parser->container_section_count_offset = 100;
    parser->container_sections_offset = 104;
    parser->section_length = 24;
    parser->section_total_length_offset = 12;
    parser->instance_length = 24;
    parser->instance_address_offset = 12;
    macosx_status->cfm_status.breakpoint_offset = 864;
  } else if (offset == 120) {
    parser->version = 1;
    parser->universe_length = 120;
    parser->universe_container_offset = 68;
    parser->universe_connection_offset = 84;
    parser->universe_closure_offset = 100;
    parser->connection_length = 84;
    parser->connection_next_offset = 0;
    parser->connection_container_offset = 36;
    parser->container_length = 172;
    parser->container_address_offset = 28;
    parser->container_length_offset = 32;
    parser->container_fragment_name_offset = 40;
    parser->container_section_count_offset = 96;
    parser->container_sections_offset = 100;
    parser->section_length = 24;
    parser->section_total_length_offset = 12;
    parser->instance_length = 24;
    parser->instance_address_offset = 12;
    macosx_status->cfm_status.breakpoint_offset = 864;
  } else {
    warning ("unable to determine CFM version; disabling CFM support");
    parser->version = 0;
    return;
  }
	
  macosx_status->cfm_status.info_api_cookie = SYMBOL_VALUE_ADDRESS (hooksym);
  dyld_debug ("Found gPCFMInfoHooks in CarbonCore: 0x%lx with version %d\n",
	      SYMBOL_VALUE_ADDRESS (hooksym), parser->version);

  if (inferior_auto_start_cfm_flag)
    macosx_cfm_thread_create (&macosx_status->cfm_status, macosx_status->task);

#endif /* WITH_CFM */
}

void dyld_load_symfile (struct dyld_objfile_entry *e) 
{
  char *name = NULL;
  char *leaf = NULL;
  struct section_addr_info addrs;
  unsigned int i;

  if (e->loaded_error) { return; }

  CHECK_FATAL (e->allocated);

  CHECK_FATAL (e->abfd != NULL);
  
  if (e->reason & dyld_reason_executable_mask) {
    CHECK_FATAL (e->objfile == symfile_objfile);
  }

  name = dyld_entry_string (e, dyld_print_basenames_flag);

  leaf = strrchr (name, '/');
  leaf = ((leaf != NULL) ? leaf : name);

  if (e->dyld_valid) { 
    e->loaded_addr = e->dyld_addr;
    e->loaded_addrisoffset = 0;
  } else if (e->image_addr_valid) {
    e->loaded_addr = e->image_addr;
    e->loaded_addrisoffset = 0;
  } else {
    e->loaded_addr = e->dyld_slide;
    e->loaded_addrisoffset = 1;
  }

  for (i = 0; i < MAX_SECTIONS; i++) {
    addrs.other[i].name = NULL;
    addrs.other[i].addr = e->dyld_slide;
    addrs.other[i].sectindex = 0;
  }

  addrs.addrs_are_offsets = 1;

  if (e->objfile != NULL) {
    struct section_offsets *new_offsets = (struct section_offsets *) xmalloc (SIZEOF_SECTION_OFFSETS);
    tell_breakpoints_objfile_changed (e->objfile);
    for (i = 0; i < SECT_OFF_MAX; i++) {
      new_offsets->offsets[i] = addrs.other[0].addr;
    }
    if (info_verbose)
      printf_filtered ("Relocating symbols from %s...", e->objfile->name);
    gdb_flush (gdb_stdout);
#if MAPPED_SYMFILES
    mmalloc_protect (e->objfile->md, PROT_READ | PROT_WRITE);
#endif
    objfile_relocate (e->objfile, new_offsets);
#if MAPPED_SYMFILES
    mmalloc_protect (e->objfile->md, PROT_READ);
#endif
    if (info_verbose)
      printf_filtered ("done\n");
  } else {
    e->objfile = symbol_file_add_bfd_safe (e->abfd, 0, &addrs, 0, 0, e->load_flag, 0, e->prefix);
  }

  xfree (name);

  if (e->objfile != NULL) { 
    CHECK_FATAL (e->objfile->obfd != NULL);
  }

  if (e->objfile == NULL) {
    e->loaded_error = 1;
    e->abfd = NULL;
    xfree (name);
    return;
  }

  dyld_symfile_loaded_hook (e->objfile);

  if (e->reason & dyld_reason_executable_mask) {
    CHECK_FATAL ((symfile_objfile == NULL) || (symfile_objfile == e->objfile));
    symfile_objfile = e->objfile;
    return;
  }
}

void dyld_load_symfiles (struct dyld_objfile_info *result)
{
  unsigned int i;
  unsigned int first = 1;
  CHECK_FATAL (result != NULL);

  for (i = 0; i < result->nents; i++) {

    struct dyld_objfile_entry *e = &result->entries[i];
    char load_char;

    if (! e->allocated) { continue; }
    if (e->loaded_error) { continue; }
    if (e->abfd == NULL) { continue; }
    if (e->objfile != NULL) {
      if ((e->dyld_valid) && (e->loaded_addr == e->dyld_addr) && (! e->loaded_addrisoffset))
	continue;
      if ((e->dyld_valid) && (e->loaded_addr == e->dyld_slide) && (e->loaded_addrisoffset))
	continue;
      if ((e->image_addr_valid) && (e->loaded_addr == e->image_addr) && (! e->loaded_addrisoffset))
	continue;
      if ((! e->dyld_valid) && (! e->image_addr_valid))
	continue;
    }

    load_char = (e->objfile != NULL) ? '+' : '.';
    if (first && (! info_verbose) && dyld_print_status()) {
      first = 0;
      printf_filtered ("Reading symbols for shared libraries ");
      gdb_flush (gdb_stdout);
    }
    dyld_load_symfile (e);
    if ((! info_verbose) && dyld_print_status()) {
      printf_filtered ("%c", load_char);
      gdb_flush (gdb_stdout);
    }
  }

  if ((! first) && (! info_verbose) && dyld_print_status()) {
    printf_filtered (" done\n");
    gdb_flush (gdb_stdout);
  }
}

int dyld_objfile_allocated (struct objfile *o)
{
  struct objfile *objfile, *temp;

  ALL_OBJFILES_SAFE (objfile, temp) {
    if (o == objfile) {
      return 1;
    }
  }
  return 0;
}

void dyld_remove_objfile (struct dyld_objfile_entry *e)
{
  char *s = NULL;

  CHECK_FATAL (e->allocated);

  if (e->reason & dyld_reason_executable_mask)
    CHECK_FATAL (e->objfile == symfile_objfile);

  if (e->objfile == NULL) {
    return;
  }

  CHECK_FATAL (dyld_objfile_allocated (e->objfile));
  CHECK_FATAL (e->objfile->obfd != NULL);

  s = dyld_entry_string (e, dyld_print_basenames_flag);
  if (info_verbose) {
    printf_filtered ("Removing symbols for %s\n", s);
  }
  xfree (s);
  gdb_flush (gdb_stdout);
  free_objfile (e->objfile);
  e->objfile = NULL;
  e->abfd = NULL;
  e->loaded_name = NULL;
  e->loaded_memaddr = 0;
  gdb_flush (gdb_stdout);

  if (e->reason & dyld_reason_executable_mask) {
    symfile_objfile = e->objfile;
  }
}

void dyld_remove_objfiles (const struct dyld_path_info *d, struct dyld_objfile_info *result)
{
  unsigned int i;
  unsigned int first = 1;
  CHECK_FATAL (result != NULL);

  for (i = 0; i < result->nents; i++) {

    struct dyld_objfile_entry *e = &result->entries[i];
    int should_reload = 0;

    if (! e->allocated) { continue; }
    if (e->load_flag < 0) {
      e->load_flag = dyld_default_load_flag (d, e) | dyld_minimal_load_flag (d, e);
    }

    if (e->reason & dyld_reason_executable_mask)
      CHECK_FATAL (e->objfile == symfile_objfile);

    if (e->objfile != NULL)
      {
	if ((e->user_name != NULL) && (strcmp (e->user_name, e->objfile->name) != 0))
	  should_reload = 1;
	
	/* For cached symbol files, don't reload if the cached file
	   contains *more* symbols than the request being made. */
	if ((e->objfile->flags & OBJF_MAPPED) && (e->load_flag & ~e->objfile->symflags))
	  should_reload = 1;
	
	/* For regular symbol files, reload if there is any difference
	   in the requested symbols at all. */
	if ((! (e->objfile->flags & OBJF_MAPPED)) && (e->load_flag != e->objfile->symflags))
	  should_reload = 1;
      }

    if (should_reload) {
      dyld_remove_objfile (e);
      if (first && (! info_verbose) && dyld_print_status()) {
	first = 0;
	printf_filtered ("Removing symbols for unused shared libraries ");
	gdb_flush (gdb_stdout);
      }
      if ((! info_verbose) && dyld_print_status()) {
	printf_filtered (".");
	gdb_flush (gdb_stdout);
      }
    }
  }
  if ((! first) && (! info_verbose) && dyld_print_status()) {
    printf_filtered (" done\n");
    gdb_flush (gdb_stdout);
  }
}

static int dyld_libraries_similar
(struct dyld_objfile_entry *f, struct dyld_objfile_entry *l)
{
  const char *fname = NULL;
  const char *lname = NULL;

  const char *fbase = NULL;
  const char *lbase = NULL;
  unsigned int flen = 0;
  unsigned int llen = 0;

  CHECK_FATAL (f != NULL);
  CHECK_FATAL (l != NULL);

  if ((library_offset (f) != 0) && (library_offset (l) != 0)) {
    return (library_offset (f) == library_offset (l));
  }

  fname = dyld_entry_filename (f, NULL, DYLD_ENTRY_FILENAME_LOADED);
  lname = dyld_entry_filename (l, NULL, DYLD_ENTRY_FILENAME_LOADED);

  if ((lname != NULL) && (fname != NULL)) {

    int f_is_framework, f_is_bundle;
    int l_is_framework, l_is_bundle;

    dyld_library_basename (fname, &fbase, &flen, &f_is_framework, &f_is_bundle);
    dyld_library_basename (lname, &lbase, &llen, &l_is_framework, &l_is_bundle);

    if ((flen != llen) || (strncmp (fbase, lbase, llen) != 0))
      return 0;

    if (f_is_framework != l_is_framework)
      return 0;

    if (f_is_bundle != l_is_bundle)
      return 0;

    return 1;
  }
  
  return 0;
}

static int dyld_libraries_compatible
(struct dyld_path_info *d,
 struct dyld_objfile_entry *f, struct dyld_objfile_entry *l)
{
  const char *fname = NULL;
  const char *lname = NULL;

  CHECK_FATAL (f != NULL);
  CHECK_FATAL (l != NULL);

  if ((f->prefix != NULL && l->prefix != NULL)
      && ((f->prefix != l->prefix) 
	  || (strcmp (f->prefix, l->prefix) != 0))) {
    return 0;
  }

  fname = dyld_entry_filename (f, d, DYLD_ENTRY_FILENAME_LOADED);
  lname = dyld_entry_filename (l, d, DYLD_ENTRY_FILENAME_LOADED);

  if ((fname != NULL) && (lname != NULL)) {
    if (strcmp (fname, lname) != 0) {
      return 0;
    }
  }
  
  if (dyld_always_read_from_memory_flag) {
    if (f->loaded_from_memory != l->loaded_from_memory) {
      return 0;
    }
  }

  return 1;
}
	  
void dyld_objfile_move_load_data
(struct dyld_objfile_entry *f, struct dyld_objfile_entry *l)
{
  l->objfile = f->objfile;
  l->abfd = f->abfd;
  
  if (l->load_flag < 0) {
    l->load_flag = f->load_flag; 
  }

  l->prefix = f->prefix;
  l->loaded_name = f->loaded_name;
  l->loaded_memaddr = f->loaded_memaddr;
  l->loaded_addr = f->loaded_addr;
  l->loaded_offset = f->loaded_offset;
  l->loaded_addrisoffset = f->loaded_addrisoffset;
  l->loaded_from_memory = f->loaded_from_memory;
  l->loaded_error = f->loaded_error;

  f->objfile = NULL;
  f->abfd = NULL;
  
  f->load_flag = -1;

  f->loaded_name = NULL;
  f->loaded_memaddr = 0;
  f->loaded_addr = 0;
  f->loaded_offset = 0;
  f->loaded_addrisoffset = 0;
  f->loaded_from_memory = 0;
  f->loaded_error = 0;
}

void dyld_check_discarded (struct dyld_objfile_info *info)
{
  unsigned int j;
  for (j = 0; j < info->nents; j++) {
    struct dyld_objfile_entry *e = &info->entries[j];
    if ((e->abfd == NULL) && (e->objfile == NULL) && (! e->loaded_error)) {
      dyld_objfile_entry_clear (e);
    }
  }
}

/* Grab the shlib info for 'n' from 'old', if it exists there. */

void dyld_merge_shlib
(const struct macosx_dyld_thread_status *s,
 struct dyld_path_info *d,
 struct dyld_objfile_info *old, 
 struct dyld_objfile_entry *n)
{
  unsigned int i;
  struct dyld_objfile_entry *o;

  DYLD_ALL_OBJFILE_INFO_ENTRIES (old, o, i)
    if (dyld_libraries_compatible (d, n, o))
      {
	dyld_objfile_move_load_data (o, n);
	if (n->reason & dyld_reason_executable_mask)
	  symfile_objfile = n->objfile;
      }

  DYLD_ALL_OBJFILE_INFO_ENTRIES (old, o, i)
    if ((n->reason & dyld_reason_image_mask)
	&& dyld_libraries_similar (n, o)
	&& (o->objfile != NULL))
      {
	dyld_objfile_move_load_data (o, n);
	if (n->reason & dyld_reason_executable_mask)
	  symfile_objfile = n->objfile;
      }
}

void dyld_prune_shlib
(const struct macosx_dyld_thread_status *s,
 struct dyld_path_info *d,
 struct dyld_objfile_info *old, 
 struct dyld_objfile_entry *n)
{
  struct dyld_objfile_entry *o = NULL;
  unsigned int i;

  DYLD_ALL_OBJFILE_INFO_ENTRIES (old, o, i)
    {
      if ((o->reason & dyld_reason_executable_mask)
	  && (n->reason & dyld_reason_executable_mask))
	{
	  if (o->objfile != NULL)
	    tell_breakpoints_objfile_changed (o->objfile);
	  dyld_objfile_entry_clear (o);
	  continue;
	}

      if (dyld_libraries_similar (o, n))
	{
	  if (o->objfile != NULL)
	    tell_breakpoints_objfile_changed (o->objfile);
	  dyld_remove_objfile (o);
	  dyld_objfile_entry_clear (o);
	}
    }
}

void dyld_merge_shlibs
(const struct macosx_dyld_thread_status *s,
 struct dyld_path_info *d,
 struct dyld_objfile_info *old, 
 struct dyld_objfile_info *new)
{
  struct dyld_objfile_entry *n = NULL;
  struct dyld_objfile_entry *o = NULL;
  unsigned int i;

  CHECK_FATAL (old != NULL);
  CHECK_FATAL (new != NULL);
  CHECK_FATAL (old != new);

  dyld_resolve_filenames (s, new);

  DYLD_ALL_OBJFILE_INFO_ENTRIES (new, n, i)
    dyld_merge_shlib (s, d, old, n);

  DYLD_ALL_OBJFILE_INFO_ENTRIES (new, n, i)
    dyld_prune_shlib (s, d, old, n);

  DYLD_ALL_OBJFILE_INFO_ENTRIES (old, o, i)
    {
      struct dyld_objfile_entry *e = NULL;
      e = dyld_objfile_entry_alloc (new);
      *e = *o;

      e->reason |= dyld_reason_cached_mask;

      dyld_objfile_entry_clear (o);
    }
}

static void dyld_shlibs_updated (struct dyld_objfile_info *info)
{
  dyld_objfile_info_pack (info);
  update_section_tables (&current_target, info);
  reread_symbols ();
  breakpoint_update ();
  re_enable_breakpoints_in_shlibs (0);
}

void dyld_update_shlibs
(const struct macosx_dyld_thread_status *s,
 struct dyld_path_info *d,
 struct dyld_objfile_info *result)
{
  CHECK_FATAL (result != NULL);

  dyld_debug ("dyld_update_shlibs: updating shared library information\n");

  dyld_remove_objfiles (d, result);
  dyld_load_libraries (d, result);
  dyld_load_symfiles (result);

  dyld_shlibs_updated (result);
}

void dyld_purge_cached_libraries (struct dyld_objfile_info *info)
{
  unsigned int i;
  CHECK_FATAL (info != NULL);

  for (i = 0; i < info->nents; i++) {
    struct dyld_objfile_entry *e = &info->entries[i];
    if (! e->allocated) { continue; }
    if (e->reason & dyld_reason_cached_mask) {
      dyld_remove_objfile (e);
      dyld_objfile_entry_clear (e);
    }
  }

  dyld_shlibs_updated (info);
}

void
_initialize_macosx_nat_dyld_process ()
{
}
