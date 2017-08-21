irwm is an X11 window manager designed for remote controls. It shows one window
at time in full screen. It allows switching between them and opening new ones
from a menu.

It can be started from a virtual terminal via either of the two following ways:

- xinit path/to/irwm
- startx path/to/irwm

Upon startup, irwm reads the configuration file .irwmrc or /etc/irwmrc, which
contains the programs it initially launch and the ones that are shown in the
program list so that the user can start them. File [irwmrc](/irwmrc) is an
example.

It can be controlled via the keyboard:

- alt-right: next window
- alt-left: previous window
- alt-tab: list of currently open windows, key 'c' to close one
- ctrl-tab: list of programs the user can launch
- ctrl-shift-tab: quit

The same functions can be performed from a remote control by passing option -l
to irwm and adding to .lircrc the association between lirc keys and irwm
commands. This is described in irwm man page [irwm.1](/irwm.1). File
[lircrc](/lircrc) is an example.

