/*
 * init.c - main loop and initialization routines
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1992-1997 Paul Falstad
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Paul Falstad or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Paul Falstad and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Paul Falstad and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Paul Falstad and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zsh.mdh"

#include "zshpaths.h"
#include "zshxmods.h"

#include "init.pro"

#include "version.h"

/**/
int noexitct = 0;

/* buffer for $_ and its length */

/**/
char *zunderscore;

/**/
int underscorelen, underscoreused;

/* what level of sourcing we are at */
 
/**/
int sourcelevel;

/* the shell tty fd */

/**/
mod_export int SHTTY;

/* the FILE attached to the shell tty */

/**/
mod_export FILE *shout;

/* termcap strings */
 
/**/
mod_export char *tcstr[TC_COUNT];

/* lengths of each termcap string */
 
/**/
mod_export int tclen[TC_COUNT];

/* Values of the li, co and am entries */

/**/
int tclines, tccolumns;
/**/
mod_export int hasam, hasxn;

/* Value of the Co (max_colors) entry: may not be set */

/**/
mod_export int tccolours;

/* SIGCHLD mask */

/**/
mod_export sigset_t sigchld_mask;

/**/
mod_export struct hookdef zshhooks[] = {
    HOOKDEF("exit", NULL, HOOKF_ALL),
    HOOKDEF("before_trap", NULL, HOOKF_ALL),
    HOOKDEF("after_trap", NULL, HOOKF_ALL),
};

/* keep executing lists until EOF found */

/**/
enum loop_return
loop(int toplevel, int justonce)
{
    Eprog prog;
    int err, non_empty = 0;

    pushheap();
    if (!toplevel)
	lexsave();
    for (;;) {
	freeheap();
	if (stophist == 3)	/* re-entry via preprompt() */
	    hend(NULL);
	hbegin(1);		/* init history mech        */
	if (isset(SHINSTDIN)) {
	    setblock_stdin();
	    if (interact && toplevel) {
	        int hstop = stophist;
		stophist = 3;
		preprompt();
		if (stophist != 3)
		    hbegin(1);
		else
		    stophist = hstop;
		errflag = 0;
	    }
	}
	use_exit_printed = 0;
	intr();			/* interrupts on            */
	lexinit();              /* initialize lexical state */
	if (!(prog = parse_event())) {	/* if we couldn't parse a list */
	    hend(NULL);
	    if ((tok == ENDINPUT && !errflag) ||
		(tok == LEXERR && (!isset(SHINSTDIN) || !toplevel)) ||
		justonce)
		break;
	    if (exit_pending) {
		/*
		 * Something down there (a ZLE function?) decided
		 * to exit when there was stuff to clear up.
		 * Handle that now.
		 */
		stopmsg = 1;
		zexit(exit_pending >> 1, 0);
	    }
	    if (tok == LEXERR && !lastval)
		lastval = 1;
	    continue;
	}
	if (hend(prog)) {
	    int toksav = tok;

	    non_empty = 1;
	    if (toplevel &&
		(getshfunc("preexec") ||
		 paramtab->getnode(paramtab, "preexec" HOOK_SUFFIX))) {
		LinkList args;
		char *cmdstr;

		/*
		 * As we're about to freeheap() or popheap()
		 * anyway, there's no gain in using permanent
		 * storage here.
		 */
		args = newlinklist();
		addlinknode(args, "preexec");
		/* If curline got dumped from the history, we don't know
		 * what the user typed. */
		if (hist_ring && curline.histnum == curhist)
		    addlinknode(args, hist_ring->node.nam);
		else
		    addlinknode(args, "");
		addlinknode(args, dupstring(getjobtext(prog, NULL)));
		addlinknode(args, cmdstr = getpermtext(prog, NULL, 0));

		callhookfunc("preexec", args, 1, NULL);

		/* The only permanent storage is from getpermtext() */
		zsfree(cmdstr);
		errflag = 0;
	    }
	    if (stopmsg)	/* unset 'you have stopped jobs' flag */
		stopmsg--;
	    execode(prog, 0, 0, toplevel ? "toplevel" : "file");
	    tok = toksav;
	    if (toplevel)
		noexitct = 0;
	}
	if (ferror(stderr)) {
	    zerr("write error");
	    clearerr(stderr);
	}
	if (subsh)		/* how'd we get this far in a subshell? */
	    exit(lastval);
	if (((!interact || sourcelevel) && errflag) || retflag)
	    break;
	if (isset(SINGLECOMMAND) && toplevel) {
	    if (sigtrapped[SIGEXIT])
		dotrap(SIGEXIT);
	    exit(lastval);
	}
	if (justonce)
	    break;
    }
    err = errflag;
    if (!toplevel)
	lexrestore();
    popheap();

    if (err)
	return LOOP_ERROR;
    if (!non_empty)
	return LOOP_EMPTY;
    return LOOP_OK;
}

static char *cmd;
static int restricted;

/**/
static void
parseargs(char **argv, char **runscript)
{
    int optionbreak = 0;
    char **x;
    int action, optno;
    LinkList paramlist;

    argzero = *argv++;
    SHIN = 0;

    /* There's a bit of trickery with opts[INTERACTIVE] here.  It starts *
     * at a value of 2 (instead of 1) or 0.  If it is explicitly set on  *
     * the command line, it goes to 1 or 0.  If input is coming from     *
     * somewhere that normally makes the shell non-interactive, we do    *
     * "opts[INTERACTIVE] &= 1", so that only a *default* on state will  *
     * be changed.  At the end of the function, a value of 2 gets        *
     * changed to 1.                                                     */
    opts[INTERACTIVE] = isatty(0) ? 2 : 0;
    /*
     * MONITOR is similar:  we initialise it to 2, and if it's
     * still 2 at the end, we set it to the value of INTERACTIVE.
     */
    opts[MONITOR] = 2;   /* may be unset in init_io() */
    opts[SHINSTDIN] = 0;
    opts[SINGLECOMMAND] = 0;

    /* loop through command line options (begins with "-" or "+") */
    while (!optionbreak && *argv && (**argv == '-' || **argv == '+')) {
	char *args = *argv;
	action = (**argv == '-');
	if (!argv[0][1])
	    *argv = "--";
	while (*++*argv) {
	    if (**argv == '-') {
		if(!argv[0][1]) {
		    /* The pseudo-option `--' signifies the end of options. */
		    argv++;
		    goto doneoptions;
		}
		if(*argv != args+1 || **argv != '-')
		    goto badoptionstring;
		/* GNU-style long options */
		++*argv;
		if (!strcmp(*argv, "version")) {
		    printf("zsh %s (%s-%s-%s)\n",
			    ZSH_VERSION, MACHTYPE, VENDOR, OSTYPE);
		    exit(0);
		}
		if (!strcmp(*argv, "help")) {
		    printhelp();
		    exit(0);
		}
		/* `-' characters are allowed in long options */
		for(args = *argv; *args; args++)
		    if(*args == '-')
			*args = '_';
		goto longoptions;
	    }

	    if (unset(SHOPTIONLETTERS) && **argv == 'b') {
		/* -b ends options at the end of this argument */
		optionbreak = 1;
	    } else if (**argv == 'c') {
		/* -c command */
		cmd = *argv;
		opts[INTERACTIVE] &= 1;
		scriptname = scriptfilename = ztrdup("zsh");
	    } else if (**argv == 'o') {
		if (!*++*argv)
		    argv++;
		if (!*argv) {
		    zerr("string expected after -o");
		    exit(1);
		}
	    longoptions:
		if (!(optno = optlookup(*argv))) {
		    zerr("no such option: %s", *argv);
		    exit(1);
		} else if (optno == RESTRICTED)
		    restricted = action;
		else
		    dosetopt(optno, action, 1);
              break;
	    } else if (isspace(STOUC(**argv))) {
		/* zsh's typtab not yet set, have to use ctype */
		while (*++*argv)
		    if (!isspace(STOUC(**argv))) {
		    badoptionstring:
			zerr("bad option string: `%s'", args);
			exit(1);
		    }
		break;
	    } else {
	    	if (!(optno = optlookupc(**argv))) {
		    zerr("bad option: -%c", **argv);
		    exit(1);
		} else if (optno == RESTRICTED)
		    restricted = action;
		else
		    dosetopt(optno, action, 1);
	    }
	}
	argv++;
    }
    doneoptions:
    paramlist = znewlinklist();
    if (cmd) {
	if (!*argv) {
	    zerr("string expected after -%s", cmd);
	    exit(1);
	}
	cmd = *argv++;
    }
    if (*argv) {
	if (unset(SHINSTDIN)) {
	    if (cmd)
		argzero = *argv;
	    else
		*runscript = *argv;
	    opts[INTERACTIVE] &= 1;
	    argv++;
	}
	while (*argv)
	    zaddlinknode(paramlist, ztrdup(*argv++));
    } else if (!cmd)
	opts[SHINSTDIN] = 1;
    if(isset(SINGLECOMMAND))
	opts[INTERACTIVE] &= 1;
    opts[INTERACTIVE] = !!opts[INTERACTIVE];
    if (opts[MONITOR] == 2)
	opts[MONITOR] = opts[INTERACTIVE];
    pparams = x = (char **) zshcalloc((countlinknodes(paramlist) + 1) * sizeof(char *));

    while ((*x++ = (char *)getlinknode(paramlist)));
    free(paramlist);
    argzero = ztrdup(argzero);
}

/**/
static void
printhelp(void)
{
    printf("Usage: %s [<options>] [<argument> ...]\n", argzero);
    printf("\nSpecial options:\n");
    printf("  --help     show this message, then exit\n");
    printf("  --version  show zsh version number, then exit\n");
    if(unset(SHOPTIONLETTERS))
	printf("  -b         end option processing, like --\n");
    printf("  -c         take first argument as a command to execute\n");
    printf("  -o OPTION  set an option by name (see below)\n");
    printf("\nNormal options are named.  An option may be turned on by\n");
    printf("`-o OPTION', `--OPTION', `+o no_OPTION' or `+-no-OPTION'.  An\n");
    printf("option may be turned off by `-o no_OPTION', `--no-OPTION',\n");
    printf("`+o OPTION' or `+-OPTION'.  Options are listed below only in\n");
    printf("`--OPTION' or `--no-OPTION' form.\n");
    printoptionlist();
}

/**/
mod_export void
init_io(void)
{
    static char outbuf[BUFSIZ], errbuf[BUFSIZ];

#ifdef RSH_BUG_WORKAROUND
    int i;
#endif

/* stdout, stderr fully buffered */
#ifdef _IOFBF
    setvbuf(stdout, outbuf, _IOFBF, BUFSIZ);
    setvbuf(stderr, errbuf, _IOFBF, BUFSIZ);
#else
    setbuffer(stdout, outbuf, BUFSIZ);
    setbuffer(stderr, errbuf, BUFSIZ);
#endif

/* This works around a bug in some versions of in.rshd. *
 * Currently this is not defined by default.            */
#ifdef RSH_BUG_WORKAROUND
    if (cmd) {
	for (i = 3; i < 10; i++)
	    close(i);
    }
#endif

    if (shout) {
	/*
	 * Check if shout was set to stderr, if so don't close it.
	 * We do this if we are interactive but don't have a
	 * terminal.
	 */
	if (shout != stderr)
	    fclose(shout);
	shout = 0;
    }
    if (SHTTY != -1) {
	zclose(SHTTY);
	SHTTY = -1;
    }

    /* Send xtrace output to stderr -- see execcmd() */
    xtrerr = stderr;

    /* Make sure the tty is opened read/write. */
    if (isatty(0)) {
	zsfree(ttystrname);
	if ((ttystrname = ztrdup(ttyname(0)))) {
	    SHTTY = movefd(open(ttystrname, O_RDWR | O_NOCTTY));
#ifdef TIOCNXCL
	    /*
	     * See if the terminal claims to be busy.  If so, and fd 0
	     * is a terminal, try and set non-exclusive use for that.
	     * This is something to do with Solaris over-cleverness.
	     */
	    if (SHTTY == -1 && errno == EBUSY)
		ioctl(0, TIOCNXCL, 0);
#endif
	}
	/*
	 * xterm, rxvt and probably all terminal emulators except
	 * dtterm on Solaris 2.6 & 7 have a bug. Applications are
	 * unable to open /dev/tty or /dev/pts/<terminal number here>
	 * because something in Sun's STREAMS modules doesn't like
	 * it. The open() call fails with EBUSY which is not even
	 * listed as a possibility in the open(2) man page.  So we'll
	 * try to outsmart The Company.  -- <dave@srce.hr>
	 *
	 * Presumably there's no harm trying this on any OS, given that
	 * isatty(0) worked but opening the tty didn't.  Possibly we won't
	 * get the tty read/write, but it's the best we can do -- pws
	 *
	 * Try both stdin and stdout before trying /dev/tty. -- Bart
	 */
#if defined(HAVE_FCNTL_H) && defined(F_GETFL)
#define rdwrtty(fd)	((fcntl(fd, F_GETFL, 0) & O_RDWR) == O_RDWR)
#else
#define rdwrtty(fd)	1
#endif
	if (SHTTY == -1 && rdwrtty(0)) {
	    SHTTY = movefd(dup(0));
	}
    }
    if (SHTTY == -1 && isatty(1) && rdwrtty(1) &&
	(SHTTY = movefd(dup(1))) != -1) {
	zsfree(ttystrname);
	ttystrname = ztrdup(ttyname(1));
    }
    if (SHTTY == -1 &&
	(SHTTY = movefd(open("/dev/tty", O_RDWR | O_NOCTTY))) != -1) {
	zsfree(ttystrname);
	ttystrname = ztrdup(ttyname(SHTTY));
    }
    if (SHTTY == -1) {
	zsfree(ttystrname);
	ttystrname = ztrdup("");
    } else {
#ifdef FD_CLOEXEC
	long fdflags = fcntl(SHTTY, F_GETFD, 0);
	if (fdflags != (long)-1) {
	    fdflags |= FD_CLOEXEC;
	    fcntl(SHTTY, F_SETFD, fdflags);
	}
#endif
	if (!ttystrname)
	    ttystrname = ztrdup("/dev/tty");
    }

    /* We will only use zle if shell is interactive, *
     * SHTTY != -1, and shout != 0                   */
    if (interact) {
	init_shout();
	if(!SHTTY || !shout)
	    opts[USEZLE] = 0;
    } else
	opts[USEZLE] = 0;

#ifdef JOB_CONTROL
    /* If interactive, make sure the shell is in the foreground and is the
     * process group leader.
     */
    mypid = (zlong)getpid();
    if (opts[MONITOR] && (SHTTY != -1)) {
	origpgrp = GETPGRP();
        acquire_pgrp(); /* might also clear opts[MONITOR] */
    } else
	opts[MONITOR] = 0;
#else
    opts[MONITOR] = 0;
#endif
}

/**/
mod_export void
init_shout(void)
{
    static char shoutbuf[BUFSIZ];
#if defined(JOB_CONTROL) && defined(TIOCSETD) && defined(NTTYDISC)
    int ldisc;
#endif

    if (SHTTY == -1)
    {
	/* Since we're interative, it's nice to have somewhere to write. */
	shout = stderr;
	return;
    }

#if defined(JOB_CONTROL) && defined(TIOCSETD) && defined(NTTYDISC)
    ldisc = NTTYDISC;
    ioctl(SHTTY, TIOCSETD, (char *)&ldisc);
#endif

    /* Associate terminal file descriptor with a FILE pointer */
    shout = fdopen(SHTTY, "w");
#ifdef _IOFBF
    setvbuf(shout, shoutbuf, _IOFBF, BUFSIZ);
#endif
  
    gettyinfo(&shttyinfo);	/* get tty state */
#if defined(__sgi)
    if (shttyinfo.tio.c_cc[VSWTCH] <= 0)	/* hack for irises */
	shttyinfo.tio.c_cc[VSWTCH] = CSWTCH;
#endif
}

/* names of the termcap strings we want */

static char *tccapnams[TC_COUNT] = {
    "cl", "le", "LE", "nd", "RI", "up", "UP", "do",
    "DO", "dc", "DC", "ic", "IC", "cd", "ce", "al", "dl", "ta",
    "md", "so", "us", "me", "se", "ue", "ch",
    "ku", "kd", "kl", "kr", "sc", "rc", "bc", "AF", "AB"
};

/* Initialise termcap */

/**/
mod_export int
init_term(void)
{
#ifndef TGETENT_ACCEPTS_NULL
    static char termbuf[2048];	/* the termcap buffer */
#endif

    if (!*term) {
	termflags |= TERM_UNKNOWN;
	return 0;
    }

    /* unset zle if using zsh under emacs */
    if (!strcmp(term, "emacs"))
	opts[USEZLE] = 0;

#ifdef TGETENT_ACCEPTS_NULL
    /* If possible, we let tgetent allocate its own termcap buffer */
    if (tgetent(NULL, term) != TGETENT_SUCCESS)
#else
    if (tgetent(termbuf, term) != TGETENT_SUCCESS)
#endif
    {
	if (isset(INTERACTIVE))
	    zerr("can't find terminal definition for %s", term);
	errflag = 0;
	termflags |= TERM_BAD;
	return 0;
    } else {
	char tbuf[1024], *pp;
	int t0;

	termflags &= ~TERM_BAD;
	termflags &= ~TERM_UNKNOWN;
	for (t0 = 0; t0 != TC_COUNT; t0++) {
	    pp = tbuf;
	    zsfree(tcstr[t0]);
	/* AIX tgetstr() ignores second argument */
	    if (!(pp = tgetstr(tccapnams[t0], &pp)))
		tcstr[t0] = NULL, tclen[t0] = 0;
	    else {
		tclen[t0] = strlen(pp);
		tcstr[t0] = (char *) zalloc(tclen[t0] + 1);
		memcpy(tcstr[t0], pp, tclen[t0] + 1);
	    }
	}

	/* check whether terminal has automargin (wraparound) capability */
	hasam = tgetflag("am");
	hasxn = tgetflag("xn"); /* also check for newline wraparound glitch */

	tclines = tgetnum("li");
	tccolumns = tgetnum("co");
	tccolours = tgetnum("Co");

	/* if there's no termcap entry for cursor up, use single line mode: *
	 * this is flagged by termflags which is examined in zle_refresh.c  *
	 */
	if (tccan(TCUP))
	    termflags &= ~TERM_NOUP;
	else {
	    zsfree(tcstr[TCUP]);
	    tcstr[TCUP] = NULL;
	    termflags |= TERM_NOUP;
	}

	/* most termcaps don't define "bc" because they use \b. */
	if (!tccan(TCBACKSPACE)) {
	    zsfree(tcstr[TCBACKSPACE]);
	    tcstr[TCBACKSPACE] = ztrdup("\b");
	    tclen[TCBACKSPACE] = 1;
	}

	/* if there's no termcap entry for cursor left, use backspace. */
	if (!tccan(TCLEFT)) {
	    zsfree(tcstr[TCLEFT]);
	    tcstr[TCLEFT] = ztrdup(tcstr[TCBACKSPACE]);
	    tclen[TCLEFT] = tclen[TCBACKSPACE];
	}

	if (tccan(TCSAVECURSOR) && !tccan(TCRESTRCURSOR)) {
	    tclen[TCSAVECURSOR] = 0;
	    zsfree(tcstr[TCSAVECURSOR]);
	    tcstr[TCSAVECURSOR] = NULL;
	}

	/* if the termcap entry for down is \n, don't use it. */
	if (tccan(TCDOWN) && tcstr[TCDOWN][0] == '\n') {
	    tclen[TCDOWN] = 0;
	    zsfree(tcstr[TCDOWN]);
	    tcstr[TCDOWN] = NULL;
	}

	/* if there's no termcap entry for clear, use ^L. */
	if (!tccan(TCCLEARSCREEN)) {
	    zsfree(tcstr[TCCLEARSCREEN]);
	    tcstr[TCCLEARSCREEN] = ztrdup("\14");
	    tclen[TCCLEARSCREEN] = 1;
	}
    }
    return 1;
}

/* Initialize lots of global variables and hash tables */

/**/
void
setupvals(void)
{
#ifdef USE_GETPWUID
    struct passwd *pswd;
#endif
    struct timezone dummy_tz;
    char *ptr;
    int i, j;
#if defined(SITEFPATH_DIR) || defined(FPATH_DIR)
    char **fpathptr;
# if defined(FPATH_DIR) && defined(FPATH_SUBDIRS)
    char *fpath_subdirs[] = FPATH_SUBDIRS;
# endif
# ifdef SITEFPATH_DIR
    int fpathlen = 1;
# else
    int fpathlen = 0;
# endif
#endif
    int close_fds[10], tmppipe[2];

    /*
     * Workaround a problem with NIS (in one guise or another) which
     * grabs file descriptors and keeps them for future reference.
     * We don't want these to be in the range where the user can
     * open fd's, i.e. 0 to 9 inclusive.  So we make sure all
     * fd's in that range are in use.
     */
    memset(close_fds, 0, 10*sizeof(int));
    if (pipe(tmppipe) == 0) {
	/*
	 * Strategy:  Make sure we have at least fd 0 open (hence
	 * the pipe).  From then on, keep dup'ing until we are
	 * up to 9.  If we go over the top, close immediately, else
	 * mark for later closure.
	 */
	i = -1;			/* max fd we have checked */
	while (i < 9) {
	    /* j is current fd */
	    if (i < tmppipe[0])
		j = tmppipe[0];
	    else if (i < tmppipe[1])
		j = tmppipe[1];
	    else {
		j = dup(0);
		if (j == -1)
		    break;
	    }
	    if (j < 10)
		close_fds[j] = 1;
	    else
		close(j);
	    if (i < j)
		i = j;
	}
	if (i < tmppipe[0])
	    close(tmppipe[0]);
	if (i < tmppipe[1])
	    close(tmppipe[1]);
    }

    (void)addhookdefs(NULL, zshhooks, sizeof(zshhooks)/sizeof(*zshhooks));

    init_eprog();

    zero_mnumber.type = MN_INTEGER;
    zero_mnumber.u.l = 0;

    noeval = 0;
    curhist = 0;
    histsiz = DEFAULT_HISTSIZE;
    inithist();

    cmdstack = (unsigned char *) zalloc(CMDSTACKSZ);
    cmdsp = 0;

    bangchar = '!';
    hashchar = '#';
    hatchar = '^';
    termflags = TERM_UNKNOWN;
    curjob = prevjob = coprocin = coprocout = -1;
    gettimeofday(&shtimer, &dummy_tz);	/* init $SECONDS */
    srand((unsigned int)(shtimer.tv_sec + shtimer.tv_usec)); /* seed $RANDOM */

    /* Set default path */
    path    = (char **) zalloc(sizeof(*path) * 5);
    path[0] = ztrdup("/bin");
    path[1] = ztrdup("/usr/bin");
    path[2] = ztrdup("/usr/ucb");
    path[3] = ztrdup("/usr/local/bin");
    path[4] = NULL;

    cdpath   = mkarray(NULL);
    manpath  = mkarray(NULL);
    fignore  = mkarray(NULL);

#if defined(SITEFPATH_DIR) || defined(FPATH_DIR)
# ifdef FPATH_DIR
#  ifdef FPATH_SUBDIRS
    fpathlen += sizeof(fpath_subdirs)/sizeof(char *);
#  else
    fpathlen++;
#  endif
# endif
    fpath = fpathptr = (char **)zalloc((fpathlen+1)*sizeof(char *));
# ifdef SITEFPATH_DIR
    *fpathptr++ = ztrdup(SITEFPATH_DIR);
    fpathlen--;
# endif
# ifdef FPATH_DIR
#  ifdef FPATH_SUBDIRS
    for (j = 0; j < fpathlen; j++)
	*fpathptr++ = tricat(FPATH_DIR, "/", fpath_subdirs[j]);
#  else
    *fpathptr++ = ztrdup(FPATH_DIR);
#  endif
# endif
    *fpathptr = NULL;
#else
    fpath    = mkarray(NULL);
#endif

    mailpath = mkarray(NULL);
    watch    = mkarray(NULL);
    psvar    = mkarray(NULL);
    module_path = mkarray(ztrdup(MODULE_DIR));
    modulestab = newmoduletable(17, "modules");
    linkedmodules = znewlinklist();

    /* Set default prompts */
    if(unset(INTERACTIVE)) {
	prompt = ztrdup("");
	prompt2 = ztrdup("");
    } else if (EMULATION(EMULATE_KSH|EMULATE_SH)) {
	prompt  = ztrdup(privasserted() ? "# " : "$ ");
	prompt2 = ztrdup("> ");
    } else {
	prompt  = ztrdup("%m%# ");
	prompt2 = ztrdup("%_> ");
    }
    prompt3 = ztrdup("?# ");
    prompt4 = EMULATION(EMULATE_KSH|EMULATE_SH)
	? ztrdup("+ ") : ztrdup("+%N:%i> ");
    sprompt = ztrdup("zsh: correct '%R' to '%r' [nyae]? ");

    ifs         = EMULATION(EMULATE_KSH|EMULATE_SH) ?
	ztrdup(DEFAULT_IFS_SH) : ztrdup(DEFAULT_IFS);
    wordchars   = ztrdup(DEFAULT_WORDCHARS);
    postedit    = ztrdup("");
    zunderscore  = (char *) zalloc(underscorelen = 32);
    underscoreused = 1;
    *zunderscore = '\0';

    zoptarg = ztrdup("");
    zoptind = 1;

    ppid  = (zlong) getppid();
    mypid = (zlong) getpid();
    term  = ztrdup("");

    nullcmd     = ztrdup("cat");
    readnullcmd = ztrdup(DEFAULT_READNULLCMD);

    /* We cache the uid so we know when to *
     * recheck the info for `USERNAME'     */
    cached_uid = getuid();

    /* Get password entry and set info for `USERNAME' */
#ifdef USE_GETPWUID
    if ((pswd = getpwuid(cached_uid))) {
	if (EMULATION(EMULATE_ZSH))
	    home = metafy(pswd->pw_dir, -1, META_DUP);
	cached_username = ztrdup(pswd->pw_name);
    }
    else
#endif /* USE_GETPWUID */
    {
	if (EMULATION(EMULATE_ZSH))
	    home = ztrdup("/");
	cached_username = ztrdup("");
    }

    /*
     * Try a cheap test to see if we can initialize `PWD' from `HOME'.
     * In non-native emulations HOME must come from the environment;
     * we're not allowed to set it locally.
     */
    if (EMULATION(EMULATE_ZSH))
	ptr = home;
    else
	ptr = zgetenv("HOME");
    if (ptr && ispwd(ptr))
	pwd = ztrdup(ptr);
    else if ((ptr = zgetenv("PWD")) && (strlen(ptr) < PATH_MAX) &&
	     (ptr = metafy(ptr, -1, META_STATIC), ispwd(ptr)))
	pwd = ztrdup(ptr);
    else {
	pwd = NULL;
	pwd = metafy(zgetcwd(), -1, META_DUP);
    }

    oldpwd = ztrdup(pwd);  /* initialize `OLDPWD' = `PWD' */

    inittyptab();     /* initialize the ztypes table */
    initlextabs();    /* initialize lexing tables    */

    createreswdtable();     /* create hash table for reserved words    */
    createaliastables();    /* create hash tables for aliases           */
    createcmdnamtable();    /* create hash table for external commands */
    createshfunctable();    /* create hash table for shell functions   */
    createbuiltintable();   /* create hash table for builtin commands  */
    createnameddirtable();  /* create hash table for named directories */
    createparamtable();     /* create parameter hash table             */

    condtab = NULL;
    wrappers = NULL;

#ifdef TIOCGWINSZ
    adjustwinsize(0);
#else
    /* columns and lines are normally zero, unless something different *
     * was inhereted from the environment.  If either of them are zero *
     * the setiparam calls below set them to the defaults from termcap */
    setiparam("COLUMNS", columns);
    setiparam("LINES", lines);
#endif

#ifdef HAVE_GETRLIMIT
    for (i = 0; i != RLIM_NLIMITS; i++) {
	getrlimit(i, current_limits + i);
	limits[i] = current_limits[i];
    }
#endif

    breaks = loops = 0;
    lastmailcheck = time(NULL);
    locallevel = sourcelevel = 0;
    sfcontext = SFC_NONE;
    trap_return = 0;
    trap_state = TRAP_STATE_INACTIVE;
    noerrexit = -1;
    nohistsave = 1;
    dirstack = znewlinklist();
    bufstack = znewlinklist();
    hsubl = hsubr = NULL;
    lastpid = 0;
    lastpid_status = -1L;

    get_usage();

    /* Close the file descriptors we opened to block off 0 to 9 */
    for (i = 0; i < 10; i++)
	if (close_fds[i])
	    close(i);

    /* Colour sequences for outputting colours in prompts and zle */
    set_default_colour_sequences();
}

/*
 * Setup shell input, opening any script file (runscript, may be NULL).
 * This is deferred until we have a path to search, in case
 * PATHSCRIPT is set for sh-compatible behaviour.
 */
static void
setupshin(char *runscript)
{
    if (runscript) {
	char *funmeta, *sfname = NULL;
	struct stat st;

	funmeta = unmeta(runscript);
	/*
	 * Always search the current directory first.
	 */
	if (access(funmeta, F_OK) == 0 &&
	    stat(funmeta, &st) >= 0 &&
	    !S_ISDIR(st.st_mode))
	    sfname = runscript;
	else if (isset(PATHSCRIPT) && !strchr(runscript, '/')) {
	    /*
	     * With the PATHSCRIPT option, search the path if no
	     * path was given in the script name.
	     */
	    funmeta = pathprog(runscript, &sfname);
	}
	if (!sfname ||
	    (SHIN = movefd(open(funmeta, O_RDONLY | O_NOCTTY)))
	    == -1) {
	    zerr("can't open input file: %s", runscript);
	    exit(127);
	}
	scriptfilename = sfname;
	argzero = runscript;
    }
    /*
     * We only initialise line numbering once there is a script to
     * read commands from.
     */
    lineno = 1;
    /*
     * Finish setting up SHIN and its relatives.
     */
    bshin = SHIN ? fdopen(SHIN, "r") : stdin;
    if (isset(SHINSTDIN) && !SHIN && unset(INTERACTIVE)) {
#ifdef _IONBF
	setvbuf(stdin, NULL, _IONBF, 0);
#else
	setlinebuf(stdin);
#endif
    }
}

/* Initialize signal handling */

/**/
void
init_signals(void)
{
    if (interact) {
	int i;
	signal_setmask(signal_mask(0));
	for (i=0; i<NSIG; ++i)
	    signal_default(i);
    }
    sigchld_mask = signal_mask(SIGCHLD);

    intr();

#ifndef QDEBUG
    signal_ignore(SIGQUIT);
#endif

    if (signal_ignore(SIGHUP) == SIG_IGN)
	opts[HUP] = 0;
    else
	install_handler(SIGHUP);
    install_handler(SIGCHLD);
#ifdef SIGWINCH
    install_handler(SIGWINCH);
#endif
    if (interact) {
	install_handler(SIGALRM);
	signal_ignore(SIGTERM);
    }
    if (jobbing) {
	signal_ignore(SIGTTOU);
	signal_ignore(SIGTSTP);
	signal_ignore(SIGTTIN);
    }
}

/* Source the init scripts.  If called as "ksh" or "sh"  *
 * then we source the standard sh/ksh scripts instead of *
 * the standard zsh scripts                              */

/**/
void
run_init_scripts(void)
{
    noerrexit = -1;

    if (EMULATION(EMULATE_KSH|EMULATE_SH)) {
	if (islogin)
	    source("/etc/profile");
	if (unset(PRIVILEGED)) {
	    char *s = getsparam("ENV");
	    if (islogin)
		sourcehome(".profile");
	    noerrs = 2;
	    if (s && !parsestr(s)) {
		singsub(&s);
		noerrs = 0;
		source(s);
	    }
	    noerrs = 0;
	} else
	    source("/etc/suid_profile");
    } else {
#ifdef GLOBAL_ZSHENV
	source(GLOBAL_ZSHENV);
#endif

	if (isset(RCS) && unset(PRIVILEGED))
	{
	    if (isset(INTERACTIVE)) {
		/*
		 * Always attempt to load the newuser module to perform
		 * checks for new zsh users.  Don't care if we can't load it.
		 */
		if (!load_module("zsh/newuser", NULL, 1)) {
		    /* Unload it immediately. */
		    unload_named_module("zsh/newuser", "zsh", 1);
		}
	    }

	    sourcehome(".zshenv");
	}
	if (islogin) {
#ifdef GLOBAL_ZPROFILE
	    if (isset(RCS) && isset(GLOBALRCS))
		    source(GLOBAL_ZPROFILE);
#endif
	    if (isset(RCS) && unset(PRIVILEGED))
		sourcehome(".zprofile");
	}
	if (interact) {
#ifdef GLOBAL_ZSHRC
	    if (isset(RCS) && isset(GLOBALRCS))
		source(GLOBAL_ZSHRC);
#endif
	    if (isset(RCS) && unset(PRIVILEGED))
		sourcehome(".zshrc");
	}
	if (islogin) {
#ifdef GLOBAL_ZLOGIN
	    if (isset(RCS) && isset(GLOBALRCS))
		source(GLOBAL_ZLOGIN);
#endif
	    if (isset(RCS) && unset(PRIVILEGED))
		sourcehome(".zlogin");
	}
    }
    noerrexit = 0;
    nohistsave = 0;
}

/* Miscellaneous initializations that happen after init scripts are run */

/**/
void
init_misc(void)
{
#ifndef RESTRICTED_R
    if ( restricted )
#else
    if (*zsh_name == 'r' || restricted)
#endif
	dosetopt(RESTRICTED, 1, 0);
    if (cmd) {
	if (SHIN >= 10)
	    fclose(bshin);
	SHIN = movefd(open("/dev/null", O_RDONLY | O_NOCTTY));
	bshin = fdopen(SHIN, "r");
	execstring(cmd, 0, 1, "cmdarg");
	stopmsg = 1;
	zexit(lastval, 0);
    }

    if (interact && isset(RCS))
	readhistfile(NULL, 0, HFILE_USE_OPTIONS);
}

/*
 * source a file
 * Returns one of the SOURCE_* enum values.
 */

/**/
mod_export enum source_return
source(char *s)
{
    Eprog prog;
    int tempfd = -1, fd, cj;
    zlong oldlineno;
    int oldshst, osubsh, oloops;
    FILE *obshin;
    char *old_scriptname = scriptname, *us;
    char *old_scriptfilename = scriptfilename;
    unsigned char *ocs;
    int ocsp;
    int otrap_return = trap_return, otrap_state = trap_state;
    struct funcstack fstack;
    enum source_return ret = SOURCE_OK;

    if (!s || 
	(!(prog = try_source_file((us = unmeta(s)))) &&
	 (tempfd = movefd(open(us, O_RDONLY | O_NOCTTY))) == -1)) {
	return SOURCE_NOT_FOUND;
    }

    /* save the current shell state */
    fd        = SHIN;            /* store the shell input fd                  */
    obshin    = bshin;          /* store file handle for buffered shell input */
    osubsh    = subsh;           /* store whether we are in a subshell        */
    cj        = thisjob;         /* store our current job number              */
    oldlineno = lineno;          /* store our current lineno                  */
    oloops    = loops;           /* stored the # of nested loops we are in    */
    oldshst   = opts[SHINSTDIN]; /* store current value of this option        */
    ocs = cmdstack;
    ocsp = cmdsp;
    cmdstack = (unsigned char *) zalloc(CMDSTACKSZ);
    cmdsp = 0;

    if (!prog) {
	SHIN = tempfd;
	bshin = fdopen(SHIN, "r");
    }
    subsh  = 0;
    lineno = 1;
    loops  = 0;
    dosetopt(SHINSTDIN, 0, 1);
    scriptname = s;
    scriptfilename = s;

    if (isset(SOURCETRACE)) {
	printprompt4();
	fprintf(xtrerr ? xtrerr : stderr, "<sourcetrace>\n");
    }

    /*
     * The special return behaviour of traps shouldn't
     * trigger in files sourced from traps; the return
     * is just a return from the file.
     */
    trap_state = TRAP_STATE_INACTIVE;

    sourcelevel++;

    fstack.name = scriptfilename;
    fstack.caller = funcstack ? funcstack->name :
	dupstring(old_scriptfilename ? old_scriptfilename : "zsh");
    fstack.flineno = 0;
    fstack.lineno = oldlineno;
    fstack.filename = scriptfilename;
    fstack.prev = funcstack;
    fstack.tp = FS_SOURCE;
    funcstack = &fstack;

    if (prog) {
	pushheap();
	errflag = 0;
	execode(prog, 1, 0, "filecode");
	popheap();
	if (errflag)
	    ret = SOURCE_ERROR;
    } else {
	/* loop through the file to be sourced  */
	switch (loop(0, 0))
	{
	case LOOP_OK:
	    /* nothing to do but compilers like a complete enum */
	    break;

	case LOOP_EMPTY:
	    /* Empty code resets status */
	    lastval = 0;
	    break;

	case LOOP_ERROR:
	    ret = SOURCE_ERROR;
	    break;
	}
    }
    funcstack = funcstack->prev;
    sourcelevel--;

    trap_state = otrap_state;
    trap_return = otrap_return;

    /* restore the current shell state */
    if (prog)
	freeeprog(prog);
    else {
	fclose(bshin);
	fdtable[SHIN] = FDT_UNUSED;
	SHIN = fd;		     /* the shell input fd                   */
	bshin = obshin;		     /* file handle for buffered shell input */
    }
    subsh = osubsh;                  /* whether we are in a subshell         */
    thisjob = cj;                    /* current job number                   */
    lineno = oldlineno;              /* our current lineno                   */
    loops = oloops;                  /* the # of nested loops we are in      */
    dosetopt(SHINSTDIN, oldshst, 1); /* SHINSTDIN option                     */
    errflag = 0;
    if (!exit_pending)
	retflag = 0;
    scriptname = old_scriptname;
    scriptfilename = old_scriptfilename;
    free(cmdstack);
    cmdstack = ocs;
    cmdsp = ocsp;

    return ret;
}

/* Try to source a file in the home directory */

/**/
void
sourcehome(char *s)
{
    char *h;

    queue_signals();
    if (EMULATION(EMULATE_SH|EMULATE_KSH) || !(h = getsparam("ZDOTDIR"))) {
	h = home;
	if (!h)
	    return;
    }

    {
	/* Let source() complain if path is too long */
	VARARR(char, buf, strlen(h) + strlen(s) + 2);
	sprintf(buf, "%s/%s", h, s);
	unqueue_signals();
	source(buf);
    }
}

/**/
void
init_bltinmods(void)
{

#include "bltinmods.list"

    (void)load_module("zsh/main", NULL, 0);
}

/**/
mod_export void
noop_function(void)
{
    /* do nothing */
}

/**/
mod_export void
noop_function_int(UNUSED(int nothing))
{
    /* do nothing */
}

/*
 * ZLE entry point pointer.
 * No other source file needs to know which modules are linked in.
 */
/**/
mod_export ZleEntryPoint zle_entry_ptr;

/*
 * State of loading of zle.
 * 0 = Not loaded, not attempted.
 * 1 = Loaded successfully
 * 2 = Failed to load.
 */
/**/
mod_export int zle_load_state;

/**/
mod_export char *
zleentry(VA_ALIST1(int cmd))
VA_DCL
{
    char *ret = NULL;
    va_list ap;
    VA_DEF_ARG(int cmd);

    VA_START(ap, cmd);
    VA_GET_ARG(ap, cmd, int);

#if defined(LINKED_XMOD_zshQszle) || defined(UNLINKED_XMOD_zshQszle)
    /* autoload */
    switch (zle_load_state) {
    case 0:
	/*
	 * Some commands don't require us to load ZLE.
	 * These also have no fallback.
	 */
	if (cmd != ZLE_CMD_TRASH && cmd != ZLE_CMD_RESET_PROMPT &&
	    cmd != ZLE_CMD_REFRESH)
	{
	    if (load_module("zsh/zle", NULL, 0) != 1) {
		(void)load_module("zsh/compctl", NULL, 0);
		ret = zle_entry_ptr(cmd, ap);
		/* Don't execute fallback code */
		cmd = -1;
	    } else {
		zle_load_state = 2;
		/* Execute fallback code below */
	    }
	}
	break;

    case 1:
	ret = zle_entry_ptr(cmd, ap);
	/* Don't execute fallback code */
	cmd = -1;
	break;

    case 2:
	/* Execute fallback code */
	break;
    }
#endif

    switch (cmd) {
	/*
	 * Only the read command really needs a fallback if zle
	 * is not available.  ZLE_CMD_GET_LINE has traditionally
	 * had local code in bufferwords() to do this, but that'
	 * probably only because bufferwords() is part of completion
	 * and so everything to do with it is horribly complicated.
	 */
    case ZLE_CMD_READ:
    {
	char *pptbuf, **lp;
	int pptlen;

	lp = va_arg(ap, char **);

	pptbuf = unmetafy(promptexpand(lp ? *lp : NULL, 0, NULL, NULL,
				       NULL),
			  &pptlen);
	write_loop(2, pptbuf, pptlen);
	free(pptbuf);

	ret = shingetline();
	break;
    }

    case ZLE_CMD_GET_LINE:
    {
	int *ll, *cs;

	ll = va_arg(ap, int *);
	cs = va_arg(ap, int *);
	*ll = *cs = 0;
	ret = ztrdup("");
	break;
    }
    }

    va_end(ap);
    return ret;
}

/* compctl entry point pointers.  Similar to the ZLE ones. */

/**/
mod_export CompctlReadFn compctlreadptr = fallback_compctlread;

/**/
mod_export int
fallback_compctlread(char *name, UNUSED(char **args), UNUSED(Options ops), UNUSED(char *reply))
{
    zwarnnam(name, "option valid only in functions called from completion");
    return 1;
}

/*
 * Used by zle to indicate it has already printed a "use 'exit' to exit"
 * message.
 */
/**/
mod_export int use_exit_printed;

/*
 * This is real main entry point. This has to be mod_export'ed
 * so zsh.exe can found it on Cygwin
 */

/**/
mod_export int
zsh_main(UNUSED(int argc), char **argv)
{
    char **t, *runscript = NULL;
    int t0;
#ifdef USE_LOCALE
    setlocale(LC_ALL, "");
#endif

    init_jobs(argv, environ);

    /*
     * Provisionally set up the type table to allow metafication.
     * This will be done properly when we have decided if we are
     * interactive
     */
    typtab['\0'] |= IMETA;
    typtab[STOUC(Meta)  ] |= IMETA;
    typtab[STOUC(Marker)] |= IMETA;
    for (t0 = (int)STOUC(Pound); t0 <= (int)STOUC(Nularg); t0++)
	typtab[t0] |= ITOK | IMETA;

    for (t = argv; *t; *t = metafy(*t, -1, META_ALLOC), t++);

    zsh_name = argv[0];
    do {
      char *arg0 = zsh_name;
      if (!(zsh_name = strrchr(arg0, '/')))
	  zsh_name = arg0;
      else
	  zsh_name++;
      if (*zsh_name == '-')
	  zsh_name++;
      if (strcmp(zsh_name, "su") == 0) {
	  char *sh = zgetenv("SHELL");
	  if (sh && *sh && arg0 != sh)
	      zsh_name = sh;
	  else
	      break;
      } else
	  break;
    } while (zsh_name);

    fdtable_size = zopenmax();
    fdtable = zshcalloc(fdtable_size*sizeof(*fdtable));

    createoptiontable();
    emulate(zsh_name, 1);   /* initialises most options */
    opts[LOGINSHELL] = (**argv == '-');
    opts[PRIVILEGED] = (getuid() != geteuid() || getgid() != getegid());
    opts[USEZLE] = 1;   /* may be unset in init_io() */
    /* sets INTERACTIVE, SHINSTDIN and SINGLECOMMAND */
    parseargs(argv, &runscript);

    SHTTY = -1;
    init_io();
    setupvals();

    init_signals();
    init_bltinmods();
    init_builtins();
    run_init_scripts();
    setupshin(runscript);
    init_misc();

    for (;;) {
	/*
	 * See if we can free up some of jobtab.
	 * We only do this at top level, because if we are
	 * executing stuff we may refer to them by job pointer.
	 */
	maybeshrinkjobtab();

	do {
	    /* Reset return from top level which gets us back here */
	    retflag = 0;
	    loop(1,0);
	} while (tok != ENDINPUT && (tok != LEXERR || isset(SHINSTDIN)));
	if (tok == LEXERR) {
	    /* Make sure a parse error exits with non-zero status */
	    if (!lastval)
		lastval = 1;
	    stopmsg = 1;
	    zexit(lastval, 0);
	}
	if (!(isset(IGNOREEOF) && interact)) {
#if 0
	    if (interact)
		fputs(islogin ? "logout\n" : "exit\n", shout);
#endif
	    zexit(lastval, 0);
	    continue;
	}
	noexitct++;
	if (noexitct >= 10) {
	    stopmsg = 1;
	    zexit(lastval, 0);
	}
	/*
	 * Don't print the message if it was already handled by
	 * zle, since that makes special arrangements to keep
	 * the display tidy.
	 */
	if (!use_exit_printed)
	    zerrnam("zsh", (!islogin) ? "use 'exit' to exit."
		    : "use 'logout' to logout.");
    }
}
