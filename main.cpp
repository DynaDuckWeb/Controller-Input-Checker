#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/input.h>
#include <linux/joystick.h>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <cmath>
#include <csignal>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// ANSI helpers
// ─────────────────────────────────────────────────────────────────────────────
#define ESC        "\033["
#define RESET      "\033[0m"
#define BOLD       "\033[1m"
#define DIM        "\033[2m"

// 256-color fg / bg
#define FG(n)  "\033[38;5;" #n "m"
#define BG(n)  "\033[48;5;" #n "m"

// Palette
#define C_BG        "\033[48;5;234m"   // near-black bg
#define C_PANEL     "\033[48;5;236m"   // card bg
#define C_BORDER    "\033[38;5;240m"   // dim border
#define C_TITLE     "\033[38;5;75m"    // blue title
#define C_LABEL     "\033[38;5;250m"   // light grey label
#define C_DIM       "\033[38;5;240m"   // dark grey
#define C_ON        "\033[38;5;82m"    // bright green  (pressed)
#define C_AXIS      "\033[38;5;75m"    // blue axis value
#define C_TRIG      "\033[38;5;214m"   // orange trigger
#define C_DPAD      "\033[38;5;183m"   // lavender dpad
#define C_WARN      "\033[38;5;196m"   // red

static void clear_screen()  { std::fputs("\033[2J\033[H",  stdout); }
static void cursor_home()   { std::fputs("\033[H",         stdout); }
static void hide_cursor()   { std::fputs("\033[?25l",      stdout); }
static void show_cursor()   { std::fputs("\033[?25h",      stdout); }
static void goto_xy(int r, int c) {
    std::printf("\033[%d;%dH", r, c);
}

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int DEADZONE = 8000;

struct ControllerState {
    // Sticks  -32767…32767
    int lx = 0, ly = 0;
    int rx = 0, ry = 0;
    // Triggers  0…255
    int lt = 0, rt = 0;
    // D-Pad  -1,0,1
    int dpx = 0, dpy = 0;
    // Buttons
    std::map<int,bool> buttons;
    // Device name
    std::string name;
    // Last event log (short)
    std::vector<std::string> log;

    void push_log(const std::string &s) {
        log.push_back(s);
        if ((int)log.size() > 5) log.erase(log.begin());
    }
};

static ControllerState g_state;
static volatile bool   g_running = true;

void on_sigint(int) {
    g_running = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Button name table
// ─────────────────────────────────────────────────────────────────────────────
static const std::map<int,std::string> BTN_NAMES = {
    {BTN_A,      "A"     }, {BTN_B,     "B"  },
    {BTN_X,      "X"     }, {BTN_Y,     "Y"  },
    {BTN_TL,     "L1"    }, {BTN_TR,    "R1" },
    {BTN_TL2,    "L2"    }, {BTN_TR2,   "R2" },
    {BTN_SELECT, "Select"}, {BTN_START, "Start"},
    {BTN_MODE,   "Guide" },
    {BTN_THUMBL, "L3"    }, {BTN_THUMBR,"R3" },
    {BTN_DPAD_UP,"DU"   },{BTN_DPAD_DOWN,"DD"},
    {BTN_DPAD_LEFT,"DL" },{BTN_DPAD_RIGHT,"DR"},
};
static std::string btn_name(int c) {
    auto it = BTN_NAMES.find(c);
    return (it != BTN_NAMES.end()) ? it->second : "?";
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw helpers
// ─────────────────────────────────────────────────────────────────────────────

// Draw a horizontal bar for a trigger (0-255), width chars wide
static void draw_trigger_bar(int val, int width) {
    int filled = val * width / 255;
    std::fputs(C_TRIG, stdout);
    for (int i = 0; i < width; i++)
        std::fputs(i < filled ? "█" : "░", stdout);
    std::fputs(RESET, stdout);
}

// Draw a mini stick circle (5×3 chars) with a dot showing position
//   x,y in -32767..32767
static void draw_stick(int x, int y, int base_row, int base_col) {
    // Map -32767..32767 → 0..4 (col) and 0..2 (row)
    int cx = (x + 32767) * 4 / 65534;  // 0-4
    int cy = (y + 32767) * 2 / 65534;  // 0-2

    // Dead zone → center
    if (std::abs(x) < DEADZONE && std::abs(y) < DEADZONE) { cx = 2; cy = 1; }

    const char *ring[3][5] = {
        {"╭","─","─","─","╮"},
        {"│"," "," "," ","│"},
        {"╰","─","─","─","╯"},
    };
    for (int row = 0; row < 3; row++) {
        goto_xy(base_row + row, base_col);
        std::fputs(C_BORDER, stdout);
        for (int col = 0; col < 5; col++) {
            if (row == cy && col == cx)
                std::printf(C_AXIS "●" C_BORDER);
            else
                std::fputs(ring[row][col], stdout);
        }
        std::fputs(RESET, stdout);
    }
}

// Draw a D-Pad (5×5 chars) at base_row, base_col
static void draw_dpad(int dx, int dy, int base_row, int base_col) {
    // Layout:
    //  row0: "  ▲  "
    //  row1: "◄   ►"
    //  row2: "  ▼  "
    auto color = [](bool on) { return on ? C_DPAD BOLD : C_DIM; };

    goto_xy(base_row,   base_col); std::printf("  %s▲" RESET "  ", color(dy < 0));
    goto_xy(base_row+1, base_col); std::printf("%s◄" RESET "   %s►" RESET, color(dx < 0), color(dx > 0));
    goto_xy(base_row+2, base_col); std::printf("  %s▼" RESET "  ", color(dy > 0));
}

// Draw a button badge (label, is_pressed)
static void draw_btn(int row, int col, const std::string &label, bool pressed) {
    goto_xy(row, col);
    if (pressed)
        std::printf(C_ON BOLD "[%s]" RESET, label.c_str());
    else
        std::printf(C_DIM "[%s]" RESET, label.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// Full dashboard render  (fits in 24×80)
// ─────────────────────────────────────────────────────────────────────────────
static void render() {
    cursor_home();

    // ── Title bar ────────────────────────────────────────────────────────────
    std::printf(C_PANEL BOLD C_TITLE
    " ◈  Controller Input  " RESET C_PANEL C_DIM
    "  %-50s" RESET "\n",
    g_state.name.c_str());
    std::printf(C_DIM "─────────────────────────────────────────────────────────────────────────────\n" RESET);

    // ── Left Trigger  ────────────────────────────────────────────────────────
    goto_xy(3, 2);
    std::printf(C_LABEL " L2 " RESET);
    goto_xy(3, 6);
    draw_trigger_bar(g_state.lt, 12);
    std::printf(C_DIM " %3d" RESET, g_state.lt * 100 / 255);
    std::fputs("%", stdout);

    // ── Right Trigger ────────────────────────────────────────────────────────
    goto_xy(3, 46);
    std::printf(C_LABEL "R2 " RESET);
    goto_xy(3, 49);
    draw_trigger_bar(g_state.rt, 12);
    std::printf(C_DIM " %3d" RESET, g_state.rt * 100 / 255);
    std::fputs("%", stdout);

    // ── Shoulder buttons ─────────────────────────────────────────────────────
    draw_btn(4,  2, "L1", g_state.buttons[BTN_TL]);
    draw_btn(4, 46, "R1", g_state.buttons[BTN_TR]);

    // ── Left stick ───────────────────────────────────────────────────────────
    goto_xy(6, 2);
    std::printf(C_LABEL "Left Stick" RESET);
    draw_stick(g_state.lx, g_state.ly, 7, 2);

    // Direction text under stick
    std::string ldir = "";
    bool lin_dead = std::abs(g_state.lx) < DEADZONE && std::abs(g_state.ly) < DEADZONE;
    if (!lin_dead) {
        if (g_state.ly < -DEADZONE) ldir += "UP ";
        if (g_state.ly >  DEADZONE) ldir += "DOWN ";
        if (g_state.lx < -DEADZONE) ldir += "LEFT";
        if (g_state.lx >  DEADZONE) ldir += "RIGHT";
    }
    goto_xy(10, 2);
    std::printf(C_AXIS "%-10s" RESET, lin_dead ? "CENTER" : ldir.c_str());

    // ── Right stick ──────────────────────────────────────────────────────────
    goto_xy(6, 46);
    std::printf(C_LABEL "Right Stick" RESET);
    draw_stick(g_state.rx, g_state.ry, 7, 46);

    std::string rdir = "";
    bool rin_dead = std::abs(g_state.rx) < DEADZONE && std::abs(g_state.ry) < DEADZONE;
    if (!rin_dead) {
        if (g_state.ry < -DEADZONE) rdir += "UP ";
        if (g_state.ry >  DEADZONE) rdir += "DOWN ";
        if (g_state.rx < -DEADZONE) rdir += "LEFT";
        if (g_state.rx >  DEADZONE) rdir += "RIGHT";
    }
    goto_xy(10, 46);
    std::printf(C_AXIS "%-10s" RESET, rin_dead ? "CENTER" : rdir.c_str());

    // ── D-Pad ────────────────────────────────────────────────────────────────
    goto_xy(6, 22);
    std::printf(C_LABEL "D-Pad" RESET);
    draw_dpad(g_state.dpx, g_state.dpy, 7, 22);

    // ── Face buttons ─────────────────────────────────────────────────────────
    //   Y
    // X   B
    //   A
    goto_xy(7, 34);  std::printf("  ");  draw_btn(7, 36, "Y", g_state.buttons[BTN_Y]);
    draw_btn(8, 34, "X", g_state.buttons[BTN_X]);
    goto_xy(8, 38);  std::fputs("  ", stdout);
    draw_btn(8, 38, "B", g_state.buttons[BTN_B]);
    goto_xy(9, 35);  std::fputs(" ", stdout);
    draw_btn(9, 36, "A", g_state.buttons[BTN_A]);

    // ── Center buttons ───────────────────────────────────────────────────────
    draw_btn(12, 22, "Select", g_state.buttons[BTN_SELECT]);
    draw_btn(12, 32, "Guide",  g_state.buttons[BTN_MODE]);
    draw_btn(12, 41, "Start",  g_state.buttons[BTN_START]);

    // Thumbstick clicks
    draw_btn(13, 2,  "L3", g_state.buttons[BTN_THUMBL]);
    draw_btn(13, 46, "R3", g_state.buttons[BTN_THUMBR]);

    // ── Divider ──────────────────────────────────────────────────────────────
    goto_xy(15, 1);
    std::printf(C_DIM "─────────────────────────────────────────────────────────────────────────────\n" RESET);

    // ── Event log ────────────────────────────────────────────────────────────
    goto_xy(16, 2);
    std::printf(C_LABEL BOLD "Recent events" RESET);
    for (int i = 0; i < 5; i++) {
        goto_xy(17 + i, 2);
        if (i < (int)g_state.log.size())
            std::printf(C_LABEL "%-76s" RESET, g_state.log[g_state.log.size()-1-i].c_str());
        else
            std::printf("%-76s", "");
    }

    // ── Footer ───────────────────────────────────────────────────────────────
    goto_xy(23, 1);
    std::printf(C_DIM " Ctrl+C to quit" RESET);

    std::fflush(stdout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Auto-detect
// ─────────────────────────────────────────────────────────────────────────────
std::optional<std::string> find_gamepad() {
    DIR *dir = opendir("/dev/input");
    if (!dir) return std::nullopt;
    std::vector<std::string> cands;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
        if (std::string(ent->d_name).rfind("event", 0) == 0)
            cands.push_back("/dev/input/" + std::string(ent->d_name));
    closedir(dir);
    std::sort(cands.begin(), cands.end());
    for (const auto &p : cands) {
        int fd = open(p.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long evbits = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits);
        if (((evbits >> EV_KEY) & 1) && ((evbits >> EV_ABS) & 1)) {
            unsigned long kb[KEY_CNT/(8*sizeof(unsigned long))+1] = {};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(kb)), kb);
            for (int b : {BTN_A,BTN_B,BTN_X,BTN_Y,BTN_START,BTN_SELECT}) {
                if ((kb[b/(8*sizeof(unsigned long))] >> (b%(8*sizeof(unsigned long)))) & 1)
                { close(fd); return p; }
            }
        }
        close(fd);
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// evdev loop
// ─────────────────────────────────────────────────────────────────────────────
void read_evdev(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(("open " + path).c_str()); return; }
    char devname[256] = "Unknown";
    ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);
    g_state.name = devname;

    clear_screen();
    hide_cursor();
    render();

    struct input_event ev;
    while (g_running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n < (ssize_t)sizeof(ev)) continue;

        char log_buf[128];

        if (ev.type == EV_KEY) {
            g_state.buttons[ev.code] = ev.value != 0;
            std::snprintf(log_buf, sizeof(log_buf),
                          "Button %-8s %s",
                          btn_name(ev.code).c_str(),
                          ev.value ? "▼ pressed" : "▲ released");
            g_state.push_log(log_buf);
        } else if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_X:     g_state.lx = ev.value; break;
                case ABS_Y:     g_state.ly = ev.value; break;
                case ABS_RX:    g_state.rx = ev.value; break;
                case ABS_RY:    g_state.ry = ev.value; break;
                case ABS_Z:     g_state.lt = ev.value;
                std::snprintf(log_buf, sizeof(log_buf), "L2 trigger  %d%%", ev.value*100/255);
                g_state.push_log(log_buf); break;
                case ABS_RZ:    g_state.rt = ev.value;
                std::snprintf(log_buf, sizeof(log_buf), "R2 trigger  %d%%", ev.value*100/255);
                g_state.push_log(log_buf); break;
                case ABS_HAT0X: g_state.dpx = ev.value; break;
                case ABS_HAT0Y: g_state.dpy = ev.value; break;
                default: break;
            }
        }
        render();
    }
    close(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// jsdev loop
// ─────────────────────────────────────────────────────────────────────────────
void read_jsdev(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(("open " + path).c_str()); return; }
    char devname[256] = "Unknown";
    ioctl(fd, JSIOCGNAME(sizeof(devname)), devname);
    g_state.name = devname;

    clear_screen();
    hide_cursor();
    render();

    struct js_event ev;
    while (g_running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < 0) { if (errno == EINTR) continue; break; }
        ev.type &= ~JS_EVENT_INIT;

        char log_buf[128];
        if (ev.type == JS_EVENT_BUTTON) {
            // Map js button numbers to evdev codes (common XInput layout)
            static const int js_to_btn[] = {
                BTN_A, BTN_B, BTN_X, BTN_Y,
                BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
                BTN_MODE, BTN_THUMBL, BTN_THUMBR
            };
            int code = (ev.number < 11) ? js_to_btn[ev.number] : BTN_A;
            g_state.buttons[code] = ev.value != 0;
            std::snprintf(log_buf, sizeof(log_buf),
                          "Button %-8s %s",
                          btn_name(code).c_str(),
                          ev.value ? "▼ pressed" : "▲ released");
            g_state.push_log(log_buf);
        } else if (ev.type == JS_EVENT_AXIS) {
            switch (ev.number) {
                case 0: g_state.lx = ev.value; break;
                case 1: g_state.ly = ev.value; break;
                case 2: g_state.lt = (ev.value + 32767) * 255 / 65534; break;
                case 3: g_state.rx = ev.value; break;
                case 4: g_state.ry = ev.value; break;
                case 5: g_state.rt = (ev.value + 32767) * 255 / 65534; break;
                case 6: g_state.dpx = (ev.value < -16000) ? -1 : (ev.value > 16000) ? 1 : 0; break;
                case 7: g_state.dpy = (ev.value < -16000) ? -1 : (ev.value > 16000) ? 1 : 0; break;
                default: break;
            }
        }
        render();
    }
    close(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    std::signal(SIGINT,  on_sigint);
    std::signal(SIGTERM, on_sigint);

    std::string path;
    if (argc >= 2) {
        path = argv[1];
    } else {
        std::cout << "Auto-detecting gamepad...\n";
        auto found = find_gamepad();
        if (!found) {
            std::cerr << "No gamepad found. Plug in your controller and retry,\n"
            << "or pass the device path:\n"
            << "  " << argv[0] << " /dev/input/eventN\n"
            << "  " << argv[0] << " /dev/input/jsN\n";
            return 1;
        }
        path = *found;
    }

    if (path.find("/dev/input/js") != std::string::npos)
        read_jsdev(path);
    else
        read_evdev(path);

    // Cleanup
    show_cursor();
    clear_screen();
    std::cout << "Bye!\n";
    return 0;
}
