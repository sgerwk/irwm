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
 *   ctrl-shift-l	print panels in the log file
 *   ctrl-shift-tab	quit
 *
 * lirc, or ClientMessage of message_type "IRWM" to the root window:
 *
 *   NOCOMMAND		nop
 *   NEXTPANEL		switch to next panel
 *   PREVPANEL		switch to previous panel
 *   RESTART		restart irwm
 *   QUIT		quit irwm
 *   LOGLIST		print the list of panels in the log file
 *   POSITIONFIX	toggle fixing the position of override windows
 *   RESIZE		resize the current panel
 *
 *   PANELWINDOW	show/hide the panel list window
 *   PROGSWINDOW	show/hide the program list window
 *   CONFIRMWINDOW	show/hide the quit confirm dialog
 *
 *   UPWINDOW		up in the window
 *   DOWNWINDOW		down in the window
 *   HIDEWINDOW		hide window
 *   OKWINDOW		select the current item in the window
 *   KOWINDOW		only in the panel list window: close the current panel
 *   ENDWINDOW		only in the panel list window: move panel at end
 *
 *   NUMWINDOW(n)	select entry n in the list
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

/*
 * ui:
 *
 * the list of programs that can be run, the list of currently active panels
 * and confirmation for exit have each its own window; they are respectively
 * drawn by drawprogs(), drawpanel() and drawconfirm(); they all call
 * drawlist() and are called on Expose events on their windows
 *
 * these windows are always present and are always both raised by raiselists()
 * when something changes (map, unmap and destroy event), but are only mapped
 * in response to the commands PROGSWINDOW, PANELWINDOW and QUIT, or when
 * "quit" is selected in the program list; the Boolean variables showprogs,
 * showpanels and showconfirm store whether they are mapped; they are unmapped
 * on commands OKWINDOW and HIDEWINDOW or when another is mapped
 */

/*
 * override_redirect windows
 *
 * windows with the override_redirect flag usually stay over the regular
 * windows; irwm does not reparent them to a panel, but keeps a list of them
 * and raises them every time it enters a panel
 *
 * a typical use of override windows is to implement menus; programs often
 * misplace them too close to the screen border, where parts of them fall
 * outside the screen; irwm moves them inside the screen; the POSITIONFIX
 * command toggles this behaviour, which is disabled by default
 *
 * a special case are override windows that are larger or taller than the
 * screen; irwm moves them in a random position that makes them fill the width
 * or the height of the screen, with a preference to aligning them to the
 * horizontal or vertical borders; opening such a menu multiple times allows
 * eventually accessing all its items; the target position is saved to avoid
 * moving them again
 */

/*
 * the error handler
 *
 * regular programs should not make wrong requests, negating the need for an
 * error handler; this is not the case with window managers and other programs
 * that need to operate on windows that are outside their control
 *
 * the programs may close their windows at any time, but the window manager
 * receives notifications one at time; dealing with a closure may require
 * entering the panel of another window that is already closed, but its closure
 * has not yet been notified to the window manager; entering a panel requires
 * operating on its window, firing a string of errors from the X server
 *
 * a common case are exit confirmation dialogs; for example, gedit asks for
 * confirmation before exiting when the file has not been saved; clicking on
 * the "don't save" button makes it close both the dialog and its main window;
 * when the closure of the dialog is notified to irwm, it removes its panel and
 * enters the panel under it; this is often the main gedit window, which is
 * already closed
 *
 * the error handler is also needed because irwm prints the name of atoms it
 * receives as window manager hints; a program may send arbitrary integers, not
 * representing any atom; the X server send back an error when asked for their
 * strings; the default error handler would terminate the window manager in
 * such cases
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
#include <X11/Xproto.h>
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
#define NOCOMMAND      0	/* no command */
#define NEXTPANEL      1	/* switch to next panel */
#define PREVPANEL      2	/* switch to previous panel */
#define RESTART        3	/* restart irwm */
#define QUIT           4	/* quit irwm */
#define LOGLIST        5	/* print panels in the log file */
#define POSITIONFIX    6	/* toggle: fix position of override windows */
#define RESIZE         7	/* resize the current panel */

#define PANELWINDOW   10	/* show the panel list window */
#define PROGSWINDOW   11	/* show the programs window */
#define CONFIRMWINDOW 12	/* show the quit confirm dialog */

#define UPWINDOW      20	/* up in the window */
#define DOWNWINDOW    21	/* down in the window */
#define HIDEWINDOW    22	/* hide both windows */
#define OKWINDOW      23	/* select the current item in the window */
#define KOWINDOW      24	/* close currently selected panel */
#define ENDWINDOW     25	/* move currently active panel at the end */

#define NUMWINDOW(n)  (100 + (n))	/* select entry n in the window */

/*
 * commands, their names and keystrokes
 */
struct {
	int command;	char *string;	int keysym;	unsigned modifier;
} commandstring[] = {
	{NOCOMMAND,	"NOCOMMAND",	XK_VoidSymbol,	0},
	{NEXTPANEL,	"NEXTPANEL",	XK_Right,	Mod1Mask},
	{PREVPANEL,	"PREVPANEL",	XK_Left,	Mod1Mask},
	{RESTART,	"RESTART", XK_Tab, ControlMask | ShiftMask | Mod1Mask},
	{QUIT,		"QUIT",		XK_Tab,	ControlMask | ShiftMask},
	{LOGLIST,	"LOGLIST",	XK_l,	ControlMask | ShiftMask},
	{PANELWINDOW,	"PANELWINDOW",	XK_Tab,		Mod1Mask},
	{PROGSWINDOW,	"PROGSWINDOW",	XK_Tab,		ControlMask},
	{-1,		"ENDGRAB",	XK_VoidSymbol,	0},
	{RESIZE,	"RESIZE",	XK_VoidSymbol,  0},
	{POSITIONFIX,	"POSITIONFIX",	XK_VoidSymbol,	0},
	{CONFIRMWINDOW, "CONFIRMWINDOW", XK_VoidSymbol, 0},
	{UPWINDOW,	"UPWINDOW",	XK_Up,		0},
	{DOWNWINDOW,	"DOWNWINDOW",	XK_Down,	0},
	{HIDEWINDOW,	"HIDEWINDOW",	XK_Escape,	0},
	{OKWINDOW,	"OKWINDOW",	XK_Return,	0},
	{KOWINDOW,	"KOWINDOW",	XK_c,		0},
	{ENDWINDOW,	"ENDWINDOW",	XK_e,		0},
	{NUMWINDOW(1),	"NUMWINDOW(1)",	XK_1,		0},
	{NUMWINDOW(2),	"NUMWINDOW(2)",	XK_2,		0},
	{NUMWINDOW(3),	"NUMWINDOW(3)",	XK_3,		0},
	{NUMWINDOW(4),	"NUMWINDOW(4)",	XK_4,		0},
	{NUMWINDOW(5),	"NUMWINDOW(5)",	XK_5,		0},
	{NUMWINDOW(6),	"NUMWINDOW(6)",	XK_6,		0},
	{NUMWINDOW(7),	"NUMWINDOW(7)",	XK_7,		0},
	{NUMWINDOW(8),	"NUMWINDOW(8)",	XK_8,		0},
	{NUMWINDOW(9),	"NUMWINDOW(9)",	XK_9,		0},
	{-1,		NULL,		XK_VoidSymbol,	0},
	{-1,		NULL,		XK_VoidSymbol,  0}
};
char *commandtostring(int command) {
	int i;
	for(i = 0; commandstring[i].string; i++)
		if (commandstring[i].command == command)
			return commandstring[i].string;
	if (command >= NUMWINDOW(0)) {
		if (commandstring[i + 1].string == NULL)
			commandstring[i + 1].string = malloc(100);
		sprintf(commandstring[i + 1].string, "NUMWINDOW(%d)", command);
		return commandstring[i + 1].string;
	}
	return "ERROR: no such command";
}
int stringtocommand(char *string) {
	int i;
	char par;
	for(i = 0; commandstring[i].string; i++)
		if (! strcmp(commandstring[i].string, string))
			return commandstring[i].command;
	if (sscanf(string, "NUMWINDOW(%d%c", &i, &par) == 2 &&
	    i >=0 && par == ')')
		return NUMWINDOW(i);
	return -1;
}
int eventtocommand(Display *dsp, XKeyEvent e, KeySym *list) {
	int i;
	for(i = 0; commandstring[i].string; i++)
		if (e.keycode == XKeysymToKeycode(dsp, commandstring[i].keysym)
		    && e.state == commandstring[i].modifier)
			return commandstring[i].command;
	if (list == NULL)
		return NOCOMMAND;
	for (list = list, i = 0; *list != XK_VoidSymbol; list++, i++)
		if (e.keycode == XKeysymToKeycode(dsp, *list))
			return NUMWINDOW(i + 1);
	return NOCOMMAND;
}

/*
 * increase or decrease with module
 */
#define MODULEINCREASE(n, mod, rel) n = ((n) + (mod) + (rel)) % (mod)

/*
 * ICCCM atoms
 */
Atom wm_protocols, wm_state, wm_delete_window;
Atom net_supported;
Atom net_client_list, net_client_list_stacking, net_active_window;

/*
 * error handler
 */
#define Error 0
#define Reply 1
int handler(Display *d, XErrorEvent *e) {
	printf("error handler called\n");
	XPutBackEvent(d, (XEvent *) e);
	return 0;
}

/*
 * the lirc client
 */
#ifndef LIRC
int lirc(Window root, Atom irwm, char *lircrc) {
	(void) root;
	(void) irwm;
	(void) lircrc;
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
 * override_redirect windows
 */
#define MAXOVERRIDE 1000
struct {
	Window win;
	int nx, ny;
	Bool ontop;
} override[MAXOVERRIDE];
int numoverride = 0;
Bool raiseoverride = True;
#define UNMOVED (-10000)

/*
 * print an override window
 */
void overrideprint(char *type, int i) {
	printf("OVERRIDE %d %-10.10s 0x%lx", i, type, override[i].win);
	if (override[i].nx != UNMOVED || override[i].ny != UNMOVED)
		printf("%d,%d", override[i].nx, override[i].ny);
	printf("\n");
}

/*
 * check whether a window is in the list of the override windows
 */
int overrideexists(Window win) {
	int i;
	for (i = 0; i < numoverride; i++)
		if (win == override[i].win)
			return i;
	return -1;
}

/*
 * add an override window
 */
void overrideadd(Window win) {
	if (numoverride >= MAXOVERRIDE) {
		printf("WARNING: too many override_redirect windows\n");
		return;
	}
	override[numoverride].win = win;
	override[numoverride].nx = UNMOVED;
	override[numoverride].ny = UNMOVED;
	override[numoverride].ontop = False;
	overrideprint("ADD", numoverride);
	numoverride++;
}

/*
 * remove an override window
 */
void overrideremove(Window win) {
	int i;
	for (i = 0; i < numoverride; i++)
		if (override[i].win == win) {
			overrideprint("REMOVE", i);
			numoverride--;
			override[i] = override[numoverride];
			return;
		}
}

/*
 * raise all override windows
 */
void overrideraise(Display *dsp) {
	int i;
	if (! raiseoverride)
		return;
	for (i = 0; i < numoverride; i++)
		if (! override[i].ontop) {
			overrideprint("RAISE", i);
			XRaiseWindow(dsp, override[i].win);
		}
	for (i = 0; i < numoverride; i++)
		if (override[i].ontop) {
			overrideprint("RAISE", i);
			XRaiseWindow(dsp, override[i].win);
		}
}

/*
 * fix placement of an override windows
 */
int randombetween(int d, int c, int rc) {
	if (c >= rc && c <= rc + d)
		return c;
	if (rand() % 3 == 0)
		return rc;
	if (rand() % 3 == 0)
		return rc + d;
	return rc + (rand() % (d + (d < 0 ? -1 : 1)) + (d < 0 ? d : 0));
}
void overrideplace(Display *dsp, Window win, XWindowAttributes *rwa) {
	XWindowAttributes wa;
	int i;
	int d;
	for (i = 0; i < numoverride; i++) {
		if (override[i].win == win) {
			XGetWindowAttributes(dsp, win, &wa);
			if (override[i].nx == wa.x && override[i].ny == wa.y)
				return;

			d = rwa->width - wa.width - 2 * wa.border_width;
			override[i].nx = randombetween(d, wa.x, rwa->x);
			d = rwa->height - wa.height - 2 * wa.border_width;
			override[i].ny = randombetween(d, wa.y, rwa->y);

			if (override[i].nx == wa.x && override[i].ny == wa.y)
				return;
			XMoveWindow(dsp, win, override[i].nx, override[i].ny);
			overrideprint("MOVE", i);
			printf("\tmoved to %d,%d\n",
				override[i].nx, override[i].ny);
			return;
		}
	}
}

/*
 * the panels and their contents
 */
#define MAXPANELS 1000
struct panel {
	Window panel;		/* container for a window */
	Window content;		/* a window created by some program */
	char *name;		/* name of the window */
	Window leader;		/* group leader, or None */
	Bool withdrawn;		/* content is withdrawn by program */
} panel[MAXPANELS];
int numpanels = 0;
int numactive = 0;
int activepanel = -1;
Window activecontent = None;
Bool unmaponleave = False;	/* unmap window when switching to another */
Window activewindow = None;	/* may not be the content of a panel */

/*
 * print data of a panel
 */
void panelprint(char *type, int pn) {
	printf("PANEL %d %-10.10s ", pn, type);
	printf("%s ", pn == activepanel ? "*" : " ");
	printf("%s ", activecontent == panel[pn].content ? "=" : " ");
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
int paneladd(Display *dsp, Window root, Window win, XWindowAttributes *wa,
		Window leader) {
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
	panel[numpanels].leader = leader;
	panel[numpanels].withdrawn = False;

	panelprint("CREATE", numpanels);

	numactive++;
	return numpanels++;
}

/*
 * remove a panel
 */
int panelremove(Display *dsp, int pn, int destroy) {
	int i, j, n;
	Window c;

	panelprint("REMOVE", pn);
	if (pn < 0 || pn >= numpanels)
		return -1;
	c = panel[pn].content;
	if (c == activecontent) {
		activecontent = None;
		printf("ACTIVECONTENT 0x%lx\n", activecontent);
	}

	j = 0;
	n = numpanels;
	for (i = 0; i < n; i++) {
		if (i == pn || panel[i].leader == c) {
			if (! panel[i].withdrawn)
				numactive--;
			if (destroy) {
				panelprint("DESTROY", i);
				free(panel[i].name);
				XDestroyWindow(dsp, panel[i].panel);
				numpanels--;
			}
			else if (! panel[i].withdrawn) {
				panelprint("WITHDRAW", i);
				XUnmapWindow(dsp, panel[i].panel);
				panel[i].withdrawn = True;
			}
			if (activepanel == j && numactive > 0) {
				do {
					MODULEINCREASE(activepanel,
						numpanels, -1);
				}
				while (panel[activepanel].withdrawn);
			}
			if (destroy) {
				if (activepanel > j)
					activepanel--;
				continue;
			}
		}
		if (j != i)
			panel[j] = panel[i];
		j++;
	}

	if (numactive == 0)
		activepanel = -1;

	return 0;
}

/*
 * swap panels
 */
int panelswap(int pn1, int pn2) {
	struct panel temp;

	if (pn1 == -1 || pn1 > numpanels - 2)
		return -1;
	if (pn2 == -1 || pn2 > numpanels - 1)
		return -1;

	temp = panel[pn2];
	panel[pn2] = panel[pn1];
	panel[pn1] = temp;

	return 0;
}

/*
 * resize a panel
 */
void panelresize(Display *dsp, int pn, XWindowAttributes base) {
	if (activepanel == -1)
		return;
	panelprint("RESIZE", pn);
	XSetWindowBorderWidth(dsp, panel[pn].content, 0);
	XMoveResizeWindow(dsp, panel[pn].content,
		0, 0, base.width, base.height);
}

/*
 * leave the current panel
 */
void panelleave(Display *dsp) {
	if (activepanel == -1)
		return;

	panelprint("LEAVE", activepanel);

	if (! unmaponleave)
		return;

	XUnmapWindow(dsp, panel[activepanel].panel);
	XUnmapWindow(dsp, panel[activepanel].content);

	XDeleteProperty(dsp, panel[activepanel].content, wm_state);
}

/*
 * update the lists of managed windows
 */
void clientlistupdate(Display *dsp, Window root) {
	Window *list, *slist;
	int i;

	XChangeProperty(dsp, root, net_active_window,
		XA_WINDOW, 32, PropModeReplace,
		(unsigned char *) &activewindow, 1);

	list = malloc(numpanels * sizeof(Window));
	slist = malloc(numpanels * sizeof(Window));
	for (i = 0; i < numpanels; i++) {
		list[i] = panel[i].content;
		slist[i] = panel[(activepanel + 1 + i) % numpanels].content;
	}
	XChangeProperty(dsp, root, net_client_list,
		XA_WINDOW, 32, PropModeReplace,
		(unsigned char *) list, numpanels);
	XChangeProperty(dsp, root, net_client_list_stacking,
		XA_WINDOW, 32, PropModeReplace,
		(unsigned char *) slist, numpanels);
	free(list);
	free(slist);
}

/*
 * enter the current panel
 */
void panelenter(Display *dsp, Window root) {
	long data[2];

	if (activepanel == -1) {
		activecontent = None;
		printf("ACTIVECONTENT 0x%lx\n", activecontent);
		clientlistupdate(dsp, root);
		return;
	}

	panelprint("ENTER", activepanel);

	if (activepanel >= numpanels) {
		printf("WARNING: activepanel=%d not less than numpanels=%d\n",
			activepanel, numpanels);
		return;
	}

	if (panel[activepanel].withdrawn) {
		panelprint("RESTORE", activepanel);
		panel[activepanel].withdrawn = False;
		numactive++;
	}

	if (activecontent == panel[activepanel].content) {
		printf("NOTE: active content already active\n");
		return;
	}

	activecontent = panel[activepanel].content;
	printf("ACTIVECONTENT 0x%lx\n", activecontent);
	activewindow = panel[activepanel].content;
	printf("ACTIVEWINDOW 0x%lx\n", activewindow);
	clientlistupdate(dsp, root);

	XMapWindow(dsp, panel[activepanel].content);
	XMapWindow(dsp, panel[activepanel].panel);
	XRaiseWindow(dsp, panel[activepanel].panel);
	overrideraise(dsp);

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
int panelswitch(Display *dsp, Window root, int rel) {
	if (activepanel == -1)
		return -1;
	panelleave(dsp);
	do {
		MODULEINCREASE(activepanel, numpanels, rel);
	} while (panel[activepanel].withdrawn);
	panelenter(dsp, root);
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
 * the panel, the program and the confirmation list
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
	XPoint ps[4];

	*y += PADDING + lw->font->ascent;
	if (draw) {
		ps[0].x = x1;
		ps[0].y = *y - (up ? 0 : lw->font->ascent);
		ps[1].x = x2;
		ps[1].y = *y - (up ? lw->font->ascent : 0);
		ps[2].x = x3;
		ps[2].y = ps[0].y;
		ps[3] = ps[0];
		XDrawLines(dsp, lw->window, lw->gc, ps, 4, CoordModeOrigin);
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
	char buf[100];

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

		snprintf(buf, 100, "%2d %s", i + 1, elements[i]);
		drawstring(dsp, lw, x + PADDING, &y, buf);
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
	int i, j, a;
	char **elements;
	char *help[] = {"enter: ok",
			"escape: ok",
			"c: close window",
			"e: move window at end",
			NULL};

	elements = malloc((numactive + 1) * sizeof(char *));
	a = 0;
	j = 0;
	for (i = 0; i < numpanels; i++) {
		if (panel[i].withdrawn)
			continue;
		if (i == activepanel)
			a = j;
		panelname(dsp, i);
		elements[j] = panel[i].name;
		j++;
	}
	elements[numactive] = NULL;

	drawlist(dsp, lw, IRWM ": panel list", elements, a, help);
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

	drawlist(dsp, lw, IRWM ": programs", elements, selected, help);

	for (i = 0; i < numprograms; i++)
		free(elements[i]);
	free(elements);
}

/*
 * draw the confirmation window
 */
void drawconfirm(Display *dsp, ListWindow *cw, int selected) {
	char *elements[] = {"yes", "no", NULL};
	char *help[] = {NULL};
	drawlist(dsp, cw, IRWM ": confirm quit", elements, selected, help);
}

/*
 * clear the panel list window and raise the list windows, if any is mapped
 */
void raiselists(Display *dsp,
		ListWindow *panels, ListWindow *confirm, ListWindow *progs) {
	XClearArea(dsp, panels->window, 0, 0, 0, 0, True);
	XRaiseWindow(dsp, panels->window);
	XRaiseWindow(dsp, confirm->window);
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
		printf("xkillclient 0x%lx\n", win);
		XKillClient(dsp, win);
		return;
	}

	printf("wm_delete_window message to 0x%lx\n", win);
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
	char **cargv;
	char *logfile = "irwm.log";
	int lf;

	char *irwmrcname;
	FILE *irwmrc;
	char *lircrc = NULL;
	char line[200], s1[200], s2[200];

	char *displayname = NULL, *fontname = NULL;
	Display *dsp;
	int (*defaulthandler)(Display *, XErrorEvent *);
	Window root, win;
	XWindowAttributes rwa, *irwa = NULL;
	GC gc;
	XGCValues gcv;
#ifndef XFT
	XFontStruct *font;
#else
	XftFont *font;
#endif
	ListWindow panelwindow, progswindow, confirmwindow;
	XWindowChanges wc;
	int listwidth, listheight;
	Atom irwm, net_wm_state, net_wm_state_stays_on_top;
	Atom supported[100];
	int nsupported = 0;
	int pn;
	char *message;
	int i, j, c, w;
	Bool tran;
	KeySym shortcuts[100];

	Bool uselirc = False, singlekey = False;
	Bool overridefix = False;
	Bool quitonlastclose = False, confirmquit = False;
	Bool run, restart;
	int command;
	Bool showpanel = False, showprogs = False, showconfirm = False;
	int progselected = 0, confirmselected = 0;
	char *p, *t;

	XEvent evt;
	XMapRequestEvent ermap;
	XConfigureRequestEvent erconfigure;
	XConfigureEvent econfigure;
	XDestroyWindowEvent edestroy;
	XReparentEvent ereparent;
	XClientMessageEvent emessage;
	XKeyEvent ekey;
	XErrorEvent err;
	char numstring[50], errortext[2000];

				/* parse options */

	cargv = argv;
	while (argn - 1 > 0 && argv[1][0] == '-') {
		if (! strcmp(argv[1], "-l"))
			uselirc = True;
		else if (! strcmp(argv[1], "-q"))
			quitonlastclose = True;
		else if (! strcmp(argv[1], "-c"))
			confirmquit = True;
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
		else if (! strcmp(argv[1], "-geometry")) {
			if (argn - 1 < 2) {
				printf("error: -geometry requires value\n");
				exit(EXIT_FAILURE);
			}
			irwa = malloc(sizeof(XWindowAttributes));
			sscanf(argv[2], "%dx%d+%d+%d",
				&irwa->width, &irwa->height,
				&irwa->x, &irwa->y);
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
			printf("\t-c\t\t\tconfirm quit if a window is open\n");
			printf("\t-r\t\t\tswitch to window by raising it\n");
			printf("\t-u\t\t\tswitch by unmapping previous\n");
			printf("\t-display display\tconnect to server\n");
			printf("\t-geometry WxH+X+Y\tgeometry of windows\n");
			printf("\t-fn font\t\tfont used in lists\n");
			printf("\t-log file\t\tlog to file\n");
			exit(! strcmp(argv[1], "-h") ?
				EXIT_SUCCESS : EXIT_FAILURE);
		}
		argn--;
		argv++;
	}

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

		numprograms = 0;
		programs[numprograms].title = "xterm";
		programs[numprograms].program = "/usr/bin/xterm";
		shortcuts[numprograms] = XK_x;
		numprograms++;
		programs[numprograms].title = "quit";
		programs[numprograms].program = NULL;
		shortcuts[numprograms] = XK_q;
		numprograms++;

		forkprogram(programs[0].program, NULL);
	}
	else {
		numprograms = 0;
		while (fgets(line, 100, irwmrc)) {
			if (1 == sscanf(line, "%s", s1) &&
			    ! strcmp(s1, "quitonlastclose"))
				quitonlastclose = True;
			else if (1 == sscanf(line, "%s", s1) &&
			     ! strcmp(s1, "confirmquit"))
				confirmquit = True;
			else if (1 == sscanf(line, "%s", s1) &&
			     ! strcmp(s1, "positionfix"))
				overridefix = True;
			else if (1 == sscanf(line, "echo %[^\n]", s1))
				printf("%s\n", s1);
			else if (1 == sscanf(line, "font %s", s1)) {
				if (fontname == NULL)
					fontname = strdup(s1);
			}
			else if (1 == sscanf(line, "logfile %s", s1))
				logfile = strdup(s1);
			else if (1 == sscanf(line, "startup %s", s1))
				forkprogram(s1, NULL);
			else if (2 == sscanf(line, "program %s %s", s1, s2)) {
				programs[numprograms].title = strdup(s1);
				programs[numprograms].program = strdup(s2);
				shortcuts[numprograms] = s1[0] - 'a' + XK_a;
				numprograms++;
			}
			else if (1 == sscanf(line, "program %s", s1)) {
				programs[numprograms].title = strdup(s1);
				programs[numprograms].program = NULL;
				shortcuts[numprograms] = s1[0] - 'a' + XK_a;
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
	programs[numprograms].title = NULL;
	shortcuts[numprograms] = XK_VoidSymbol;

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
	defaulthandler = XSetErrorHandler(handler);

				/* root window */

	root = DefaultRootWindow(dsp);
	XGetWindowAttributes(dsp, root, &rwa);
	printf("root: 0x%lx (%dx%d)\n", root, rwa.width, rwa.height);
	if (irwa) {
		rwa.width = irwa->width;
		rwa.height = irwa->height;
		rwa.x = irwa->x;
		rwa.y = irwa->y;
		free(irwa);
	}
	printf("geometry: %dx%d+%d+%d\n", rwa.width, rwa.height, rwa.x, rwa.y);

	XSelectInput(dsp, root,
		SubstructureRedirectMask |
		SubstructureNotifyMask |
		KeyPressMask);

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
	listheight = 16 * (font->ascent + font->descent + PADDING * 2) +
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

				/* confirm window */

	confirmwindow.window = XCreateSimpleWindow(dsp, root,
		rwa.width / 3, rwa.height / 2 - listheight / 2,
		listwidth, listheight,
		2, BlackPixel(dsp, 0), WhitePixel(dsp, 0));
	printf("confirm window: 0x%lx\n", confirmwindow.window);
	XStoreName(dsp, confirmwindow.window, "irwm confirm window");
	XSelectInput(dsp, confirmwindow.window, ExposureMask);

	confirmwindow.gc = gc;
	confirmwindow.font = font;
	confirmwindow.width = listwidth;
#ifdef XFT
	confirmwindow.draw = XftDrawCreate(dsp, confirmwindow.window,
		rwa.visual, rwa.colormap);
	confirmwindow.color = panelwindow.color;
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
	net_supported = XInternAtom(dsp, "_NET_PROTOCOLS", False);
	net_wm_state = XInternAtom(dsp, "_NET_WM_STATE", False);
	net_wm_state_stays_on_top =
		XInternAtom(dsp, "_NET_WM_STATE_STAYS_ON_TOP", False);
	net_active_window = XInternAtom(dsp, "_NET_ACTIVE_WINDOW", False);
	net_client_list = XInternAtom(dsp, "_NET_CLIENT_LIST", False);
	net_client_list_stacking =
		XInternAtom(dsp, "_NET_CLIENT_LIST_STACKING", False);

	supported[nsupported++] = net_wm_state;
	supported[nsupported++] = net_wm_state_stays_on_top;
	supported[nsupported++] = net_active_window;
	supported[nsupported++] = net_client_list;
	supported[nsupported++] = net_client_list_stacking; // max 100
	XChangeProperty(dsp, root, net_supported, XA_ATOM, 32,
		PropModeReplace, (unsigned char *) supported, nsupported);

	clientlistupdate(dsp, root);

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

	XWarpPointer(dsp, None, root, 0, 0, 0, 0, rwa.x + 10, rwa.y + 10);

				/* grab keys */

	for (i = 1; ! ! strcmp(commandstring[i].string, "ENDGRAB"); i++)
		XGrabKey(dsp,
			XKeysymToKeycode(dsp, commandstring[i].keysym),
			commandstring[i].modifier,
			root, False, GrabModeAsync, GrabModeAsync);

				/* main loop */

	restart = False;
	for (run = True; run; ) {

				/* X event */

		XNextEvent(dsp, &evt);
		printf("[%ld] ", evt.type == Error ? None : evt.xany.serial);

		command = NOCOMMAND;

		switch(evt.type) {

				/* substructure redirect events */

		case MapRequest:
			printf("MapRequest\n");
			ermap = evt.xmaprequest;
			tran = XGetTransientForHint(dsp, ermap.window, &win);
			printf("\t0x%lx", ermap.window);
			printf(" parent=0x%lx", ermap.parent);
			if (tran)
				printf(" transient_for=0x%lx", win);
			printf("\n");

			pn = paneladd(dsp, root, ermap.window, &rwa,
				tran ? win : None);
			if (pn == -1)
				break;

			if (activepanel != -1)
				panelleave(dsp);
			activepanel = pn;
			panelresize(dsp, pn, rwa);
			panelenter(dsp, root);
			raiselists(dsp,
				&panelwindow, &confirmwindow, &progswindow);
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

			pn = panelfind(erconfigure.window, PANEL | CONTENT);
			if (pn != -1) {
				panelresize(dsp, pn, rwa);
				break;
			}

			printf("CONFIGURE 0x%lx\n", erconfigure.window);
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
			if (overridefix)
				overrideplace(dsp, econfigure.window, &rwa);
			break;
		case CreateNotify:
			printf("CreateNotify\n");
			printf("\t0x%lx ", evt.xcreatewindow.window);
			printf("parent=0x%lx", evt.xcreatewindow.parent);
			if (evt.xcreatewindow.override_redirect) {
				printf(" override_redirect\n");
				overrideadd(evt.xcreatewindow.window);
			}
			else
				printf("\n");
			break;
		case DestroyNotify:
			printf("DestroyNotify\n");
			edestroy = evt.xdestroywindow;
			printf("\t0x%lx ", edestroy.window);
			printf("parent=0x%lx", edestroy.event);
			printf("\n");

			overrideremove(edestroy.window);

			pn = panelfind(edestroy.event, PANEL);
			if (pn == -1)
				break;

			panelremove(dsp, pn, True);

			if (numactive > 0)
				panelenter(dsp, root);
			else if (numpanels == 0 && quitonlastclose) {
				printf("QUIT on last close\n");
				run = False;
				break;
			}
			else {
				activepanel = -1;
				clientlistupdate(dsp, root);
				XSetInputFocus(dsp, root,
					RevertToParent, CurrentTime);
				printf("to quit on last close, pass -q\n");
			}

			raiselists(dsp,
				&panelwindow, &confirmwindow, &progswindow);
			break;
		case GravityNotify:
			printf("GravityNotify\n");
			break;
		case ReparentNotify:
			printf("ReparentNotify\n");
			ereparent = evt.xreparent;
			printf("\t0x%lx reparented ", ereparent.window);
			if (ereparent.event != ereparent.parent)
				printf("away from 0x%lx, ", ereparent.event);
			printf("to 0x%lx\n", ereparent.parent);
			break;
		case MapNotify:
			printf("MapNotify\n");
			printf("\t0x%lx", evt.xmap.window);
			printf(" parent=0x%lx", evt.xmap.event);
			printf("\n");

			pn = panelfind(evt.xunmap.window, CONTENT);
			if (pn == -1 && overridefix)
				overrideplace(dsp, evt.xunmap.window, &rwa);
			if (pn == -1 || pn == activepanel)
				break;
			panelleave(dsp);
			activepanel = pn;
			panelenter(dsp, root);
			raiselists(dsp,
				&panelwindow, &confirmwindow, &progswindow);

			break;
		case UnmapNotify:
			printf("UnmapNotify\n");
			printf("\t0x%lx", evt.xunmap.window);
			printf(" parent=0x%lx", evt.xunmap.event);
			printf(" %s", evt.xunmap.send_event ? "synthetic" : "");
			printf("\n");

			pn = panelfind(evt.xunmap.window, CONTENT);
			if (pn == -1)
				break;
			printf("\tcontent in panel %d\n", pn);

			if (evt.xunmap.send_event) {
				panelremove(dsp, pn, False);
				if (numactive > 0)
					panelenter(dsp, root);
				else if (numpanels == 0 && quitonlastclose) {
					run = False;
					break;
				}
				else {
					activepanel = -1;
					clientlistupdate(dsp, root);
					XSetInputFocus(dsp, root,
						RevertToParent, CurrentTime);
					printf("to quit on last close, ");
					printf("pass -q\n");
				}

				raiselists(dsp, &panelwindow,
					&confirmwindow, &progswindow);
			}

			win = panel[pn].leader;
			if (win == None)
				break;
			printf("\tleader is 0x%lx\n", win);

			pn = panelfind(win, CONTENT);
			if (pn == -1 || pn == activepanel)
				break;

			printf("\tswitching to panel %d\n", pn);
			panelleave(dsp);
			activepanel = pn;
			panelenter(dsp, root);
			raiselists(dsp,
				&panelwindow, &confirmwindow, &progswindow);

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
			    emessage.format == 32)
				command = emessage.data.l[0];

			if (emessage.message_type == net_active_window &&
			    emessage.format == 32) {
				XRaiseWindow(dsp, emessage.window);
				activewindow = emessage.window;
				printf("ACTIVEWINDOW 0x%lx\n", activewindow);
				clientlistupdate(dsp, root);
				overrideraise(dsp);
			}

			if (emessage.message_type == net_wm_state &&
			    emessage.format == 32) {
				c = emessage.data.l[0];
				printf("\t\t%s", c == 0 ? "REMOVE" :
					c == 1 ? "ADD" : "TOGGLE");
				for (i = 1; i <= 2; i++)  {
					j = emessage.data.l[i];
					if (j == 0)
						continue;
					message = XGetAtomName(dsp, j);
					printf(" %s", message);
					XFree(message);

					w = overrideexists(emessage.window);
					if (w == -1)
						continue;
					if ((Atom) j != net_wm_state_stays_on_top)
						continue;
					override[w].ontop =
						c == 0 ? False :
						c == 1 ? True :
						         ! override[w].ontop;
				}
				printf("\n");
			}

			break;

					/* keypress events */

		case KeyPress:
			printf("KeyPress\n");
			ekey = evt.xkey;
			printf("\t0x%lx ", ekey.subwindow);
			printf("key=%d state=%d", ekey.keycode, ekey.state);
			printf("\n");

			command = eventtocommand(dsp, ekey,
					showprogs ? shortcuts : NULL);
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
			printf("Expose\n");
			if (evt.xexpose.window == panelwindow.window)
				drawpanel(dsp, &panelwindow, activepanel);
			if (evt.xexpose.window == progswindow.window)
				drawprogs(dsp, &progswindow, progselected);
			if (evt.xexpose.window == confirmwindow.window)
				drawconfirm(dsp, &confirmwindow,
					confirmselected);
			break;

		case MappingNotify:
			printf("MappingNotify\n");
			printf("\t%d", evt.xmapping.request);
			printf(" %d", evt.xmapping.first_keycode);
			printf(" %d", evt.xmapping.count);
			printf("\n");
			break;

		case Error:
			printf("Error\n");
			err = evt.xerror;
			if (err.error_code == BadWindow &&
			    (err.request_code == X_MapWindow ||
			     err.request_code == X_ChangeProperty ||
			     err.request_code == X_SetInputFocus ||
			     err.request_code == X_ConfigureWindow ||
			     err.request_code == X_GetWindowAttributes)) {
				printf("NOTE: ignoring a BadWindow error ");
				printf("window=0x%lx ", err.resourceid);
				sprintf(numstring, "%d", err.request_code);
				XGetErrorDatabaseText(dsp, "XRequest",
					numstring, "", errortext, 2000);
				printf("%s\n", errortext);
				break;
			}
			if (err.error_code == BadAtom &&
			    err.request_code == X_GetAtomName) {
				printf("NOTE: ignoring a BadAtom error ");
				printf("on a X_GetAtomName request\n");
				break;
			}
			defaulthandler(dsp, &err);
			break;
		default:
			printf("Unexpected event, type=%d\n", evt.type);
		}

					/* execute command */

		while (command != NOCOMMAND) {

						/* print command */

			printf("COMMAND %s\n", commandtostring(command));

			if (command == PANELWINDOW && showpanel)
				command = singlekey ? PROGSWINDOW : HIDEWINDOW;
			if (command == PANELWINDOW && showprogs && singlekey)
				command = HIDEWINDOW;
			if (command == PROGSWINDOW && showprogs)
				command = HIDEWINDOW;
			if (command == CONFIRMWINDOW && showconfirm)
				command = HIDEWINDOW;

						/* commands in lists */

			if (NUMWINDOW(1) <= command) {
				if (showpanel) {
					if (activepanel == -1)
						continue;
					j = 0;
					for (i = 0; i < activepanel; i++)
						if (! panel[i].withdrawn)
							j++;
					panelswitch(dsp, root,
						command - NUMWINDOW(1) - j);
					XClearArea(dsp, panelwindow.window,
						0, 0, 0, 0, True);
					XRaiseWindow(dsp, panelwindow.window);
				}
				if (showprogs) {
					progselected = command - NUMWINDOW(1);
					printf("PROGSELECTED %d \"%s\"\n",
						progselected,
						programs[progselected].title);
				}
				command = OKWINDOW;
			}

						/* execute command */

			switch (command) {
			case NOCOMMAND:
				break;
			case NEXTPANEL:
			case PREVPANEL:
				panelswitch(dsp, root,
					command == PREVPANEL ? -1 : 1);
				raiselists(dsp,
					&panelwindow,
					&confirmwindow,
					&progswindow);
				break;
			case RESTART:
				restart = True;
				/* fallthrough */
			case QUIT:
				if (! confirmquit || numpanels == 0) {
					run = False;
					break;
				}
				showconfirm = True;
				confirmselected = 0;
				XMapWindow(dsp, confirmwindow.window);
				XGrabKeyboard(dsp, root, False,
					GrabModeAsync, GrabModeAsync,
					CurrentTime);
				break;

			case PANELWINDOW:
				showpanel = True;
				showprogs = False;
				showconfirm = False;
				break;
			case PROGSWINDOW:
				showprogs = True;
				showpanel = False;
				showconfirm = False;
				break;
			case CONFIRMWINDOW:
				showconfirm = True;
				showpanel = False;
				showprogs = False;
				break;

			case UPWINDOW:
			case DOWNWINDOW:
				i = command == UPWINDOW ? -1 : 1;
				if (showpanel) {
					panelswitch(dsp, root, i);
					XClearArea(dsp, panelwindow.window,
						0, 0, 0, 0, True);
					XRaiseWindow(dsp, panelwindow.window);
				}
				if (showprogs) {
					MODULEINCREASE(progselected,
						numprograms, i);
					XClearArea(dsp, progswindow.window,
						0, 0, 0, 0, True);
				}
				if (showconfirm) {
					MODULEINCREASE(confirmselected, 2, i);
					XClearArea(dsp, confirmwindow.window,
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
				else if (showprogs) {
					showprogs = False;
					XUnmapWindow(dsp, progswindow.window);
					XUngrabKeyboard(dsp, CurrentTime);
					if (command == HIDEWINDOW)
						break;
					p = programs[progselected].program;
					t = programs[progselected].title;
					if (p)
						forkprogram(p, NULL);
					else if (! strcmp(t, "resize")) {
						command = RESIZE;
						continue;
					}
					else if (! strcmp(t, "loglist")) {
						command = LOGLIST;
						continue;
					}
					else if (! strcmp(t, "positionfix")) {
						command = POSITIONFIX;
						continue;
					}
					else if (! strcmp(t, "restart")) {
						command = RESTART;
						continue;
					}
					else if (! strcmp(t, "quit")) {
						command = QUIT;
						continue;
					}
				}
				else if (showconfirm) {
					showconfirm = False;
					XUngrabKeyboard(dsp, CurrentTime);
					XUnmapWindow(dsp,
						confirmwindow.window);
					if (command == HIDEWINDOW)
						break;
					if (confirmselected == 0) {
						run = False;
						break;
					}
					showconfirm = False;
				}
				break;
			case KOWINDOW:
				if (showpanel && activepanel != -1)
					closewindow(dsp,
						panel[activepanel].content);
				break;
			case ENDWINDOW:
				if (showpanel &&
				    activepanel != -1 &&
				    activepanel < numpanels - 1) {
					for (i = activepanel;
					     i < numpanels - 1;
					     i++)
						panelswap(i, i + 1);
					activepanel = numpanels - 1;
					XClearArea(dsp, panelwindow.window,
						0, 0, 0, 0, True);
					XRaiseWindow(dsp, panelwindow.window);
				}
				break;

			case RESIZE:
				panelresize(dsp, activepanel, rwa);
				break;
			case LOGLIST:
				for (pn = 0; pn < numpanels; pn++)
					panelprint("LOG", pn);
				for (i = 0; i < numoverride; i++)
					overrideprint("LOG", i);
				break;
			case POSITIONFIX:
				overridefix = ! overridefix;
				printf("OVERRIDEFIX %d\n", overridefix);
				break;
			}

						/* show/remove lists */

			if (showpanel)
				XMapWindow(dsp, panelwindow.window);
			else
				XUnmapWindow(dsp, panelwindow.window);
			if (showprogs)
				XMapWindow(dsp, progswindow.window);
			else
				XUnmapWindow(dsp, progswindow.window);
			if (showconfirm)
				XMapWindow(dsp, confirmwindow.window);
			else
				XUnmapWindow(dsp, confirmwindow.window);
			if (showpanel || showprogs || showconfirm)
				XGrabKeyboard(dsp, root, False,
					GrabModeAsync, GrabModeAsync,
					CurrentTime);

			fflush(NULL);
			command = NOCOMMAND;
		}
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
	if (restart) {
		printf("irwm restart\n");
		execvp(cargv[0], cargv);
		perror(cargv[0]);
	}
	printf("irwm ended\n");

	return EXIT_SUCCESS;
}

