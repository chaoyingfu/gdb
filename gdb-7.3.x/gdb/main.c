/* Top level stuff for GDB, the GNU debugger.

   Copyright (C) 1986, 1987, 1988, 1989, 1990, 1991, 1992, 1993, 1994, 1995,
   1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2007, 2008,
   2009, 2010, 2011 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "top.h"
#include "target.h"
#include "inferior.h"
#include "symfile.h"
#include "gdbcore.h"

#include "exceptions.h"
#include "getopt.h"

#include <sys/types.h>
#include "gdb_stat.h"
#include <ctype.h>

#include "gdb_string.h"
#include "event-loop.h"
#include "ui-out.h"
#include "usage-logging.h"

#include "interps.h"
#include "main.h"
#include "source.h"
#include "cli/cli-cmds.h"
#include "python/python.h"
#include "objfiles.h"

#ifdef __MINGW32__
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* The selected interpreter.  This will be used as a set command
   variable, so it should always be malloc'ed - since
   do_setshow_command will free it.  */
char *interpreter_p;

/* Whether xdb commands will be handled.  */
int xdb_commands = 0;

/* Whether dbx commands will be handled.  */
int dbx_commands = 0;

/* System root path, used to find libraries etc.  */
char *gdb_sysroot = 0;

/* GDB datadir, used to store data files.  */
char *gdb_datadir = 0;

/* If gdb was configured with --with-python=/path,
   the possibly relocated path to python's lib directory.  */
char *python_libdir = 0;

struct ui_file *gdb_stdout;
struct ui_file *gdb_stderr;
struct ui_file *gdb_stdlog;
struct ui_file *gdb_stdin;
/* Target IO streams.  */
struct ui_file *gdb_stdtargin;
struct ui_file *gdb_stdtarg;
struct ui_file *gdb_stdtargerr;

/* True if --batch or --batch-silent was seen.  */
int batch_flag = 0;

/* Support for the --batch-silent option.  */
int batch_silent = 0;

/* Support for --return-child-result option.
   Set the default to -1 to return error in the case
   that the program does not run or does not complete.  */
int return_child_result = 0;
int return_child_result_value = -1;

/* Whether to enable writing into executable and core files.  */
extern int write_files;

/* GDB as it has been invoked from the command line (i.e. argv[0]).  */
char *gdb_program_name;

static void print_gdb_help (struct ui_file *);

/* These two are used to set the external editor commands when gdb is
   farming out files to be edited by another program.  */

extern char *external_editor_command;

/* Relocate a file or directory.  PROGNAME is the name by which gdb
   was invoked (i.e., argv[0]).  INITIAL is the default value for the
   file or directory.  FLAG is true if the value is relocatable, false
   otherwise.  Returns a newly allocated string; this may return NULL
   under the same conditions as make_relative_prefix.  */

static char *
relocate_path (const char *progname, const char *initial, int flag)
{
  if (flag)
    return make_relative_prefix (progname, BINDIR, initial);
  return xstrdup (initial);
}

/* Like relocate_path, but specifically checks for a directory.
   INITIAL is relocated according to the rules of relocate_path.  If
   the result is a directory, it is used; otherwise, INITIAL is used.
   The chosen directory is then canonicalized using lrealpath.  This
   function always returns a newly-allocated string.  */

char *
relocate_gdb_directory (const char *initial, int flag)
{
  char *dir;

  dir = relocate_path (gdb_program_name, initial, flag);
  if (dir)
    {
      struct stat s;

      if (stat (dir, &s) != 0 || !S_ISDIR (s.st_mode))
	{
	  xfree (dir);
	  dir = NULL;
	}
    }
  if (!dir)
    dir = xstrdup (initial);

  /* Canonicalize the directory.  */
  if (*dir)
    {
      char *canon_sysroot = lrealpath (dir);

      if (canon_sysroot)
	{
	  xfree (dir);
	  dir = canon_sysroot;
	}
    }

  return dir;
}

/* Compute the locations of init files that GDB should source and
   return them in SYSTEM_GDBINIT, HOME_GDBINIT, LOCAL_GDBINIT.  If
   there is no system gdbinit (resp. home gdbinit and local gdbinit)
   to be loaded, then SYSTEM_GDBINIT (resp. HOME_GDBINIT and
   LOCAL_GDBINIT) is set to NULL.  */
static void
get_init_files (char **system_gdbinit,
		char **home_gdbinit,
		char **local_gdbinit)
{
  static char *sysgdbinit = NULL;
  static char *homeinit = NULL;
  static char *localinit = NULL;
  static int initialized = 0;

  if (!initialized)
    {
      struct stat homebuf, cwdbuf, s;
      char *homedir, *relocated_sysgdbinit;

      if (SYSTEM_GDBINIT[0])
	{
	  relocated_sysgdbinit = relocate_path (gdb_program_name,
						SYSTEM_GDBINIT,
						SYSTEM_GDBINIT_RELOCATABLE);
	  if (relocated_sysgdbinit && stat (relocated_sysgdbinit, &s) == 0)
	    sysgdbinit = relocated_sysgdbinit;
	  else
	    xfree (relocated_sysgdbinit);
	}

      homedir = getenv ("HOME");

      /* If the .gdbinit file in the current directory is the same as
	 the $HOME/.gdbinit file, it should not be sourced.  homebuf
	 and cwdbuf are used in that purpose.  Make sure that the stats
	 are zero in case one of them fails (this guarantees that they
	 won't match if either exists).  */

      memset (&homebuf, 0, sizeof (struct stat));
      memset (&cwdbuf, 0, sizeof (struct stat));

      if (homedir)
	{
	  homeinit = xstrprintf ("%s/%s", homedir, gdbinit);
	  if (stat (homeinit, &homebuf) != 0)
	    {
	      xfree (homeinit);
	      homeinit = NULL;
	    }
	}

      if (stat (gdbinit, &cwdbuf) == 0)
	{
	  if (!homeinit
	      || memcmp ((char *) &homebuf, (char *) &cwdbuf,
			 sizeof (struct stat)))
	    localinit = gdbinit;
	}
      
      initialized = 1;
    }

  *system_gdbinit = sysgdbinit;
  *home_gdbinit = homeinit;
  *local_gdbinit = localinit;
}

/* Call command_loop.  If it happens to return, pass that through as a
   non-zero return status.  */

static int
captured_command_loop (void *data)
{
  current_interp_command_loop ();
  /* FIXME: cagney/1999-11-05: A correct command_loop() implementaton
     would clean things up (restoring the cleanup chain) to the state
     they were just prior to the call.  Technically, this means that
     the do_cleanups() below is redundant.  Unfortunately, many FUNCs
     are not that well behaved.  do_cleanups should either be replaced
     with a do_cleanups call (to cover the problem) or an assertion
     check to detect bad FUNCs code.  */
  do_cleanups (ALL_CLEANUPS);
  /* If the command_loop returned, normally (rather than threw an
     error) we try to quit.  If the quit is aborted, catch_errors()
     which called this catch the signal and restart the command
     loop.  */
  quit_command (NULL, instream == stdin);
  return 1;
}

/* Bug 1155829.
   Subroutine of captured_main to keep the local mod separate.
   Warn about troublesome environment variables.  */

static void
warn_troublesome_environment ()
{
  const char *ld_library_path;

  if (getenv ("LD_ASSUME_KERNEL") != NULL)
    {
      printf_filtered ("\n");
      printf_filtered (_("WARNING: GDB has detected that you have set the\n"
                         "LD_ASSUME_KERNEL environment variable.\n"));
      printf_filtered (_("You may have intended to do this, but you should know\n"
                         "that this can often cause problems while debugging.\n"));
      printf_filtered (_("For more information see http://wiki/Main/GdbFaq#LD_ASSUME_KERNEL_usage_warning\n"));
      printf_filtered ("\n");
    }

  ld_library_path = getenv ("LD_LIBRARY_PATH");
  if (ld_library_path != NULL
      && strstr (ld_library_path, "/usr/lib/debug") != NULL)
    {
      printf_filtered ("\n");
      printf_filtered (_("WARNING: GDB has detected that your LD_LIBRARY_PATH environment variable\n"
                         "includes /usr/lib/debug. You may have intended this, but if your program\n"
                         "is using a different libc than /usr/lib/libc.so, it is the wrong thing to do.\n"
                         "For example Crosstool V11 and higher use the libc from GRTE.\n"));
      printf_filtered (_("For more information see http://wiki/Main/GdbFaq#LD_LIBRARY_PATH_usage_warning\n"));
      printf_filtered ("\n");
    }
}

char* get_program_name(char* argv0)
{
  char new_argv0[1024] = {'\0'};
#if defined(__linux__) || defined(__CYGWIN__)
  size_t len;
  if ((len = readlink("/proc/self/exe", new_argv0, sizeof(new_argv0)-1)) != -1)
    {
      new_argv0[len] = '\0';
    }
#elif defined(__APPLE__)
  uint32_t len;
  len=(uint32_t)sizeof(new_argv0);
  _NSGetExecutablePath(new_argv0, &len);
#else
  unsigned long bufsize=sizeof(new_argv0);
  if (GetModuleFileName(NULL, new_argv0, bufsize) != 0)
    {
      /* Early conversion to unix slashes instead of more changes
       * everywhere else... */
      char *winslash = strchr(new_argv0,'\\');
      while (winslash)
        {
          *winslash = '/';
          winslash = strchr(winslash,'\\');
        }
    }
#endif
  if (!strlen(new_argv0))
    {
       return xstrdup(argv0);
    }
  else
    {
      return xstrdup(new_argv0);
    }
}

static int
captured_main (void *data)
{
  struct captured_main_args *context = data;
  int argc = context->argc;
  char **argv = context->argv;
  static int quiet = 0;
  static int set_args = 0;

  /* Pointers to various arguments from command line.  */
  char *symarg = NULL;
  char *execarg = NULL;
  char *pidarg = NULL;
  char *corearg = NULL;
  char *pid_or_core_arg = NULL;
  char *cdarg = NULL;
  char *ttyarg = NULL;

  /* These are static so that we can take their address in an
     initializer.  */
  static int print_help;
  static int print_version;

  /* Pointers to all arguments of --command option.  */
  struct cmdarg {
    enum {
      CMDARG_FILE,
      CMDARG_COMMAND
    } type;
    char *string;
  } *cmdarg;
  /* Allocated size of cmdarg.  */
  int cmdsize;
  /* Number of elements of cmdarg used.  */
  int ncmd;

  /* Indices of all arguments of --directory option.  */
  char **dirarg;
  /* Allocated size.  */
  int dirsize;
  /* Number of elements used.  */
  int ndir;

  /* gdb init files.  */
  char *system_gdbinit;
  char *home_gdbinit;
  char *local_gdbinit;

  int i;
  int save_auto_load;
  struct objfile *objfile;

  struct cleanup *pre_stat_chain;

#ifdef HAVE_SBRK
  /* Set this before calling make_command_stats_cleanup.  */
  lim_at_start = (char *) sbrk (0);
#endif

  pre_stat_chain = make_command_stats_cleanup (0, "init");

#if defined (HAVE_SETLOCALE) && defined (HAVE_LC_MESSAGES)
  setlocale (LC_MESSAGES, "");
#endif
#if defined (HAVE_SETLOCALE)
  setlocale (LC_CTYPE, "");
#endif
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  cmdsize = 1;
  cmdarg = (struct cmdarg *) xmalloc (cmdsize * sizeof (*cmdarg));
  ncmd = 0;
  dirsize = 1;
  dirarg = (char **) xmalloc (dirsize * sizeof (*dirarg));
  ndir = 0;

  quit_flag = 0;
  line = (char *) xmalloc (linesize);
  line[0] = '\0';		/* Terminate saved (now empty) cmd line.  */
  instream = stdin;

  gdb_stdout = stdio_fileopen (stdout);
  gdb_stderr = stdio_fileopen (stderr);
  gdb_stdlog = gdb_stderr;	/* for moment */
  gdb_stdtarg = gdb_stderr;	/* for moment */
  gdb_stdin = stdio_fileopen (stdin);
  gdb_stdtargerr = gdb_stderr;	/* for moment */
  gdb_stdtargin = gdb_stdin;	/* for moment */

  gdb_program_name = get_program_name (argv[0]);

  if (! getcwd (gdb_dirbuf, sizeof (gdb_dirbuf)))
    /* Don't use *_filtered or warning() (which relies on
       current_target) until after initialize_all_files().  */
    fprintf_unfiltered (gdb_stderr,
			_("%s: warning: error finding "
			  "working directory: %s\n"),
                        argv[0], safe_strerror (errno));
    
  current_directory = gdb_dirbuf;

  /* Set the sysroot path.  */
  gdb_sysroot = relocate_gdb_directory (TARGET_SYSTEM_ROOT,
					TARGET_SYSTEM_ROOT_RELOCATABLE);

  debug_file_directory = relocate_gdb_directory (DEBUGDIR,
						 DEBUGDIR_RELOCATABLE);

  gdb_datadir = relocate_gdb_directory (GDB_DATADIR,
					GDB_DATADIR_RELOCATABLE);

#ifdef WITH_PYTHON_PATH
  {
    /* For later use in helping Python find itself.  */
    char *tmp = concat (WITH_PYTHON_PATH, SLASH_STRING, "lib", NULL);

    python_libdir = relocate_gdb_directory (tmp,
					    PYTHON_PATH_RELOCATABLE);
    xfree (tmp);
  }
#endif

#ifdef RELOC_SRCDIR
  add_substitute_path_rule (RELOC_SRCDIR,
			    make_relative_prefix (argv[0], BINDIR,
						  RELOC_SRCDIR));
#endif

  /* There will always be an interpreter.  Either the one passed into
     this captured main, or one specified by the user at start up, or
     the console.  Initialize the interpreter to the one requested by 
     the application.  */
  interpreter_p = xstrdup (context->interpreter_p);

  /* Parse arguments and options.  */
  {
    int c;
    /* When var field is 0, use flag field to record the equivalent
       short option (or arbitrary numbers starting at 10 for those
       with no equivalent).  */
    enum {
      OPT_SE = 10,
      OPT_CD,
      OPT_ANNOTATE,
      OPT_STATISTICS,
      OPT_TUI,
      OPT_NOWINDOWS,
      OPT_WINDOWS
    };
    extern int dwarf2_use_gdb_index; /* GOOGLE LOCAL */
    static struct option long_options[] =
    {
      {"tui", no_argument, 0, OPT_TUI},
      {"xdb", no_argument, &xdb_commands, 1},
      {"dbx", no_argument, &dbx_commands, 1},
      {"readnow", no_argument, &readnow_symbol_files, 1},
      {"r", no_argument, &readnow_symbol_files, 1},
      {"quiet", no_argument, &quiet, 1},
      {"q", no_argument, &quiet, 1},
      {"silent", no_argument, &quiet, 1},
      {"nx", no_argument, &inhibit_gdbinit, 1},
      {"n", no_argument, &inhibit_gdbinit, 1},
      {"batch-silent", no_argument, 0, 'B'},
      {"batch", no_argument, &batch_flag, 1},
      {"epoch", no_argument, &epoch_interface, 1},

      /* GOOGLE LOCAL */
      {"disable-gdb-index", no_argument, &dwarf2_use_gdb_index, 0},

    /* This is a synonym for "--annotate=1".  --annotate is now
       preferred, but keep this here for a long time because people
       will be running emacses which use --fullname.  */
      {"fullname", no_argument, 0, 'f'},
      {"f", no_argument, 0, 'f'},

      {"annotate", required_argument, 0, OPT_ANNOTATE},
      {"help", no_argument, &print_help, 1},
      {"se", required_argument, 0, OPT_SE},
      {"symbols", required_argument, 0, 's'},
      {"s", required_argument, 0, 's'},
      {"exec", required_argument, 0, 'e'},
      {"e", required_argument, 0, 'e'},
      {"core", required_argument, 0, 'c'},
      {"c", required_argument, 0, 'c'},
      {"pid", required_argument, 0, 'p'},
      {"p", required_argument, 0, 'p'},
      {"command", required_argument, 0, 'x'},
      {"eval-command", required_argument, 0, 'X'},
      {"version", no_argument, &print_version, 1},
      {"x", required_argument, 0, 'x'},
      {"ex", required_argument, 0, 'X'},
#ifdef GDBTK
      {"tclcommand", required_argument, 0, 'z'},
      {"enable-external-editor", no_argument, 0, 'y'},
      {"editor-command", required_argument, 0, 'w'},
#endif
      {"ui", required_argument, 0, 'i'},
      {"interpreter", required_argument, 0, 'i'},
      {"i", required_argument, 0, 'i'},
      {"directory", required_argument, 0, 'd'},
      {"d", required_argument, 0, 'd'},
      {"data-directory", required_argument, 0, 'D'},
      {"cd", required_argument, 0, OPT_CD},
      {"tty", required_argument, 0, 't'},
      {"baud", required_argument, 0, 'b'},
      {"b", required_argument, 0, 'b'},
      {"nw", no_argument, NULL, OPT_NOWINDOWS},
      {"nowindows", no_argument, NULL, OPT_NOWINDOWS},
      {"w", no_argument, NULL, OPT_WINDOWS},
      {"windows", no_argument, NULL, OPT_WINDOWS},
      {"statistics", no_argument, 0, OPT_STATISTICS},
      {"write", no_argument, &write_files, 1},
      {"args", no_argument, &set_args, 1},
      {"l", required_argument, 0, 'l'},
      {"return-child-result", no_argument, &return_child_result, 1},
      {0, no_argument, 0, 0}
    };

    while (1)
      {
	int option_index;

	c = getopt_long_only (argc, argv, "",
			      long_options, &option_index);
	if (c == EOF || set_args)
	  break;

	/* Long option that takes an argument.  */
	if (c == 0 && long_options[option_index].flag == 0)
	  c = long_options[option_index].val;

	switch (c)
	  {
	  case 0:
	    /* Long option that just sets a flag.  */
	    break;
	  case OPT_SE:
	    symarg = optarg;
	    execarg = optarg;
	    break;
	  case OPT_CD:
	    cdarg = optarg;
	    break;
	  case OPT_ANNOTATE:
	    /* FIXME: what if the syntax is wrong (e.g. not digits)?  */
	    annotation_level = atoi (optarg);
	    break;
	  case OPT_STATISTICS:
	    /* Enable the display of both time and space usage.  */
	    set_display_time (1);
	    set_display_space (1);
	    break;
	  case OPT_TUI:
	    /* --tui is equivalent to -i=tui.  */
#ifdef TUI
	    xfree (interpreter_p);
	    interpreter_p = xstrdup (INTERP_TUI);
#else
	    fprintf_unfiltered (gdb_stderr,
				_("%s: TUI mode is not supported\n"),
				argv[0]);
	    exit (1);
#endif
	    break;
	  case OPT_WINDOWS:
	    /* FIXME: cagney/2003-03-01: Not sure if this option is
               actually useful, and if it is, what it should do.  */
#ifdef GDBTK
	    /* --windows is equivalent to -i=insight.  */
	    xfree (interpreter_p);
	    interpreter_p = xstrdup (INTERP_INSIGHT);
#endif
	    use_windows = 1;
	    break;
	  case OPT_NOWINDOWS:
	    /* -nw is equivalent to -i=console.  */
	    xfree (interpreter_p);
	    interpreter_p = xstrdup (INTERP_CONSOLE);
	    use_windows = 0;
	    break;
	  case 'f':
	    annotation_level = 1;
	    /* We have probably been invoked from emacs.  Disable
	       window interface.  */
	    use_windows = 0;
	    break;
	  case 's':
	    symarg = optarg;
	    break;
	  case 'e':
	    execarg = optarg;
	    break;
	  case 'c':
	    corearg = optarg;
	    break;
	  case 'p':
	    pidarg = optarg;
	    break;
	  case 'x':
	    cmdarg[ncmd].type = CMDARG_FILE;
	    cmdarg[ncmd++].string = optarg;
	    if (ncmd >= cmdsize)
	      {
		cmdsize *= 2;
		cmdarg = xrealloc ((char *) cmdarg,
				   cmdsize * sizeof (*cmdarg));
	      }
	    break;
	  case 'X':
	    cmdarg[ncmd].type = CMDARG_COMMAND;
	    cmdarg[ncmd++].string = optarg;
	    if (ncmd >= cmdsize)
	      {
		cmdsize *= 2;
		cmdarg = xrealloc ((char *) cmdarg,
				   cmdsize * sizeof (*cmdarg));
	      }
	    break;
	  case 'B':
	    batch_flag = batch_silent = 1;
	    gdb_stdout = ui_file_new();
	    break;
	  case 'D':
	    xfree (gdb_datadir);
	    gdb_datadir = xstrdup (optarg);
	    break;
#ifdef GDBTK
	  case 'z':
	    {
	      extern int gdbtk_test (char *);

	      if (!gdbtk_test (optarg))
		{
		  fprintf_unfiltered (gdb_stderr,
				      _("%s: unable to load "
					"tclcommand file \"%s\""),
				      argv[0], optarg);
		  exit (1);
		}
	      break;
	    }
	  case 'y':
	    /* Backwards compatibility only.  */
	    break;
	  case 'w':
	    {
	      external_editor_command = xstrdup (optarg);
	      break;
	    }
#endif /* GDBTK */
	  case 'i':
	    xfree (interpreter_p);
	    interpreter_p = xstrdup (optarg);
	    break;
	  case 'd':
	    dirarg[ndir++] = optarg;
	    if (ndir >= dirsize)
	      {
		dirsize *= 2;
		dirarg = (char **) xrealloc ((char *) dirarg,
					     dirsize * sizeof (*dirarg));
	      }
	    break;
	  case 't':
	    ttyarg = optarg;
	    break;
	  case 'q':
	    quiet = 1;
	    break;
	  case 'b':
	    {
	      int i;
	      char *p;

	      i = strtol (optarg, &p, 0);
	      if (i == 0 && p == optarg)

		/* Don't use *_filtered or warning() (which relies on
		   current_target) until after initialize_all_files().  */

		fprintf_unfiltered
		  (gdb_stderr,
		   _("warning: could not set baud rate to `%s'.\n"), optarg);
	      else
		baud_rate = i;
	    }
            break;
	  case 'l':
	    {
	      int i;
	      char *p;

	      i = strtol (optarg, &p, 0);
	      if (i == 0 && p == optarg)

		/* Don't use *_filtered or warning() (which relies on
		   current_target) until after initialize_all_files().  */

		fprintf_unfiltered (gdb_stderr,
				    _("warning: could not set "
				      "timeout limit to `%s'.\n"), optarg);
	      else
		remote_timeout = i;
	    }
	    break;

	  case '?':
	    fprintf_unfiltered (gdb_stderr,
				_("Use `%s --help' for a "
				  "complete list of options.\n"),
				argv[0]);
	    exit (1);
	  }
      }

    /* If --help or --version, disable window interface.  */
    if (print_help || print_version)
      {
	use_windows = 0;
      }

    if (batch_flag)
      quiet = 1;
  }

  /* Start of logging is deferred until here in case we want to add a
     command-line option to disable logging.
     ??? Doing it here instead of earlier does mean we miss some exits,
     e.g. bad option passed.  Is this important?  */
  usage_log_start (argc, argv);

  /* Initialize all files.  Give the interpreter a chance to take
     control of the console via the deprecated_init_ui_hook ().  */
  gdb_init (argv[0]);

  /* Now that gdb_init has created the initial inferior, we're in
     position to set args for that inferior.  */
  if (set_args)
    {
      /* The remaining options are the command-line options for the
	 inferior.  The first one is the sym/exec file, and the rest
	 are arguments.  */
      if (optind >= argc)
	{
	  fprintf_unfiltered (gdb_stderr,
			      _("%s: `--args' specified but "
				"no program specified\n"),
			      argv[0]);
	  exit (1);
	}
      symarg = argv[optind];
      execarg = argv[optind];
      ++optind;
      set_inferior_args_vector (argc - optind, &argv[optind]);
    }
  else
    {
      /* OK, that's all the options.  */

      /* The first argument, if specified, is the name of the
	 executable.  */
      if (optind < argc)
	{
	  symarg = argv[optind];
	  execarg = argv[optind];
	  optind++;
	}

      /* If the user hasn't already specified a PID or the name of a
	 core file, then a second optional argument is allowed.  If
	 present, this argument should be interpreted as either a
	 PID or a core file, whichever works.  */
      if (pidarg == NULL && corearg == NULL && optind < argc)
	{
	  pid_or_core_arg = argv[optind];
	  optind++;
	}

      /* Any argument left on the command line is unexpected and
	 will be ignored.  Inform the user.  */
      if (optind < argc)
	fprintf_unfiltered (gdb_stderr,
			    _("Excess command line "
			      "arguments ignored. (%s%s)\n"),
			    argv[optind],
			    (optind == argc - 1) ? "" : " ...");
    }

  /* Lookup gdbinit files.  Note that the gdbinit file name may be
     overriden during file initialization, so get_init_files should be
     called after gdb_init.  */
  get_init_files (&system_gdbinit, &home_gdbinit, &local_gdbinit);

  /* Do these (and anything which might call wrap_here or *_filtered)
     after initialize_all_files() but before the interpreter has been
     installed.  Otherwize the help/version messages will be eaten by
     the interpreter's output handler.  */

  if (print_version)
    {
      print_gdb_version (gdb_stdout);
      wrap_here ("");
      printf_filtered ("\n");
      exit (0);
    }

  if (print_help)
    {
      print_gdb_help (gdb_stdout);
      fputs_unfiltered ("\n", gdb_stdout);
      exit (0);
    }

  /* FIXME: cagney/2003-02-03: The big hack (part 1 of 2) that lets
     GDB retain the old MI1 interpreter startup behavior.  Output the
     copyright message before the interpreter is installed.  That way
     it isn't encapsulated in MI output.  */
  if (!quiet && strcmp (interpreter_p, INTERP_MI1) == 0)
    {
      /* Print all the junk at the top, with trailing "..." if we are
         about to read a symbol file (possibly slowly).  */
      print_gdb_version (gdb_stdout);
#ifndef HAVE_GOOGLEISMS /* GOOGLE LOCAL: this is just noise */
      if (symarg)
	printf_filtered ("..");
#endif
      wrap_here ("");
      printf_filtered ("\n");
      gdb_flush (gdb_stdout);	/* Force to screen during slow
				   operations.  */
    }

  /* Install the default UI.  All the interpreters should have had a
     look at things by now.  Initialize the default interpreter.  */

  {
    /* Find it.  */
    struct interp *interp = interp_lookup (interpreter_p);

    if (interp == NULL)
      error (_("Interpreter `%s' unrecognized"), interpreter_p);
    /* Install it.  */
    if (!interp_set (interp, 1))
      {
        fprintf_unfiltered (gdb_stderr,
			    "Interpreter `%s' failed to initialize.\n",
                            interpreter_p);
        exit (1);
      }
  }

  /* FIXME: cagney/2003-02-03: The big hack (part 2 of 2) that lets
     GDB retain the old MI1 interpreter startup behavior.  Output the
     copyright message after the interpreter is installed when it is
     any sane interpreter.  */
  if (!quiet && !current_interp_named_p (INTERP_MI1))
    {
      /* Print all the junk at the top, with trailing "..." if we are
         about to read a symbol file (possibly slowly).  */
      print_gdb_version (gdb_stdout);
#ifndef HAVE_GOOGLEISMS /* GOOGLE LOCAL: this is just noise */
      if (symarg)
	printf_filtered ("..");
#endif
      wrap_here ("");
      printf_filtered ("\n");
      gdb_flush (gdb_stdout);	/* Force to screen during slow
				   operations.  */
    }

  /* Set off error and warning messages with a blank line.  */
  error_pre_print = "\n";
  quit_pre_print = error_pre_print;
  warning_pre_print = _("\nwarning: ");

  /* Read and execute the system-wide gdbinit file, if it exists.
     This is done *before* all the command line arguments are
     processed; it sets global parameters, which are independent of
     what file you are debugging or what directory you are in.  */
  if (system_gdbinit && !inhibit_gdbinit)
    catch_command_errors (source_script, system_gdbinit, 0, RETURN_MASK_ALL);

  /* Read and execute $HOME/.gdbinit file, if it exists.  This is done
     *before* all the command line arguments are processed; it sets
     global parameters, which are independent of what file you are
     debugging or what directory you are in.  */

  if (home_gdbinit && !inhibit_gdbinit)
    catch_command_errors (source_script, home_gdbinit, 0, RETURN_MASK_ALL);

  /* Now perform all the actions indicated by the arguments.  */
  if (cdarg != NULL)
    {
      catch_command_errors (cd_command, cdarg, 0, RETURN_MASK_ALL);
    }

  /* Bug 1155829.
     Warn about troublesome environment variables.  */
  if (!quiet)
    warn_troublesome_environment ();

  for (i = 0; i < ndir; i++)
    catch_command_errors (directory_switch, dirarg[i], 0, RETURN_MASK_ALL);
  xfree (dirarg);

  /* Skip auto-loading section-specified scripts until we've sourced
     local_gdbinit (which is often used to augment the source search
     path).  */
  save_auto_load = gdbpy_global_auto_load;
  gdbpy_global_auto_load = 0;

  if (execarg != NULL
      && symarg != NULL
      && strcmp (execarg, symarg) == 0)
    {
      /* The exec file and the symbol-file are the same.  If we can't
         open it, better only print one error message.
         catch_command_errors returns non-zero on success!  */
      if (catch_command_errors (exec_file_attach, execarg,
				!batch_flag, RETURN_MASK_ALL))
	catch_command_errors (symbol_file_add_main, symarg,
			      !batch_flag, RETURN_MASK_ALL);
    }
  else
    {
      if (execarg != NULL)
	catch_command_errors (exec_file_attach, execarg,
			      !batch_flag, RETURN_MASK_ALL);
      if (symarg != NULL)
	catch_command_errors (symbol_file_add_main, symarg,
			      !batch_flag, RETURN_MASK_ALL);
    }

  if (corearg && pidarg)
    error (_("Can't attach to process and specify "
	     "a core file at the same time."));

  if (corearg != NULL)
    catch_command_errors (core_file_command, corearg,
			  !batch_flag, RETURN_MASK_ALL);
  else if (pidarg != NULL)
    catch_command_errors (attach_command, pidarg,
			  !batch_flag, RETURN_MASK_ALL);
  else if (pid_or_core_arg)
    {
      /* The user specified 'gdb program pid' or gdb program core'.
	 If pid_or_core_arg's first character is a digit, try attach
	 first and then corefile.  Otherwise try just corefile.  */

      if (isdigit (pid_or_core_arg[0]))
	{
	  if (catch_command_errors (attach_command, pid_or_core_arg,
				    !batch_flag, RETURN_MASK_ALL) == 0)
	    catch_command_errors (core_file_command, pid_or_core_arg,
				  !batch_flag, RETURN_MASK_ALL);
	}
      else /* Can't be a pid, better be a corefile.  */
	catch_command_errors (core_file_command, pid_or_core_arg,
			      !batch_flag, RETURN_MASK_ALL);
    }

  if (ttyarg != NULL)
    set_inferior_io_terminal (ttyarg);

  /* Error messages should no longer be distinguished with extra output.  */
  error_pre_print = NULL;
  quit_pre_print = NULL;
  warning_pre_print = _("warning: ");

  /* Read the .gdbinit file in the current directory, *if* it isn't
     the same as the $HOME/.gdbinit file (it should exist, also).  */
  if (local_gdbinit && !inhibit_gdbinit)
    catch_command_errors (source_script, local_gdbinit, 0, RETURN_MASK_ALL);

  /* Now that all .gdbinit's have been read and all -d options have been
     processed, we can read any scripts mentioned in SYMARG.
     We wait until now because it is common to add to the source search
     path in local_gdbinit.  */
  gdbpy_global_auto_load = save_auto_load;
  ALL_OBJFILES (objfile)
    load_auto_scripts_for_objfile (objfile);

  for (i = 0; i < ncmd; i++)
    {
      if (cmdarg[i].type == CMDARG_FILE)
        catch_command_errors (source_script, cmdarg[i].string,
			      !batch_flag, RETURN_MASK_ALL);
      else  /* cmdarg[i].type == CMDARG_COMMAND */
        catch_command_errors (execute_command, cmdarg[i].string,
			      !batch_flag, RETURN_MASK_ALL);
    }
  xfree (cmdarg);

  /* Read in the old history after all the command files have been
     read.  */
  init_history ();

  if (batch_flag)
    {
      /* We have hit the end of the batch file.  */
      quit_force (NULL, 0);
    }

  /* Show time and/or space usage.  */
  do_cleanups (pre_stat_chain);
  /* Ensure the usage log timestamp is accurate.  */
  usage_log_flush ();

  /* NOTE: cagney/1999-11-07: There is probably no reason for not
     moving this loop and the code found in captured_command_loop()
     into the command_loop() proper.  The main thing holding back that
     change - SET_TOP_LEVEL() - has been eliminated.  */
  while (1)
    {
      catch_errors (captured_command_loop, 0, "", RETURN_MASK_ALL);
    }
  /* No exit -- exit is through quit_command.  */
}

int
gdb_main (struct captured_main_args *args)
{
  use_windows = args->use_windows;
  catch_errors (captured_main, args, "", RETURN_MASK_ALL);
  /* The only way to end up here is by an error (normal exit is
     handled by quit_force()), hence always return an error status.  */
  return 1;
}


/* Don't use *_filtered for printing help.  We don't want to prompt
   for continue no matter how small the screen or how much we're going
   to print.  */

static void
print_gdb_help (struct ui_file *stream)
{
  char *system_gdbinit;
  char *home_gdbinit;
  char *local_gdbinit;

  get_init_files (&system_gdbinit, &home_gdbinit, &local_gdbinit);

  fputs_unfiltered (_("\
This is the GNU debugger.  Usage:\n\n\
    gdb [options] [executable-file [core-file or process-id]]\n\
    gdb [options] --args executable-file [inferior-arguments ...]\n\n\
Options:\n\n\
"), stream);
  fputs_unfiltered (_("\
  --args             Arguments after executable-file are passed to inferior\n\
"), stream);
  fputs_unfiltered (_("\
  -b BAUDRATE        Set serial port baud rate used for remote debugging.\n\
  --batch            Exit after processing options.\n\
  --batch-silent     As for --batch, but suppress all gdb stdout output.\n\
  --return-child-result\n\
                     GDB exit code will be the child's exit code.\n\
  --cd=DIR           Change current directory to DIR.\n\
  --command=FILE, -x Execute GDB commands from FILE.\n\
  --eval-command=COMMAND, -ex\n\
                     Execute a single GDB command.\n\
                     May be used multiple times and in conjunction\n\
                     with --command.\n\
  --core=COREFILE    Analyze the core dump COREFILE.\n\
  --pid=PID          Attach to running process PID.\n\
"), stream);
  fputs_unfiltered (_("\
  --dbx              DBX compatibility mode.\n\
  --directory=DIR    Search for source files in DIR.\n\
  --epoch            Output information used by epoch emacs-GDB interface.\n\
  --exec=EXECFILE    Use EXECFILE as the executable.\n\
  --fullname         Output information used by emacs-GDB interface.\n\
  --help             Print this message.\n\
"), stream);
  fputs_unfiltered (_("\
  --interpreter=INTERP\n\
                     Select a specific interpreter / user interface\n\
"), stream);
  fputs_unfiltered (_("\
  -l TIMEOUT         Set timeout in seconds for remote debugging.\n\
  --nw		     Do not use a window interface.\n\
  --nx               Do not read "), stream);
  fputs_unfiltered (gdbinit, stream);
  fputs_unfiltered (_(" file.\n\
  --quiet            Do not print version number on startup.\n\
  --readnow          Fully read symbol files on first access.\n\
"), stream);
  fputs_unfiltered (_("\
  --se=FILE          Use FILE as symbol file and executable file.\n\
  --symbols=SYMFILE  Read symbols from SYMFILE.\n\
  --tty=TTY          Use TTY for input/output by the program being debugged.\n\
"), stream);
#if defined(TUI)
  fputs_unfiltered (_("\
  --tui              Use a terminal user interface.\n\
"), stream);
#endif
  fputs_unfiltered (_("\
  --version          Print version information and then exit.\n\
  -w                 Use a window interface.\n\
  --write            Set writing into executable and core files.\n\
  --xdb              XDB compatibility mode.\n\
"), stream);
  /* GOOGLE LOCAL */
  fputs_unfiltered (_("\
  --disable-gdb-index\n\
                     Disable the use of the .gdb_index section.\n\
"), stream);
  /* END GOOGLE LOCAL */
  fputs_unfiltered (_("\n\
At startup, GDB reads the following init files and executes their commands:\n\
"), stream);
  if (system_gdbinit)
    fprintf_unfiltered (stream, _("\
   * system-wide init file: %s\n\
"), system_gdbinit);
  if (home_gdbinit)
    fprintf_unfiltered (stream, _("\
   * user-specific init file: %s\n\
"), home_gdbinit);
  if (local_gdbinit)
    fprintf_unfiltered (stream, _("\
   * local init file: ./%s\n\
"), local_gdbinit);
  fputs_unfiltered (_("\n\
For more information, type \"help\" from within GDB, or consult the\n\
GDB manual (available as on-line info or a printed manual).\n\
"), stream);
  if (REPORT_BUGS_TO[0] && stream == gdb_stdout)
    fprintf_unfiltered (stream, _("\
Report bugs to \"%s\".\n\
"), REPORT_BUGS_TO);
}
