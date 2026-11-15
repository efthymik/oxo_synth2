/* Send synthetic pointer events to an X11 window (XSendEvent).
 *
 * Wayland sessions cannot warp the pointer from an X client (the compositor
 * owns it), so xdotool mousemove/click do nothing there. A plugin window's
 * input can still be exercised: XSendEvent delivers a ButtonPress /
 * ButtonRelease / MotionNotify to the client that selected that event type.
 *
 * Build: cc -O2 xsend.c -o xsend $(pkg-config --cflags --libs x11)
 * Usage: xsend <window-id-hex> <down|up|move> <x> <y> [button]
 *        xsend <window-id-hex> double <x> <y>       (two clicks, 80 ms apart)
 *        xsend <window-id-hex> key <keysym>         (e.g. Escape, Right, Return)
 *        xsend <window-id-hex> text <string>        (ASCII typing)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Time nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Time)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void sendButton(Display *dpy, Window win, int type, int x, int y, unsigned button, Time time) {
    XButtonEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.display = dpy;
    ev.window = win;
    ev.root = DefaultRootWindow(dpy);
    ev.subwindow = None;
    ev.time = time;
    ev.x = x;
    ev.y = y;
    ev.x_root = x;
    ev.y_root = y;
    ev.same_screen = True;
    ev.button = button;
    ev.state = 0;
    const long mask = type == ButtonPress ? ButtonPressMask : ButtonReleaseMask;
    XSendEvent(dpy, win, False, mask, (XEvent *)&ev);
    XFlush(dpy);
}

static void sendMotion(Display *dpy, Window win, int x, int y, Time time) {
    XMotionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = MotionNotify;
    ev.display = dpy;
    ev.window = win;
    ev.root = DefaultRootWindow(dpy);
    ev.time = time;
    ev.x = x;
    ev.y = y;
    ev.x_root = x;
    ev.y_root = y;
    ev.same_screen = True;
    XSendEvent(dpy, win, False, PointerMotionMask, (XEvent *)&ev);
    XFlush(dpy);
}

static void sendKey(Display *dpy, Window win, KeySym sym, Time time) {
    const KeyCode code = XKeysymToKeycode(dpy, sym);
    if (!code) {
        fprintf(stderr, "xsend: no keycode for that keysym\n");
        return;
    }
    XKeyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = KeyPress;
    ev.display = dpy;
    ev.window = win;
    ev.root = DefaultRootWindow(dpy);
    ev.time = time;
    ev.same_screen = True;
    ev.keycode = code;
    XSendEvent(dpy, win, False, KeyPressMask, (XEvent *)&ev);
    ev.type = KeyRelease;
    XSendEvent(dpy, win, False, KeyReleaseMask, (XEvent *)&ev);
    XFlush(dpy);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: xsend <win> <down|up|move|double> <x> <y> [button]\n"
                        "       xsend <win> key <keysym>\n");
        return 2;
    }
    if (!strcmp(argv[2], "text")) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) return 1;
        const Window win = (Window)strtoul(argv[1], NULL, 0);
        const char *text = argv[3];
        for (const char *c = text; *c; ++c) {
            const KeySym sym = (KeySym)(unsigned char)*c;
            const KeyCode code = XKeysymToKeycode(dpy, sym);
            if (!code) continue;
            XKeyEvent ev;
            memset(&ev, 0, sizeof ev);
            ev.display = dpy;
            ev.window = win;
            ev.root = DefaultRootWindow(dpy);
            ev.time = nowMs();
            ev.same_screen = True;
            ev.keycode = code;
            ev.type = KeyPress;
            XSendEvent(dpy, win, False, KeyPressMask, (XEvent *)&ev);
            ev.type = KeyRelease;
            XSendEvent(dpy, win, False, KeyReleaseMask, (XEvent *)&ev);
        }
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }
    if (!strcmp(argv[2], "key")) {
        Display *dpy = XOpenDisplay(NULL);
        if (!dpy) return 1;
        const KeySym sym = XStringToKeysym(argv[3]);
        sendKey(dpy, (Window)strtoul(argv[1], NULL, 0), sym, nowMs());
        XSync(dpy, False);
        XCloseDisplay(dpy);
        return 0;
    }
    if (argc < 5) {
        fprintf(stderr, "usage: xsend <win> <down|up|move|double> <x> <y> [button]\n");
        return 2;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xsend: cannot open display\n");
        return 1;
    }
    const Window win = (Window)strtoul(argv[1], NULL, 0);
    const char *what = argv[2];
    const int x = atoi(argv[3]), y = atoi(argv[4]);
    const unsigned button = argc > 5 ? (unsigned)atoi(argv[5]) : 1u;

    if (!strcmp(what, "down")) {
        sendButton(dpy, win, ButtonPress, x, y, button, nowMs());
    } else if (!strcmp(what, "up")) {
        sendButton(dpy, win, ButtonRelease, x, y, button, nowMs());
    } else if (!strcmp(what, "move")) {
        sendMotion(dpy, win, x, y, nowMs());
    } else if (!strcmp(what, "double")) {
        const Time t = nowMs();
        sendButton(dpy, win, ButtonPress, x, y, button, t);
        sendButton(dpy, win, ButtonRelease, x, y, button, t + 1);
        sendButton(dpy, win, ButtonPress, x, y, button, t + 80);
        sendButton(dpy, win, ButtonRelease, x, y, button, t + 81);
    } else {
        fprintf(stderr, "xsend: unknown action %s\n", what);
        return 2;
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
