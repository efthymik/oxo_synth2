/* Grab an X11 window's own content and write it as a PPM.
 *
 * Why not a root grab (x11grab, import -window root): under Wayland the X
 * server is XWayland, which keeps no composited copy of window content in
 * the root, so a root capture comes out black however well the plugin draws.
 * XGetImage on the window itself reads what the client put there. (XWayland
 * refuses a full-width XGetImage on an embedded child window — BadMatch at
 * exactly the window width — so grab the host's top-level window, which
 * composites its children.)
 *
 * Build: cc -O2 xgrab.c -o xgrab -lX11
 * Usage: xgrab <window-id-hex> <out.ppm>   (ffmpeg -i out.ppm out.png)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: xgrab <window-id-hex> <out.ppm>\n");
        return 2;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xgrab: cannot open display\n");
        return 1;
    }
    const Window win = (Window)strtoul(argv[1], NULL, 0);
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr)) {
        fprintf(stderr, "xgrab: no such window %s\n", argv[1]);
        return 1;
    }
    XImage *img = XGetImage(dpy, win, 0, 0, attr.width, attr.height, AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "xgrab: XGetImage failed\n");
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "xgrab: cannot write %s\n", argv[2]);
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", attr.width, attr.height);
    for (int y = 0; y < attr.height; ++y) {
        for (int x = 0; x < attr.width; ++x) {
            const unsigned long pixel = XGetPixel(img, x, y);
            const unsigned char rgb[3] = {(unsigned char)((pixel >> 16) & 0xFF), (unsigned char)((pixel >> 8) & 0xFF),
                                          (unsigned char)(pixel & 0xFF)};
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    XDestroyImage(img);
    XCloseDisplay(dpy);
    return 0;
}
