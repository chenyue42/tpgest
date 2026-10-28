// tpgest: trackpad gesture recognizer built on the private MultitouchSupport framework.
//
// Reads raw finger contacts (~100 frames/s per trackpad), recognizes swipes, pinches and
// double taps by finger count, and fires keyboard shortcuts or sleep.
//
//   ./tpgest --dry-run --debug   log what it sees, fire nothing
//   ./tpgest                     run for real (needs Accessibility permission)

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

// ---- MultitouchSupport (undocumented; layout as used by Jitouch and others) ----

typedef struct { float x, y; } MTPoint;
typedef struct { MTPoint pos, vel; } MTVector;
typedef struct {
    int frame;
    double timestamp;
    int identifier, state, fingerId, handId;
    MTVector normalized;  // 0..1, origin bottom-left
    float size;
    int zero1;
    float angle, majorAxis, minorAxis;
    MTVector mm;
    int zero2[2];
    float density;
} MTTouch;
_Static_assert(sizeof(MTTouch) == 96, "MTTouch layout changed");

enum { MT_TOUCH_MAKE = 4, MT_TOUCH_TOUCHING = 5 };

typedef void *MTDeviceRef;
typedef int (*MTContactCallback)(MTDeviceRef, MTTouch *, int, double, int);

static CFArrayRef (*MTDeviceCreateList)(void);
static void (*MTRegisterContactFrameCallback)(MTDeviceRef, MTContactCallback);
static void (*MTDeviceStart)(MTDeviceRef, int);
static OSStatus (*MTDeviceGetFamilyID)(MTDeviceRef, int *);

// ---- Bindings: edit these ----

enum { ACT_KEY, ACT_SLEEP };
typedef struct {
    const char *gesture;  // "swipe-up-4", "pinch-in-3", "double-tap-5", ...
    int kind;
    CGKeyCode key;        // virtual key code (ANSI layout)
    CGEventFlags flags;
    const char *label;
} Binding;

#define CMD kCGEventFlagMaskCommand
#define CTRL kCGEventFlagMaskControl

static const Binding bindings[] = {
    {"double-tap-5", ACT_SLEEP, 0, 0, "sleep"},
    {"pinch-in-3", ACT_KEY, 3, CTRL | CMD, "^cmd-F"},
    {"swipe-down-4", ACT_KEY, 13, CMD, "cmd-W"},
    {"swipe-up-4", ACT_KEY, 17, CMD, "cmd-T"},
};

// ---- Tuning (distances are in normalized trackpad units, 0..1) ----
// The first three can be overridden with --settle=, --swipe=, --pinch-in= so they can be
// tuned from the LaunchAgent plist without rebuilding (a rebuild resets Accessibility).

static double settleSec = 0.03;    // finger count must hold this long before movement counts
static float swipeDist = 0.08f;    // centroid travel to recognize a swipe
static float pinchInRatio = 0.80f; // spread shrinks to this fraction of its start
#define SWIPE_AXIS_RATIO 2.0f      // dominant axis must be this many times the other
#define PINCH_OUT_RATIO 1.5f
#define PINCH_MAX_DRIFT 0.08f // centroid may drift this much during a pinch
#define MIN_FLICK_SEC 0.02    // a finger count must last this long to count as a swipe on lift-off
#define TAP_MAX_SEC 0.30
#define TAP_MAX_MOVE 0.03f
#define DOUBLE_TAP_SEC 0.40

// Magic Mouse 1 and 2 report touches too; only trackpads are handled here.
static bool isMouseFamily(int family) { return family == 112 || family == 113; }

// ---- Recognizer ----

typedef struct {
    MTDeviceRef dev;
    int count, maxCount;
    double sessionStart, countSince;
    // Where each finger first touched this session. Swipe travel is measured from here, so
    // motion while fingers are still landing counts toward a fast flick.
    struct { int id; float x, y; } origins[16];
    int originCount;
    float dx, dy;    // mean per-finger travel at the latest frame of the current finger count
    float bspread;   // spread once the current finger count settled (pinch baseline)
    float maxMove;
    bool fired;       // a bound gesture fired; ignore the rest of this session
    bool sawGesture;  // any swipe/pinch recognized, bound or not; rules out a tap
    // Diagnostics for the current finger-count phase (printed with --debug)
    float peakDx, peakDy, minRatio, maxRatio;
    char firedName[32];
    double lastTapTime;
    int lastTapFingers;
} DevState;

#define MAX_DEVICES 8
static DevState devices[MAX_DEVICES];
static int deviceCount;
static bool dryRun, debug;

static DevState *stateFor(MTDeviceRef dev) {
    for (int i = 0; i < deviceCount; i++)
        if (devices[i].dev == dev) return &devices[i];
    return NULL;
}

static void sendKey(CGKeyCode key, CGEventFlags flags) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, key, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, key, false);
    CGEventSetFlags(down, flags);
    CGEventSetFlags(up, flags);
    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);
}

static void sleepNow(void) {
    char *argv[] = {"pmset", "sleepnow", NULL};
    pid_t pid;
    posix_spawn(&pid, "/usr/bin/pmset", NULL, NULL, argv, environ);
}

static const Binding *findBinding(const char *gesture) {
    for (size_t i = 0; i < sizeof bindings / sizeof *bindings; i++)
        if (strcmp(bindings[i].gesture, gesture) == 0) return &bindings[i];
    return NULL;
}

static void trigger(const char *gesture) {
    const Binding *b = findBinding(gesture);
    if (!b) {
        if (debug) {
            printf("%s (unbound)\n", gesture);
            fflush(stdout);
        }
        return;
    }
    printf("%s -> %s%s\n", gesture, b->label, dryRun ? " (dry run)" : "");
    fflush(stdout);
    if (dryRun) return;
    if (b->kind == ACT_SLEEP) sleepNow();
    else sendKey(b->key, b->flags);
}

static void endSession(DevState *s, double ts) {
    bool tap = !s->sawGesture && ts - s->sessionStart <= TAP_MAX_SEC && s->maxMove <= TAP_MAX_MOVE;
    if (!tap) {
        s->lastTapTime = 0;
        return;
    }
    if (s->lastTapFingers == s->maxCount && ts - s->lastTapTime <= DOUBLE_TAP_SEC) {
        char name[32];
        snprintf(name, sizeof name, "double-tap-%d", s->maxCount);
        trigger(name);
        s->lastTapTime = 0;
    } else {
        s->lastTapTime = ts;
        s->lastTapFingers = s->maxCount;
    }
}

// Swipe is judged on per-finger travel; pinch (ratio > 0) on spread change with little travel.
static void recognize(DevState *s, int k, float ratio) {
    float move = hypotf(s->dx, s->dy);
    char name[32] = "";
    if (move >= swipeDist) {
        if (fabsf(s->dy) > SWIPE_AXIS_RATIO * fabsf(s->dx))
            snprintf(name, sizeof name, "swipe-%s-%d", s->dy > 0 ? "up" : "down", k);
        else if (fabsf(s->dx) > SWIPE_AXIS_RATIO * fabsf(s->dy))
            snprintf(name, sizeof name, "swipe-%s-%d", s->dx > 0 ? "right" : "left", k);
    } else if (ratio > 0 && k >= 2 && move <= PINCH_MAX_DRIFT) {
        if (ratio <= pinchInRatio) snprintf(name, sizeof name, "pinch-in-%d", k);
        else if (ratio >= PINCH_OUT_RATIO) snprintf(name, sizeof name, "pinch-out-%d", k);
    }
    // Only bound gestures end the session, so an unbound one seen early (say pinch-in-4
    // while four fingers start a swipe) can't block the gesture the user meant.
    if (name[0] && strcmp(name, s->firedName) != 0) {
        s->sawGesture = true;
        s->fired = findBinding(name) != NULL;
        snprintf(s->firedName, sizeof s->firedName, "%s", name);
        trigger(name);
    }
}

static int onFrame(MTDeviceRef dev, MTTouch *touches, int n, double ts, int frame) {
    (void)frame;
    DevState *s = stateFor(dev);
    if (!s) return 0;

    MTPoint pts[16];
    int ids[16];
    int k = 0;
    for (int i = 0; i < n && k < 16; i++)
        if (touches[i].state == MT_TOUCH_MAKE || touches[i].state == MT_TOUCH_TOUCHING) {
            ids[k] = touches[i].identifier;
            pts[k++] = touches[i].normalized.pos;
        }

    if (k != s->count) {
        if (s->count == 0) {
            s->sessionStart = ts;
            s->maxCount = 0;
            s->maxMove = 0;
            s->originCount = 0;
            s->fired = false;
            s->sawGesture = false;
        } else if (k < s->count && !s->fired && ts - s->countSince >= MIN_FLICK_SEC) {
            // Fingers are lifting: a flick can end before the count ever settles, so judge
            // the phase on its final travel.
            recognize(s, s->count, -1);
        }
        if (k == 0) endSession(s, ts);
        if (debug && s->count >= 2) {
            printf("  [dev%ld] phase n=%d dur=%.2fs dx=%+.3f dy=%+.3f ratio=%.2f..%.2f %s\n", (long)(s - devices),
                   s->count, ts - s->countSince, s->peakDx, s->peakDy, s->minRatio, s->maxRatio,
                   s->firedName[0] ? s->firedName : "-");
            fflush(stdout);
        }
        s->count = k;
        s->countSince = ts;
        s->peakDx = s->peakDy = 0;
        s->minRatio = s->maxRatio = 1;
        s->firedName[0] = '\0';
    }
    if (k == 0) return 0;
    if (k > s->maxCount) s->maxCount = k;

    float cx = 0, cy = 0, spread = 0, dx = 0, dy = 0;
    for (int i = 0; i < k; i++) cx += pts[i].x, cy += pts[i].y;
    cx /= k, cy /= k;
    for (int i = 0; i < k; i++) {
        spread += hypotf(pts[i].x - cx, pts[i].y - cy);
        int o = 0;
        while (o < s->originCount && s->origins[o].id != ids[i]) o++;
        if (o == s->originCount && o < 16) {
            s->origins[o].id = ids[i], s->origins[o].x = pts[i].x, s->origins[o].y = pts[i].y;
            s->originCount++;
        }
        if (o < 16) dx += pts[i].x - s->origins[o].x, dy += pts[i].y - s->origins[o].y;
    }
    spread /= k;
    s->dx = dx / k, s->dy = dy / k;

    float move = hypotf(s->dx, s->dy);
    if (move > s->maxMove) s->maxMove = move;
    if (move > hypotf(s->peakDx, s->peakDy)) s->peakDx = s->dx, s->peakDy = s->dy;

    // Don't fire until the finger count settles, so fingers landing one by one don't
    // trigger a gesture for the wrong count. Travel during this time still counts.
    if (ts - s->countSince < settleSec) {
        s->bspread = spread;
        return 0;
    }

    float ratio = s->bspread > 0.01f ? spread / s->bspread : -1;
    if (ratio > 0 && ratio < s->minRatio) s->minRatio = ratio;
    if (ratio > s->maxRatio) s->maxRatio = ratio;
    if (!s->fired) recognize(s, k, ratio);
    return 0;
}

// ---- Setup ----

static bool loadFramework(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "cannot load MultitouchSupport: %s\n", dlerror());
        return false;
    }
    MTDeviceCreateList = dlsym(h, "MTDeviceCreateList");
    MTRegisterContactFrameCallback = dlsym(h, "MTRegisterContactFrameCallback");
    MTDeviceStart = dlsym(h, "MTDeviceStart");
    MTDeviceGetFamilyID = dlsym(h, "MTDeviceGetFamilyID");  // optional
    if (!MTDeviceCreateList || !MTRegisterContactFrameCallback || !MTDeviceStart) {
        fprintf(stderr, "MultitouchSupport is missing expected symbols\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run")) dryRun = true;
        else if (!strcmp(argv[i], "--debug")) debug = true;
        else if (!strncmp(argv[i], "--settle=", 9)) settleSec = atof(argv[i] + 9);
        else if (!strncmp(argv[i], "--swipe=", 8)) swipeDist = atof(argv[i] + 8);
        else if (!strncmp(argv[i], "--pinch-in=", 11)) pinchInRatio = atof(argv[i] + 11);
        else {
            printf("usage: %s [--dry-run] [--debug] [--settle=SEC] [--swipe=DIST] [--pinch-in=RATIO]\n", argv[0]);
            return strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    signal(SIGCHLD, SIG_IGN);  // reap pmset without waiting
    if (!loadFramework()) return 1;

    if (!dryRun && !AXIsProcessTrusted())
        fprintf(stderr, "warning: no Accessibility permission; shortcuts will not be delivered.\n"
                        "Grant it to this binary (or your terminal) in System Settings > Privacy & Security > Accessibility.\n");

    CFArrayRef list = MTDeviceCreateList();
    for (CFIndex i = 0; list && i < CFArrayGetCount(list) && deviceCount < MAX_DEVICES; i++) {
        MTDeviceRef dev = (MTDeviceRef)CFArrayGetValueAtIndex(list, i);
        int family = -1;
        if (MTDeviceGetFamilyID) MTDeviceGetFamilyID(dev, &family);
        if (isMouseFamily(family)) {
            printf("skipping mouse (family %d)\n", family);
            continue;
        }
        printf("dev%d: trackpad family %d\n", deviceCount, family);
        devices[deviceCount++].dev = dev;
        MTRegisterContactFrameCallback(dev, onFrame);
        MTDeviceStart(dev, 0);
    }
    if (deviceCount == 0) {
        fprintf(stderr, "no trackpads found\n");
        return 1;
    }
    printf("listening%s\n", dryRun ? " (dry run)" : "");
    fflush(stdout);
    CFRunLoopRun();
    return 0;
}
