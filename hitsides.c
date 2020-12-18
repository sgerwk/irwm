/*
 * hitsides.c
 *
 * switch to previous or next panel by hitting the side of the scren with the
 * cursor
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <X11/Xlib.h>

/*
 * irwm communication
 */
Atom irwm;
#define IRWM "IRWM"
#define NEXTPANEL      1	/* switch to next panel */
#define PREVPANEL      2	/* switch to previous panel */

/*
 * send command to irwm
 */
void sendmessage(Display *d, Window root, int command) {
	XClientMessageEvent message;
	long mask;

	message.type = ClientMessage;
	message.message_type = irwm;
	message.window = root;
	message.format = 32;
	message.data.l[0] = command;
	message.data.l[1] = 0;
	message.data.l[2] = 0;
	message.data.l[3] = 0;
	message.data.l[4] = 0;
	mask = SubstructureRedirectMask | SubstructureNotifyMask;
	XSendEvent(d, root, False, mask, (XEvent *) &message);
}

/*
 * print the last cursor positions
 */
void printhistory(XTimeCoord *mh, int mhlen, int level) {
	int i;

	if (level == 0)
		return;

	printf("[%d]", mhlen);
	for (i = 0; i < mhlen; i++)
		switch (level) {
		case 2:
			printf(" %hd", mh[i].x);
			break;
		case 3:
			printf(" %lu:%hd", mh[i].time, mh[i].x);
			break;
		case 4:
			printf(" %lu:%hd,%hd", mh[i].time, mh[i].x, mh[i].y);
			break;
		}
	printf("\n");
}

/*
 * update score
 */
int score(int x, int border, int width, int prev) {
	int side = 5, leftside = 5, rightside = 100 - 5;

	if (x == 0)
		return prev;

	if (prev == 0 && border - side <= x && x <= border + side)
		return prev + 1;
	if (prev > 0 && prev < 4) {
		if (x < width * rightside / 100 && x > width * leftside / 100)
			return prev + 1;
		else
			return 1;
	}
	if (prev == 4 && border - side <= x && x <= border + side)
		return prev + 1;
	if (prev > 4 && prev < 6) {
		if (x < width * rightside / 100 && x > width * leftside / 100)
			return prev + 1;
		else
			return 5;
	}

	return prev;
}

/*
 * main
 */
int main(int argc, char *argv[]) {
	Display *d;
	Window root;
	XWindowAttributes wa;
	unsigned long size;
	XTimeCoord *mh;
	int mhlen;
	Time start, last, interval = 2000;
	int i;
	short x;
	int hitleft, hitright;
	int sleeptime = 100000;
	int printlevel = 0;

					/* arguments */

	if (argc - 1 >= 1) {
		if (! ! strcmp(argv[1], "-h"))
			printf("unrecognized argument: %s\n", argv[1]);
		printf("switch panel in irwm by hitting the side of the ");
		printf("screen twice\n");
		exit(EXIT_SUCCESS);
	}

					/* open display */

	d = XOpenDisplay(getenv("DISPLAY"));
	if (d == NULL) {
		printf("cannot open display\n");
		exit(EXIT_FAILURE);
	}

	size = XDisplayMotionBufferSize(d);
	if (printlevel)
		printf("motion event buffer size: %ld\n", size);
	if (size == 0) {
		printf("motion event history not supported\n");
		XCloseDisplay(d);
		exit(EXIT_FAILURE);
	}

	root = DefaultRootWindow(d);
	XGetWindowAttributes(d, root, &wa);
	irwm = XInternAtom(d, IRWM, False);

					/* read motion history */

	start = 1L;
	while (1) {
		mh = XGetMotionEvents(d, root, start, CurrentTime, &mhlen);
		if (mhlen == 0)
			continue;
		last = mh[mhlen - 1].time;

				/* start time of next motion history request */

		if (start == last - interval) {
			start = last + 1L;
			XFree(mh);
			usleep(sleeptime);
			continue;
		}
		start = last - interval < start ? start : last - interval;

		printhistory(mh, mhlen, printlevel);

				/* score of motion history */

		hitleft = 0;
		hitright = 0;
		for (i = mhlen - 1;
		     i >= 0 && mh[i].time >= last - 1000;
		     i--) {
			x = mh[i].x;
			hitleft = score(x, 0, wa.width, hitleft);
			hitright = score(x, wa.width - 1, wa.width, hitright);
		}
		if (printlevel)
			printf("\tleft: %d\tright: %d\n", hitleft, hitright);

				/* switch panel in irwm */

		if (hitleft == 6) {
			if (printlevel)
				printf("<<<<<<<<<<<<<<<<<< hit!\n");
			else
				sendmessage(d, root, PREVPANEL);
			start = last + 1L;
		}

		if (hitright == 6) {
			if (printlevel)
				printf(">>>>>>>>>>>>>>>>>> hit!\n");
			else
				sendmessage(d, root, NEXTPANEL);
			start = last + 1L;
		}

		XFree(mh);
		usleep(sleeptime);
	}

	XCloseDisplay(d);
}

