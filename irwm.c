/*
 * irwm.c
 *
 * a panel-based window manager: only a window at time, in full screen
 *
 * gcc -I/usr/X11R6/include -Wall -Wextra \
 * -DLIRC -DXFT -I/usr/include/freetype2 \
 * -L/usr/X11R6/lib irwm.c -lX11 -llirc_client -lXft -o irwm
 *
 * xinit ./irwm
 * startx ./irwm
 *
 * configuration file is ~/.irwmrc or /etc/irwmrc
 *
 * keyboard:
 *
 *   alt-right		next panel
 *   alt-left		previous panel
 *   alt-tab		panel list	
 *   ctrl-tab		program list
 * 			in lists: up/down/return/escape
 *			only in panel list: c = close panel
 *   ctrl-shift-tab	quit
 *
 * lirc, or ClientMessage of message_type "IRWM" to the root window:
 *
 *   NOCOMMAND		nop
 *   NEXTPANEL		switch to next panel
 *   PREVPANEL		switch to previous panel
 *   QUIT		quit irwm
 *
 *   PANELWINDOW	show/hide the panel list window
 *   PROGSWINDOW	show/hide the program list window
 *
 *   UPWINDOW		up in the window
 *   DOWNWINDOW		down in the window
 *   HIDEWINDOW		hide window
 *   OKWINDOW		select the current item in the window
 *   KOWINDOW		only in the panel list window: close the current panel
 *
 * details on configuring and testing lirc are in file lircrd
 */

/*
 * todo:
 *
 * allow the size of a content window (x,y,width,height) to depend on its
 * properties like the title (XGetWMName), class (XGetClassHint) or program
 * (XGetCommand)
 *
 * window names are assumed to be ascii
 *
 * allow arguments to programs in config file
 *
 * select also property change events to update window names when they change
 */

/*
 * internals:
 *
 * when a program tries to map a window, irwm reparents it to a new window
 * called panel; it keeps an array of pairs panel/content, where the content is
 * the original window
 *
 * always map content first and then panel; unmap in reverse order
 *
 * alternative to the lirc client: select(2) on the socket descriptors obtained
 * from ConnectionNumber(dsp) and lirc_init(IRWM, 1)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef LIRC
#include <lirc_client.h>
#endif
#ifdef XFT
#include <X11/Xft/Xft.h>
#endif

/*
 * the lirc program name and the X atom used for client-client communication
 */
#define IRWM "IRWM"

/*
 * the default font for the irwm windows (panel list and program list)
 */
#define FONT "-*-*-*-*-*-*-24-*-*-*-*-*-*-1"
#define XFTFONT "Arial-15:bold"

/*
 * commands
 */
#define NOCOMMAND    0		/* no command */
#define NEXTPANEL    1		/* switch to next panel */
#define PREVPANEL    2		/* switch to previous panel */
#define QUIT         3		/* quit irwm */

#define PANELWINDOW 10		/* show the panel list window */
#define PROGSWINDOW 11		/* show the programs window */

#define UPWINDOW    20		/* up in the window */
#define DOWNWINDOW  21		/* down in the window */
#define HIDEWINDOW  22		/* hide both windows */
#define OKWINDOW    23		/* select the current item in the window */
#define KOWINDOW    24		/* close currently selected panel */

/*
 * commands, their names and keystrokes
 */
struct {
	int command;	char *string;	int keysym;	unsigned modifier;
} commandstring[] = {
	{NOCOMMAND,	"NOCOMMAND",	XK_VoidSymbol,	0},
	{NEXTPANEL,	"NEXTPANEL",	XK_Right,	Mod1Mask},
	{PREVPANEL,	"PREVPANEL",	XK_Left,	Mod1Mask},
	{QUIT,		"QUIT",		XK_Tab,	ControlMask | ShiftMask},
	{PANELWINDOW,	"PANELWINDOW",	XK_Tab,		Mod1Mask},
	{PROGSWINDOW,	"PROGSWINDOW",	XK_Tab,		ControlMask},
	{-1,		"ENDGRAB",	XK_VoidSymbol,	0},
	{UPWINDOW,	"UPWINDOW",	XK_Up,		0},
	{DOWNWINDOW,	"DOWNWINDOW",	XK_Down,	0},
	{HIDEWINDOW,	"HIDEWINDOW",	XK_Escape,	0},
	{OKWINDOW,	"OKWINDOW",	XK_Return,	0},
	{KOWINDOW,	"KOWINDOW",	XK_c,		0},
	{-1,		NULL,		XK_VoidSymbol,	0}
};
char *commandtostring(int command) {
	int i;
	for(i = 0; commandstring[i].string; i++)
		if (commandstring[i].command == command)
			return commandstring[i].string;
	return "ERROR: no such command";
}
int stringtocommand(char *string) {
	int i;
	for(i = 0; commandstring[i].string; i++)
		if (! strcmp(commandstring[i].string, string))
			return commandstring[i].command;
	return -1;
}
int eventtocommand(Display *dsp, XKeyEvent e) {
	int i;
	for(i = 0; commandstring[i].string; i++)
		if (e.keycode == XKeysymToKeycode(dsp, commandstring[i].keysym)
		    && e.state == commandstring[i].modifier)
			return commandstring[i].command;
	return -1;
}

/*
 * increase or decrease with module
 */
#define MODULEINCREASE(n, mod, rel) n = ((n) + (mod) + (rel)) % (mod)

/*
 * ICCCM atoms
 */
Atom wm_state, wm_protocols, wm_delete_window;

/*
 * the lirc client
 */
#ifndef LIRC
int lirc(Window __attribute__((unused)) root,
         Atom __attribute__((unused)) irwm) {
	return EXIT_FAILURE;
}
#else
int lirc(Window root, Atom irwm, char *lircrc) {
	char *displayname;
	Display *dsp;
	struct lirc_config *config;
	char *code, *c;
	XEvent message;

	printf("lirc client started: ");
	printf("config file: %s\n", lircrc ? lircrc : "default");

	displayname = getenv("DISPLAY");
	dsp = XOpenDisplay(displayname);
	if (dsp == NULL) {
		printf("cannot open display: %s\n", displayname);
		exit(EXIT_FAILURE);
	}

	if (lirc_init(IRWM, 1) == -1) {
		printf("failed lirc_init\n");
		exit(EXIT_FAILURE);
	}

	if (lirc_readconfig(lircrc, &config, NULL) != 0) {
		printf("failed lirc_readconfig\n");
		exit(EXIT_FAILURE);
	}

	while (lirc_nextcode(&code) == 0) {
		if (code == NULL)
			continue;

		while (lirc_code2char(config, code, &c) == 0 && c != NULL) {
			printf("lirc: %s\n", c);

			message.type = ClientMessage;
			message.xclient.window = root;
			message.xclient.message_type = irwm;
			message.xclient.format = 32;

			message.xclient.data.l[0] = stringtocommand(c);
			message.xclient.data.l[1] = 0;
			message.xclient.data.l[2] = 0;
			message.xclient.data.l[3] = 0;
			message.xclient.data.l[4] = 0;

			XSendEvent(dsp, root, False, KeyPressMask, &message);
			XFlush(dsp);
		}
		free(code);
	}

	lirc_freeconfig(config);
	lirc_deinit();
	XCloseDisplay(dsp);

	printf("lirc client ended\n");
	return EXIT_SUCCESS;
}
#endif

/*
 * signal handler for reaping zombies
 */
int lircclient;
void reaper(int s) {
	int pid;
	printf("signal %d\n", s);
	if (s == SIGCHLD) {
		pid = wait(NULL);
		printf("reaped child %d", pid);
		if (pid == lircclient) {
			printf(" (lirc client)");
			lircclient = -1;
		}
		printf("\n");
	}
}

/*
 * fork an external program
 */
int forkprogram(char *path, char *arg) {
	int pid;
	char **argv;

	printf("forking program %s with argument %s\n", path, arg);
	fflush(stdout);

	if (path == NULL)
		return 0;

	pid = fork();
	if (pid != 0) {
		printf("pid=%d\n", pid);
		return pid;
	}

	argv = malloc(sizeof(char *) * (arg == NULL ? 2 : 3));
	argv[0] = path;
	argv[1] = arg;
	if (arg != NULL)
		argv[2] = NULL;
	execvp(path, argv);
	perror(path);
	printf("cannot execute %s\n", path);
	exit(EXIT_FAILURE);
}

/*
 * the panels and their contents
 */
#define MAXPANELS 1000
struct {
	Window panel;		/* container for a window */
	Window content;		/* a window created by some program */
	char *name;		/* name of the window */
} panel[MAXPANELS];
int numpanels = 0;
int activepanel = -1;
Bool unmaponleave = False;	/* unmap window when switching to another */

/*
 * print data of a panel
 */
void panelprint(char *type, int pn) {
	printf("PANEL %d %-10.10s ", pn, type);
	printf("panel=0x%lx ", panel[pn].panel);
	printf("content=0x%lx ", panel[pn].content);
	printf("title=%s", panel[pn].name);
	printf("\n");
}

/*
 * index of a panel and/or content (not found: -1)
 */
#define PANEL	(1<<0)
#define CONTENT (1<<1)
int panelfind(Window p, int panelorcontent) {
	int i;

	for (i = 0; i < numpanels; i++) {
		if (panelorcontent & PANEL && p == panel[i].panel)
			return i;
		if (panelorcontent & CONTENT && p == panel[i].content)
			return i;
	}

	return -1;
}

/*
 * retrieve and store the name of the window in a panel
 */
void panelname(Display *dsp, int pn) {
	XTextProperty t;
	
	if (! XGetWMName(dsp, panel[pn].content, &t)) {
		printf("no name for window 0x%lx\n", panel[pn].content);
		panel[pn].name = strdup("NoName");
		return;
	}
	/* FIXME: this implicitely assumes that the title is a string; it
	 * should instead check t.encoding and use XTextPropertyToStringList if
	 * string; see XTextProperty(3) and Xutil.h */
	panel[pn].name = strdup((char *) t.value);
}

/*
 * create a new panel for a window
 */
int paneladd(Display *dsp, Window root, Window win, XWindowAttributes *wa) {
	int e;
	Window p;
	char name[40];

	if (numpanels >= MAXPANELS) {
		printf("IRWM ERROR: too many open panels, ");
		printf("not creating a new one for window 0x%lx\n", win);
		return -1;
	}

	e = panelfind(win, PANEL | CONTENT);
	if (e != -1) {
		printf("IRWM NOTE: window 0x%lx already exists\n", win);
		return e;
	}

	p = XCreateSimpleWindow(dsp, root, wa->x, wa->y, wa->width, wa->height,
			0, 0, WhitePixel(dsp, DefaultScreen(dsp)));
	XSelectInput(dsp, p, SubstructureNotifyMask);
	XReparentWindow(dsp, win, p, 0, 0);

	sprintf(name, "irwm panel #%d", numpanels);
	XStoreName(dsp, p, name);

	panel[numpanels].panel = p;
	panel[numpanels].content = win;
	panel[numpanels].name = NULL;
	panelname(dsp, numpanels);

	panelprint("CREATE", numpanels);

	return numpanels++;
}

/*
 * remove a panel
 */
int panelremove(Display *dsp, int pn) {
	int i;

	panelprint("DESTROY", pn);
	if (pn < 0 || pn >= numpanels)
		return -1;

	XDestroyWindow(dsp, panel[pn].panel);
	free(panel[pn].name);

	for (i = pn + 1; i < numpanels; i++)
		panel[i - 1] = panel[i];

	numpanels--;

	return 0;
}

/*
 * resize the current panel
 */
void panelresize(Display *dsp, XWindowAttributes base) {
	if (activepanel == -1)
		return;
	XMoveResizeWindow(dsp, panel[activepanel].content,
		base.x, base.y, base.width, base.height);
}

/*
 * leave the current panel
 */
void panelleave(Display *dsp) {
	panelprint("LEAVE", activepanel);

	if (! unmaponleave)
		return;

	XUnmapWindow(dsp, panel[activepanel].panel);
	XUnmapWindow(dsp, panel[activepanel].content);

	XDeleteProperty(dsp, panel[activepanel].content, wm_state);
}

/*
 * enter the current panel
 */
void panelenter(Display *dsp) {
	long data[2];

	panelprint("ENTER", activepanel);

	XMapWindow(dsp, panel[activepanel].content);
	XMapWindow(dsp, panel[activepanel].panel);
	XRaiseWindow(dsp, panel[activepanel].panel);

	data[0] = NormalState;
	data[1] = None;
	XChangeProperty(dsp, panel[activepanel].content, wm_state, wm_state,
		32, PropModeReplace, (unsigned char *) data, 2);

	XSetInputFocus(dsp, panel[activepanel].content,
		RevertToParent, CurrentTime);
}

/*
 * switch to next/previous panel
 */
int panelswitch(Display *dsp, int rel) {
	if (activepanel == -1)
		return -1;
	panelleave(dsp);
	MODULEINCREASE(activepanel, numpanels, rel);
	panelenter(dsp);
	return 0;
}

/*
 * the programs in the program list
 */
#define MAXPROGRAMS 100
struct {
	char *title;
	char *program;
} programs[MAXPROGRAMS];
int numprograms = 0;

/*
 * the control windows of draws: the panel list and the program list
 */
typedef struct {
	Window window;
	GC gc;
	int width;
#ifndef XFT
	XFontStruct *font;
#else
	XftDraw *draw;
	XftFont *font;
	XftColor color;
#endif
} ListWindow;

/*
 * draw a string
 */
#define PADDING 2
void drawstring(Display *dsp, ListWindow *lw, int x, int *y, char *s) {
	*y += PADDING + lw->font->ascent;
#ifndef XFT
	XDrawString(dsp, lw->window, lw->gc, x, *y, s, strlen(s));
#else
	dsp = dsp;	/* avoid warning for unused variable */
	XftDrawString8(lw->draw, &lw->color, lw->font, x, *y,
		(unsigned char *) s, strlen(s));
#endif
	*y += lw->font->descent + PADDING;
}

/*
 * draw a separator
 */
void drawseparator(Display *dsp, ListWindow *lw, int *y) {
	*y += PADDING;
	XDrawLine(dsp, lw->window, lw->gc, 0, *y, lw->width, *y);
	*y += PADDING;
}

/*
 * draw an up or down continuation arrow
 */
void drawarrow(Display* dsp, ListWindow *lw, int *y, Bool draw, Bool up) {
	int x1 = lw->width * 1 / 4;
	int x2 = lw->width * 2 / 4;
	int x3 = lw->width * 3 / 4;
	
	*y += PADDING + lw->font->ascent;
	if (draw) {
		XDrawLine(dsp, lw->window, lw->gc,
			up ? x1 : x2, *y, up ? x2 : x3, *y - lw->font->ascent);
		XDrawLine(dsp, lw->window, lw->gc,
			up ? x2 : x1, *y - lw->font->ascent, up ? x3 : x2, *y);
	}
	*y += lw->font->descent + PADDING;
}

/*
 * draw a list with a selected element
 */
#define MARGIN 5
void drawlist(Display *dsp, ListWindow *lw,
		char *title, char *elements[], int selected, char *help[]) {
	int x, y, z, w;
	int start, i;
	Bool stop;

	x = MARGIN;
	y = MARGIN;

	drawstring(dsp, lw, x, &y, title);
	drawseparator(dsp, lw, &y);

	start = selected <= 4 ? 0 : selected - 4;
	stop = False;

	drawarrow(dsp, lw, &y, start > 0, True);

	for (i = start; i < start + 9; i++) {
		if (! stop && ! elements[i])
			stop = True;
		if (stop) {
			drawstring(dsp, lw, x + PADDING, &y, "");
			continue;
		}

		if (i == selected) {
			z = lw->width - 2 * MARGIN;
			w = lw->font->ascent + lw->font->descent + 2 * PADDING;
			XDrawRectangle(dsp, lw->window, lw->gc, x, y, z, w);
		}

		drawstring(dsp, lw, x + PADDING, &y, elements[i]);
	}

	drawarrow(dsp, lw, &y, ! stop && elements[i], False);

	drawseparator(dsp, lw, &y);

	for (i = 0; help[i]; i++)
		drawstring(dsp, lw, x, &y, help[i]);
}

/*
 * draw the panel list window
 */
void drawpanel(Display *dsp, ListWindow *lw, int activepanel) {
	int i;
	char **elements;
	char *help[] = {"enter: ok",
			"escape: ok",
			"c: close window",
			NULL};

	elements = malloc((numpanels + 1) * sizeof(char *));
	for (i = 0; i < numpanels; i++) {
		panelname(dsp, i);
		elements[i] = malloc(100);
		snprintf(elements[i], 100, " %2d - %s ", i, panel[i].name);
	}
	elements[numpanels] = NULL;

	drawlist(dsp, lw, "IRWM: panel list", elements, activepanel, help);

	for (i = 0; i < numpanels; i++)
		free(elements[i]);
	free(elements);
}

/*
 * draw the programs window
 */
void drawprogs(Display *dsp, ListWindow *lw, int selected) {
	int i;
	char **elements;
	char *help[] = {"enter: run",
			"escape: close",
			NULL};

	elements = malloc((numprograms + 1) * sizeof(char *));
	for (i = 0; i < numprograms; i++)
		elements[i] = strdup(programs[i].title);
	elements[numprograms] = NULL;

	drawlist(dsp, lw, "IRWM: programs", elements, selected, help);

	for (i = 0; i < numprograms; i++)
		free(elements[i]);
	free(elements);
}

/*
 * clear the panel list window and raise the list windows, if any is mapped
 */
void raiselists(Display *dsp, ListWindow *panels, ListWindow *progs) {
	XClearArea(dsp, panels->window, 0, 0, 0, 0, True);
	XRaiseWindow(dsp, panels->window);
	XRaiseWindow(dsp, progs->window);
}

/*
 * close a window; called when pressing 'c' in the panel list
 */
void closewindow(Display *dsp, Window win) {
	XEvent message;
	Atom *props;
	int numprops, i;
	Bool delete = False;

	if (XGetWMProtocols(dsp, win, &props, &numprops)) {
		for (i = 0; i < numprops; i++)
			if (props[i] == wm_delete_window) {
				delete = True;
				break;
			}
		XFree(props);
	}

	if (! delete) {
		XKillClient(dsp, win);
		return;
	}

	memset(&message, 0, sizeof(message));
	message.type = ClientMessage;
	message.xclient.window = win;
	message.xclient.message_type = wm_protocols;
	message.xclient.format = 32;
	message.xclient.data.l[0] = wm_delete_window;
	message.xclient.data.l[1] = CurrentTime;
	XSendEvent(dsp, win, False, 0, &message);
}

/*
 * main
 */
int main(int argn, char *argv[]) {
	char *logfile = "irwm.log";
	int lf;

	char *irwmrcname;
	FILE *irwmrc;
	char *lircrc = NULL;
	char line[200], s1[200], s2[200];

	char *displayname = NULL, *fontname = NULL;
	Display *dsp;
	Window root, win;
	XWindowAttributes rwa;
	GC gc;
	XGCValues gcv;
#ifndef XFT
	XFontStruct *font;
#else
	XftFont *font;
#endif
	ListWindow panelwindow, progswindow;
	XWindowChanges wc;
	int listwidth, listheight;
	Atom irwm;
	int pn;
	char *message;
	int i;

	Bool uselirc = False, quitonlastclose = False, singlekey = False;
	Bool run;
	int command;
	Bool showpanel = False, showprogs = False;
	int progselected = 0;
	char *p, *t;

	XEvent evt;
	XMapRequestEvent ermap;
	XConfigureRequestEvent erconfigure;
	XConfigureEvent econfigure;
	XDestroyWindowEvent edestroy;
	XReparentEvent ereparent;
	XClientMessageEvent emessage;
	XKeyEvent ekey;

				/* parse options */

	while (argn - 1 > 0 && argv[1][0] == '-') {
		if (! strcmp(argv[1], "-l"))
			uselirc = True;
		else if (! strcmp(argv[1], "-q"))
			quitonlastclose = True;
		else if (! strcmp(argv[1], "-s"))
			singlekey = True;
		else if (! strcmp(argv[1], "-u"))
			unmaponleave = True;
		else if (! strcmp(argv[1], "-r"))
			unmaponleave = False;
		else if (! strcmp(argv[1], "-display")) {
			if (argn - 1 < 2) {
				printf("error: -display requires value\n");
				exit(EXIT_FAILURE);
			}
			displayname = argv[2];
			argn--;
			argv++;
		}
		else if (! strcmp(argv[1], "-fn")) {
			if (argn - 1 < 2) {
				printf("error: -fn requires value\n");
				exit(EXIT_FAILURE);
			}
			fontname = argv[2];
			argn--;
			argv++;
		}
		else if (! strcmp(argv[1], "-log")) {
			if (argn - 1 < 2) {
				printf("error: -log requires value\n");
				exit(EXIT_FAILURE);
			}
			logfile = argv[2];
			argn--;
			argv++;
		}
		else if (! strcmp(argv[1], "-lircrc")) {
			if (argn - 1 < 2) {
				printf("error: -lircrc requires value\n");
				exit(EXIT_FAILURE);
			}
			lircrc = argv[2];
			argn--;
			argv++;
		}
		else {
			if (! ! strcmp(argv[1], "-h"))
				printf("unrecognized option: %s\n", argv[1]);
			printf("usage:\n");
			printf("\txinit irwm [options]\n");
			printf("\tstartx irwm [options]\n");
			printf("options:\n");
			printf("\t-l\t\t\tuse lirc for input\n");
			printf("\t-q\t\t\tquit when all windows are closed\n");
			printf("\t-r\t\t\tswitch to window by raising it\n");
			printf("\t-u\t\t\tswitch by unmapping previous\n");
			printf("\t-display display\tconnect to server\n");
			printf("\t-fn font\t\tfont used in lists\n");
			printf("\t-logfile file\t\tlog to file\n");
			exit(! strcmp(argv[1], "-h") ?
				EXIT_SUCCESS : EXIT_FAILURE);
		}
		argn--;
		argv++;
	}

				/* log file */
	
	if (! ! strcmp(logfile, "-")) {
		lf = creat(logfile, S_IRUSR | S_IWUSR);
		if (lf == -1)
			perror(logfile);
		else {
			fprintf(stderr, "logging to %s\n", logfile);
			dup2(lf, STDOUT_FILENO);
			dup2(lf, STDERR_FILENO);
		}
	}

				/* open display */

	if (displayname == NULL)
		displayname = getenv("DISPLAY");
	dsp = XOpenDisplay(displayname);
	if (dsp == NULL) {
		printf("cannot open display: %s\n", displayname);
		exit(EXIT_FAILURE);
	}

				/* root window */

	root = DefaultRootWindow(dsp);
	XGetWindowAttributes(dsp, root, &rwa);
	printf("root: 0x%lx (%dx%d)\n", root, rwa.width, rwa.height);

	XSelectInput(dsp, root,
		SubstructureRedirectMask |
		SubstructureNotifyMask |
		KeyPressMask);

				/* configuration file */

	signal(SIGCHLD, reaper);

	irwmrcname = malloc(strlen(getenv("HOME")) + 20);
	sprintf(irwmrcname, "%s/.irwmrc", getenv("HOME"));
	irwmrc = fopen(irwmrcname, "r");
	free(irwmrcname);
	if (irwmrc == NULL)
		irwmrc = fopen("/etc/irwmrc", "r");
	if (irwmrc == NULL) {
		printf("WARNING: cannot read /etc/irwmrc or .irwmrc\n");

		programs[0].title = "xterm";
		programs[0].program = "/usr/bin/xterm";
		programs[1].title = "quit";
		programs[1].program = NULL;
		programs[2].title = NULL;
		numprograms = 2;

		forkprogram(programs[0].program, NULL);
	}
	else {
		numprograms = 0;
		while (fgets(line, 100, irwmrc)) {
			if (1 == sscanf(line, "echo %[^\n]", s1))
				printf("%s\n", s1);
			else if (1 == sscanf(line, "font %s", s1)) {
				if (fontname == NULL)
					fontname = strdup(s1);
			}
			else if (1 == sscanf(line, "startup %s", s1))
				forkprogram(s1, NULL);
			else if (2 == sscanf(line, "program %s %s", s1, s2)) {
				programs[numprograms].title = strdup(s1);
				programs[numprograms].program = strdup(s2);
				numprograms++;
			}
			else if (1 == sscanf(line, "program %s", s1)) {
				programs[numprograms].title = strdup(s1);
				programs[numprograms].program = NULL;
				numprograms++;
			}
			else if (line[0] != '\n' && line[0] != '#')
				printf("ERROR in irwmrc: %s", line);
			if (numprograms >= MAXPROGRAMS) {
				printf("ERROR in irwmrc: too many programs\n");
				numprograms--;
			}
		}
		fclose(irwmrc);
	}

				/* graphic context, font and list size */

	gcv.line_width = 2;
#ifndef XFT
	gcv.font = XLoadFont(dsp, fontname == NULL ? FONT : fontname);
	gc = XCreateGC(dsp, root, GCLineWidth | GCFont, &gcv);
	font = XQueryFont(dsp, gcv.font);
#else
	gc = XCreateGC(dsp, root, GCLineWidth, &gcv);
	font = XftFontOpenName(dsp, 0, fontname == NULL ? XFTFONT : fontname);
#endif

	listwidth = rwa.width / 4;
	listheight = 15 * (font->ascent + font->descent + PADDING * 2) +
		PADDING * 2 * 2 + MARGIN * 2;

				/* panel list window */
	
	panelwindow.window = XCreateSimpleWindow(dsp, root,
		rwa.width / 2, rwa.height / 2 - listheight / 2,
		listwidth, listheight,
		2, BlackPixel(dsp, 0), WhitePixel(dsp, 0));
	printf("panel list window: 0x%lx\n", panelwindow.window);
	XStoreName(dsp, panelwindow.window, "irwm panel window");
	XSelectInput(dsp, panelwindow.window, ExposureMask);

	panelwindow.gc = gc;
	panelwindow.font = font;
	panelwindow.width = listwidth;
#ifdef XFT
	panelwindow.draw = XftDrawCreate(dsp, panelwindow.window,
		rwa.visual, rwa.colormap);
	XftColorAllocName(dsp, rwa.visual, rwa.colormap,
		"black", &panelwindow.color);
#endif

				/* program list window */

	progswindow.window = XCreateSimpleWindow(dsp, root,
		rwa.width / 4, rwa.height / 2 - listheight / 2,
		listwidth, listheight,
		2, BlackPixel(dsp, 0), WhitePixel(dsp, 0));
	printf("program list window: 0x%lx\n", progswindow.window);
	XStoreName(dsp, progswindow.window, "irwm progs window");
	XSelectInput(dsp, progswindow.window, ExposureMask);

	progswindow.gc = gc;
	progswindow.font = font;
	progswindow.width = listwidth;
#ifdef XFT
	progswindow.draw = XftDrawCreate(dsp, progswindow.window,
		rwa.visual, rwa.colormap);
	progswindow.color = panelwindow.color;
#endif

				/* atoms */

	irwm = XInternAtom(dsp, IRWM, False);
	wm_state = XInternAtom(dsp, "WM_STATE", False);
	wm_protocols = XInternAtom(dsp, "WM_PROTOCOLS", False);
	wm_delete_window = XInternAtom(dsp, "WM_DELETE_WINDOW", False);

				/* lirc client */

	if (! uselirc) {
		printf("no lirc client, pass -l to enable\n");
		lircclient = -1;
	}
	else {
		printf("forking the lirc client, ");
		lircclient = fork();
		if (lircclient == 0)
			return lirc(root, irwm, lircrc);
		printf("pid=%d\n", lircclient);
	}

				/* move pointer (for small windows) */

	XWarpPointer(dsp, None, root, 0, 0, 0, 0, 10, 10);

				/* grab keys */

	for (i = 1; ! ! strcmp(commandstring[i].string, "ENDGRAB"); i++)
		XGrabKey(dsp,
			XKeysymToKeycode(dsp, commandstring[i].keysym),
			commandstring[i].modifier,
			root, False, GrabModeAsync, GrabModeAsync);

				/* main loop */

	for (run = True; run; ) {

				/* get X event */

		XNextEvent(dsp, &evt);

		command = NOCOMMAND;

		switch(evt.type) {

				/* substructure redirect events */

		case MapRequest:
			printf("MapRequest\n");
			ermap = evt.xmaprequest;
			printf("\t0x%lx", ermap.window);
			printf(" 0x%lx", ermap.parent);
			if (XGetTransientForHint(dsp, ermap.window, &win))
				printf("transient_for=%lx\n", win);
			printf("\n");

			pn = paneladd(dsp, root, ermap.window, &rwa);
			if (pn == -1)
				break;

			if (activepanel != -1)
				panelleave(dsp);
			activepanel = pn;
			panelresize(dsp, rwa);
			panelenter(dsp);
			raiselists(dsp, &panelwindow, &progswindow);
			break;
		case ConfigureRequest:
			printf("ConfigureRequest\n");
			erconfigure = evt.xconfigurerequest;
			printf("\t0x%lx ", erconfigure.window);
			printf("x=%d y=%d ", erconfigure.x, erconfigure.y);
			printf("width=%d ", erconfigure.width);
			printf("height=%d ", erconfigure.height);
			printf("border_width=%d ", erconfigure.border_width);
			printf("above=0x%lx ", erconfigure.above);
			printf("\n");

			if (panelfind(erconfigure.window, PANEL) != -1 ||
			    panelfind(erconfigure.window, CONTENT) != -1) {
				panelresize(dsp, rwa);
				break;
			}

			wc.x = erconfigure.x;
			wc.y = erconfigure.y;
			wc.width = erconfigure.width;
			wc.height = erconfigure.height;
			wc.border_width = erconfigure.border_width;
			wc.sibling = None;
			wc.stack_mode = Above;
			XConfigureWindow(dsp, erconfigure.window,
				erconfigure.value_mask & ~ CWStackMode, &wc);
			break;
		case CirculateRequest:
			printf("CirculateRequest\n");
			break;

					/* substructure notify events */

		case CirculateNotify:
			printf("CirculateNotify\n");
			break;
		case ConfigureNotify:
			printf("ConfigureNotify\n");
			econfigure = evt.xconfigure;
			printf("\t0x%lx ", econfigure.window);
			printf("x=%d y=%d ", econfigure.x, econfigure.y);
			printf("width=%d ", econfigure.width);
			printf("height=%d ", econfigure.height);
			printf("border_width=%d ", econfigure.border_width);
			printf("above=0x%lx ", econfigure.above);
			printf("\n");
			break;
		case CreateNotify:
			printf("CreateNotify\n");
			printf("\t0x%lx ", evt.xcreatewindow.window);
			printf("parent=0x%lx", evt.xcreatewindow.parent);
			if (evt.xcreatewindow.override_redirect)
				printf(" override_redirect");
			printf("\n");
			break;
		case DestroyNotify:
			printf("DestroyNotify\n");
			edestroy = evt.xdestroywindow;
			printf("\t0x%lx ", edestroy.window);
			printf("0x%lx", edestroy.event);
			printf("\n");

			pn = panelfind(edestroy.event, PANEL);
			if (pn == -1)
				break;

			panelremove(dsp, pn);

			if (numpanels > 0) {
				if (activepanel > pn)
					activepanel--;
				else if (activepanel == pn) {
					if (activepanel >= numpanels)
						activepanel = numpanels - 1;
					panelenter(dsp);
				}
			}
			else if (quitonlastclose) {
				run = False;
				break;
			}
			else {
				activepanel = -1;
				XSetInputFocus(dsp, root,
					RevertToParent, CurrentTime);
				printf("to quit on last close, pass -q\n");
			}

			raiselists(dsp, &panelwindow, &progswindow);
			break;
		case GravityNotify:
			printf("GravityNotify\n");
			break;
		case MapNotify:
			printf("MapNotify\n");
			printf("\t0x%lx", evt.xmap.window);
			printf(" 0x%lx", evt.xmap.event);
			printf("\n");
			break;
		case ReparentNotify:
			printf("ReparentNotify\n");
			ereparent = evt.xreparent;
			printf("\t0x%lx reparented ", ereparent.window);
			if (ereparent.event != ereparent.parent)
				printf("away from 0x%lx, ", ereparent.event);
			printf("to 0x%lx\n", ereparent.parent);
			break;
		case UnmapNotify:
			printf("UnmapNotify\n");
			printf("\t0x%lx", evt.xunmap.window);
			printf("\n");
			break;
		case ClientMessage:
			printf("ClientMessage\n");
			emessage = evt.xclient;
			printf("\t0x%lx",  emessage.window);
			message = XGetAtomName(dsp, emessage.message_type);
			printf(" %-20s ", message);
			XFree(message);
			printf("%d\n", emessage.format);
			printf("\t\tdata: ");
			switch (emessage.format) {
			case 8:
				for (i = 0; i < 20; i++)
					printf(" %d", emessage.data.b[i]);
				break;
			case 16:
				for (i = 0; i < 10; i++)
					printf(" %d", emessage.data.s[i]);
				break;
			case 32:
				for (i = 0; i < 5; i++)
					printf(" %ld", emessage.data.l[i]);
				break;
			}
			printf("\n");

			if (emessage.message_type == irwm &&
			    emessage.format == 32) {
				command = emessage.data.l[0];
				break;
			}

			break;

					/* keypress events */

		case KeyPress:
			printf("KeyPress\n");
			ekey = evt.xkey;
			printf("\t0x%lx ", ekey.subwindow);
			printf("key=%d state=%d", ekey.keycode, ekey.state);
			printf("\n");

			command = eventtocommand(dsp, ekey);
			if (command == -1)
				command = NOCOMMAND;
			break;
		case KeyRelease:
			printf("KeyRelease\n");
			ekey = evt.xkey;
			printf("\t0x%lx ", ekey.subwindow);
			printf("key=%d state=%d", ekey.keycode, ekey.state);
			printf("\n");
			break;

					/* other events */

		case Expose:
			if (evt.xexpose.window == panelwindow.window)
				drawpanel(dsp, &panelwindow, activepanel);
			if (evt.xexpose.window == progswindow.window)
				drawprogs(dsp, &progswindow, progselected);
			break;

		case MappingNotify:
			printf("MappingNotify\n");
			printf("\t%d", evt.xmapping.request);
			printf(" %d", evt.xmapping.first_keycode);
			printf(" %d", evt.xmapping.count);
			printf("\n");
			break;

		default:
			printf("Unexpected event, type=%d\n", evt.type);
		}

					/* execute command */

		if (command != NOCOMMAND)
			printf("COMMAND %s\n", commandtostring(command));

		if (command == PANELWINDOW && showpanel)
			command = singlekey ? PROGSWINDOW : HIDEWINDOW;
		if (command == PANELWINDOW && showprogs && singlekey)
			command = HIDEWINDOW;
		if (command == PROGSWINDOW && showprogs)
			command = HIDEWINDOW;

		switch (command) {
		case NOCOMMAND:
			break;
		case NEXTPANEL:
		case PREVPANEL:
			panelswitch(dsp, command == PREVPANEL ? -1 : 1);
			raiselists(dsp, &panelwindow, &progswindow);
			break;
		case QUIT:
			run = False;
			break;

		case PANELWINDOW:
			showpanel = True;
			XMapRaised(dsp, panelwindow.window);
			showprogs = False;
			XUnmapWindow(dsp, progswindow.window);
			XGrabKeyboard(dsp, root, False,
				GrabModeAsync, GrabModeAsync, CurrentTime);
			break;
		case PROGSWINDOW:
			showprogs = True;
			XMapRaised(dsp, progswindow.window);
			showpanel = False;
			XUnmapWindow(dsp, panelwindow.window);
			XGrabKeyboard(dsp, root, False,
				GrabModeAsync, GrabModeAsync, CurrentTime);
			break;

		case UPWINDOW:
		case DOWNWINDOW:
			if (showpanel) {
				panelswitch(dsp, command == UPWINDOW ? -1 : 1);
				XClearArea(dsp, panelwindow.window,
					0, 0, 0, 0, True);
				XRaiseWindow(dsp, panelwindow.window);
			}
			if (showprogs) {
				MODULEINCREASE(progselected, numprograms,
					(command == UPWINDOW ? -1 : 1));
				XClearArea(dsp, progswindow.window,
					0, 0, 0, 0, True);
			}
			break;
		case HIDEWINDOW:
		case OKWINDOW:
			if (showpanel) {
				showpanel = False;
				XUnmapWindow(dsp, panelwindow.window);
				XUngrabKeyboard(dsp, CurrentTime);
			}
			if (showprogs) {
				showprogs = False;
				XUnmapWindow(dsp, progswindow.window);
				XUngrabKeyboard(dsp, CurrentTime);
				if (command == OKWINDOW) {
					p = programs[progselected].program;
					t = programs[progselected].title;
					if (p)
						forkprogram(p, NULL);
					else if (! strcmp(t, "resize"))
						panelresize(dsp, rwa);
					else if (! strcmp(t, "quit"))
						run = False;
				}
			}
			break;
		case KOWINDOW:
			if (showpanel && activepanel != -1)
				closewindow(dsp, panel[activepanel].content);
			break;
		}

		fflush(NULL);
	}

				/* close wm */

	if (lircclient == -1)
		printf("no lirc client to kill\n");
	else {
		printf("killing lirc client, pid=%d\n", lircclient);
		kill(lircclient, SIGTERM);
	}
	for (i = 0; i < numpanels; i++)
		closewindow(dsp, panel[i].content);
	XCloseDisplay(dsp);
	printf("irwm ended\n");

	return EXIT_SUCCESS;
}

