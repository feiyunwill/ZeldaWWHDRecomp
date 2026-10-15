// Mod charts and their sea chart markers (runtime/src/mods/story_chart.*): the shared save record, the
// services and the sea chart menu hooks on a synthetic menu with synthetic panes and save. The game's
// own functions are stubbed; no game files are needed.
#include "mods/story_chart.h"
#include "mods/story_mail_book.h"
#include "mods/story_store.h"
#include "runtime.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace story_chart;

void log_msg(const char* fmt, ...) {
    (void)fmt;
}

namespace {

// synthetic guest memory: the save pointer, the save, a sea chart menu and its panes
constexpr uint32_t kGlobals = 0x101F0000, kArena = 0x20000000;
constexpr uint32_t kSave = kArena, kMenu = kArena + 0x2000, kMain = kArena + 0x3000, kPanes = kArena + 0x4000;
constexpr uint32_t kPaneStride = 0x100;

void* map(uint32_t at, uint32_t size) {
#ifdef _WIN32
    return VirtualAlloc(mem::ptr(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return mmap(mem::ptr(at), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif
}

uint32_t pane(unsigned i) { return kPanes + i * kPaneStride; }
float tx(unsigned i) { return u32_as_f32(ld32(pane(i) + 0x1C)); }
float ty(unsigned i) { return u32_as_f32(ld32(pane(i) + 0x20)); }
bool visible(unsigned i) { return ld8(pane(i) + 0x44) & 1; }
bool dirty(unsigned i) { return ld8(pane(i) + 0x44) & 0x10; }
void clear_dirty() {
    for (unsigned i = 0; i < 8; i++) st8(pane(i) + 0x44, ld8(pane(i) + 0x44) & ~0x10);
}
// the layout's places of the story marks (made up)
float home_x(unsigned i) { return 10.0f * float(i + 1); }
float home_y(unsigned i) { return -5.0f * float(i + 1); }
void reset_pane(unsigned i) {  // the pane as the layout made it
    st32(pane(i) + 0x1C, f32_as_u32(home_x(i)));
    st32(pane(i) + 0x20, f32_as_u32(home_y(i)));
    st8(pane(i) + 0x44, 0x02);
}
void build_menu() {
    for (unsigned i = 0; i < 8; i++) {
        st32(kMenu + 0x50 + 4 * i, pane(i));
        st32(pane(i) + 0x1C, f32_as_u32(home_x(i)));
        st32(pane(i) + 0x20, f32_as_u32(home_y(i)));
        st8(pane(i) + 0x44, 0x02);  // hidden, "influenced alpha" as the game's
    }
    st32(kMenu + 0x70, kMain);
    st32(kMain + 0x1C, f32_as_u32(71.0f));    // the chart's center (square D4)
    st32(kMain + 0x20, f32_as_u32(-44.0f));
    st32(kMain + 0x3C, f32_as_u32(511.0f));   // 7 squares of 73
    st32(kMain + 0x40, f32_as_u32(511.0f));
}

// the game's code
std::vector<unsigned> game_shows;  // story marks the game's update shows
bool game_busy = false;
std::vector<std::string> calls;
std::map<uint32_t, void (*)(Cpu*)> hooked;
void original(Cpu* c, uint32_t fn) {
    switch (fn) {
    case 0x02678B74:  // updateProgressIcons: busy -> hideMenuPanes, else the story's marks
        assert(c->r[3] == kMenu);
        // the game always finds its panes in place: a pane is either the game's (shown) or hidden
        for (unsigned i = 0; i < 8; i++) assert(tx(i) == home_x(i) && ty(i) == home_y(i));
        calls.push_back("progress");
        if (game_busy) {
            hooked.at(0x02678B00)(c);
            break;
        }
        for (unsigned i : game_shows) st8(pane(i) + 0x44, ld8(pane(i) + 0x44) | 1);
        break;
    case 0x02678B00:
        calls.push_back("hide");
        for (unsigned i = 0; i < 8; i++) st8(pane(i) + 0x44, ld8(pane(i) + 0x44) & ~1);
        break;
    case 0x02677B90: calls.push_back("destroy"); break;
    case 0x026396FC: calls.push_back("balloon"); break;  // hdBtnMapIcon_c::update: the game placed its circle
    case 0x026815A8: case 0x026834A4: break;  // the sea chart grid (test_circles has its own stand-in)
    default: assert(!"unexpected original");
    }
}
uint32_t game_call(Cpu*, uint32_t fn, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a(args);
    assert(a.size() == 3 && a[1] == 13 && a[2] == 7);
    if (fn == 0x02005708) calls.push_back("circle shown");
    else if (fn == 0x02005674) calls.push_back("circle hidden");
    else assert(!"unexpected game call");
    return 0;
}
void update() {
    Cpu c{};
    c.r[3] = kMenu;
    hooked.at(0x02678B74)(&c);
}
// a story mark of the game hides when its step is done (the game's own visibility writes)
void game_hides(unsigned i) {
    std::erase(game_shows, i);
    st8(pane(i) + 0x44, ld8(pane(i) + 0x44) & ~1);
}
bool close_to(float a, float b) { return std::fabs(a - b) < 0.01f; }

void test_record() {
    uint8_t raw[story_mail::kBookSize] = {};
    story_mail::Book b(raw);
    const uint16_t chart = chart_key("mod", "tale"), letter = story_mail::letter_key("mod", "tale");
    assert(chart && chart_key("mod", "tale") == chart && chart_key("other", "tale") != chart);
    // letters and charts share the book; the record kind keeps them apart (same key, different kinds)
    assert(letter == chart);
    assert(b.set_state(letter, story_mail::kWaiting) && b.set(story_mail::kRecordMarker, chart, kChartOwned));
    assert(b.state(letter) == story_mail::kWaiting && b.get(story_mail::kRecordMarker, chart) == kChartOwned);
    assert(raw[0] == story_mail::kMagic && raw[1] == 2);
    // second record: kind 3, the key big-endian, value 1
    assert(raw[8] == 3 && raw[9] == uint8_t(chart >> 8) && raw[10] == uint8_t(chart) && raw[11] == 1);
    assert(b.set(story_mail::kRecordMarker, chart, kChartDone) && raw[11] == 2 && b.state(letter) == story_mail::kWaiting);
}

void test_services() {
    assert(state("mod", "tale") == kResultNone && give("mod", "tale") == kResultNoRoom);  // no save loaded
    st32(0x101F84DC, kSave);
    assert(give("mod", "Tale") == kResultInvalid && give("mod", "") == kResultInvalid);
    assert(give("mod", "tale") == kResultOwned && give("mod", "tale") == kResultOwned);  // idempotent
    assert(state("mod", "tale") == kResultOwned && state("other", "tale") == kResultNone);
    story_mail::Book b = story_store::book();
    assert(b.get(story_mail::kRecordMarker, chart_key("mod", "tale")) == kChartOwned);
    // markers: checked arguments, add or move, per-mod limit
    assert(mark("mod", "tale", "start", 0, -200000, kIconStory, "Windfall") == kResultOwned);
    assert(mark("mod", "tale", "Start", 0, 0, kIconStory, "") == kResultInvalid);
    assert(mark("mod", "tale", "x", 0, 0, 7, "") == kResultInvalid);                    // unknown icon
    assert(mark("mod", "tale", "x", 400000, 0, kIconStory, "") == kResultInvalid);      // off the sea
    assert(mark("mod", "tale", "x", NAN, 0, kIconStory, "") == kResultInvalid);
    assert(mark("mod", "tale", "x", 0, 0, kIconStory, "two\nlines") == kResultInvalid);
    assert(mark("mod", "tale", "x", 0, 0, kIconStory, std::string(kMaxLabelBytes + 1, 'a')) == kResultInvalid);
    assert(mark("mod", "later", "x", 100000, 0, kIconStory, "") == kResultNone);  // a chart not given yet
    for (unsigned i = 2; i < kMaxMarkersPerMod; i++)
        assert(mark("mod", "later", "m" + std::to_string(i), 0, 0, kIconStory, "") == kResultNone);
    assert(mark("mod", "later", "one-more", 0, 0, kIconStory, "") == kResultNoRoom);
    assert(mark("mod", "tale", "start", 0, -200000, kIconStory, "Windfall Island") == kResultOwned);  // a move is no new marker
    // like the game's treasure charts: nothing shows while the chart is only owned
    assert(shown_markers().empty());
    assert(unmark("mod", "later", "") == kResultNone);  // all of a chart
    assert(unmark("mod", "tale", "nothing") == kResultOwned);
}

// the notebook's tracked quest (its step's place), the source of the white ring and the balloon's circle
bool tracked_on = false;
float tracked_x = 0.0f, tracked_z = -200000.0f;
bool tracked_source(float& x, float& z, std::string& label) {
    if (!tracked_on) return false;
    x = tracked_x, z = tracked_z, label = "Quest";
    return true;
}

void test_hooks() {
    // install: all or nothing
    set_game_calls(original, game_call);
    install([](uint32_t) { return std::string(); }, [](uint32_t, void (*)(Cpu*)) { assert(!"not imported"); });
    assert(!installed());
    note_import();
    install([](uint32_t a) { return a == 0x02678B00 ? std::string("other") : std::string(); },
            [](uint32_t, void (*)(Cpu*)) { assert(!"installed despite a conflict"); });
    assert(!installed());
    install([](uint32_t) { return std::string(); }, [](uint32_t a, void (*fn)(Cpu*)) { hooked[a] = fn; });
    assert(installed() && hooked.size() == 6);

    build_menu();
    set_tracked_source(tracked_source);
    // an owned chart's marker: no ring (the game shows nothing for an owned treasure chart)
    game_shows = {6};
    update();
    for (unsigned i = 0; i < 8; i++) assert(i == 6 || !visible(i));
    // the tracked quest's ring: the first pane the game left hidden
    tracked_on = true;
    update();
    assert(visible(6) && close_to(tx(6), home_x(6)));
    assert(visible(0) && dirty(0) && close_to(tx(0), 71.0f) && close_to(ty(0), -44.0f + 2 * 73.0f));  // square D2
    for (unsigned i = 1; i < 8; i++) assert(i == 6 || !visible(i));
    // every update puts the pane back first (checked in the stub), then shows the ring again
    clear_dirty();
    update();
    assert(visible(0) && dirty(0) && close_to(ty(0), 102.0f));
    // the game's own mark wins: the story now asks for pane 0
    game_shows = {0, 6};
    update();
    assert(visible(0) && close_to(tx(0), home_x(0)) && visible(1) && close_to(tx(1), 71.0f) && close_to(ty(1), 102.0f));
    game_hides(0);
    game_hides(6);
    // the ring moves with the step
    tracked_x = 100000, tracked_z = 0;
    update();
    assert(close_to(tx(0), 71.0f + 73.0f) && close_to(ty(0), -44.0f) && visible(0) && !visible(1));
    // busy screen: the game hides its marks and so does the ring
    game_busy = true;
    update();
    for (unsigned i = 0; i < 8; i++) assert(!visible(i) && close_to(tx(i), home_x(i)));
    game_busy = false;
    update();
    assert(visible(0));
    // every pane the game's: the ring waits
    game_shows = {0, 1, 2, 3, 4, 5, 6, 7};
    update();
    for (unsigned i = 0; i < 8; i++) assert(close_to(tx(i), home_x(i)));
    for (unsigned i = 0; i < 8; i++) game_hides(i);
    // a full state is captured with the panes as the game left them
    update();
    capture_begin();
    for (unsigned i = 0; i < 8; i++) assert(!visible(i) && close_to(tx(i), home_x(i)));
    capture_end();
    assert(visible(0) && close_to(tx(0), 71.0f + 73.0f));
    // a state load replaces guest memory: nothing is put back into it
    const uint32_t moved = ld32(pane(0) + 0x1C);
    state_loaded();
    capture_begin();
    assert(ld32(pane(0) + 0x1C) == moved);  // untouched: not ours any more
    reset_pane(0);  // (the loaded state's own pane)
    update();
    assert(visible(0) && close_to(tx(0), 71.0f + 73.0f));
    // a destroyed menu: its panes are forgotten
    Cpu c{};
    c.r[3] = kMenu;
    calls.clear();
    hooked.at(0x02677B90)(&c);
    assert(calls == std::vector<std::string>{"destroy"});
    capture_begin();
    assert(visible(0));  // nothing written into the freed menu
    // untracked: no ring
    reset_pane(0);
    tracked_on = false;
    update();
    for (unsigned i = 0; i < 8; i++) assert(!visible(i));
    set_tracked_source(nullptr);
}

// ---- finished charts: the game's red circle (a salvaged treasure's mark) on the sea chart grid ----
constexpr uint32_t kGrid = kArena + 0x8000, kOwner = kArena + 0x8400, kTouch = kArena + 0x8600, kCells0 = kArena + 0x9000,
                   kCirclePanes = kArena + 0xA000, kCellAnims = kArena + 0x8800;
constexpr uint32_t kCellStride = 0x40, kCirclePaneStride = 0x50;
uint32_t cell(unsigned i) { return kCells0 + i * kCellStride; }
uint32_t circle(unsigned i) { return kCirclePanes + i * kCirclePaneStride; }
float cx(unsigned i) { return u32_as_f32(ld32(circle(i) + 0x1C)); }
float cy(unsigned i) { return u32_as_f32(ld32(circle(i) + 0x20)); }
bool complete[49];
std::vector<std::string> circle_calls;  // "show 12" / "hide 12": the cells' animations run
// the game's placement of square i's circle (made up salvage points)
float game_x(unsigned i) { return 1000.0f + float(i); }
float game_y(unsigned i) { return 2000.0f + float(i); }
void game_layout() {  // updateChartMarkLayout: every circle in place, shown if complete, else hidden
    for (unsigned i = 0; i < 49; i++) {
        st32(circle(i) + 0x1C, f32_as_u32(game_x(i)));
        st32(circle(i) + 0x20, f32_as_u32(game_y(i)));
        st8(cell(i) + 0x10, complete[i] ? 1 : 0);
        st8(circle(i) + 0x44, complete[i] ? 1 : 0);  // (the stand-in for the animation's visibility)
    }
}
void build_grid() {
    for (unsigned i = 0; i < 49; i++) {
        st32(kGrid + 4 * i, cell(i));
        st8(cell(i) + 4, uint8_t(int8_t(int(i % 7) - 3)));  // the square: x -3..3, y -3..3
        st8(cell(i) + 5, uint8_t(int8_t(int(i / 7) - 3)));
        st32(cell(i) + 0x54, kCellAnims);
        st32(cell(i) + 0x5C, 49 + i);  // its TreasureGet animation entry and slot
        st32(cell(i) + 0x6C, 50 + i);
        st32(kOwner + 0x84 + 4 * i, circle(i));
    }
    st32(kGrid + 0x10C, kOwner);
    st32(kGrid + 0xC8, kTouch);
    st32(kTouch + 0x1C, f32_as_u32(10.0f)), st32(kTouch + 0x20, f32_as_u32(20.0f));
    st32(kTouch + 0x3C, f32_as_u32(700.0f)), st32(kTouch + 0x40, f32_as_u32(350.0f));  // squares of 100 x 50
}
uint32_t grid_call(Cpu*, uint32_t fn, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a(args);
    assert(a.size() == 3);
    if (a[0] == kCellAnims) {  // a cell's circle
        const unsigned i = a[1] - 49;
        assert(i < 49 && a[2] == 50 + i);
        if (fn == 0x02005708) circle_calls.push_back("show " + std::to_string(i)), st8(circle(i) + 0x44, 1);
        else if (fn == 0x02005674) circle_calls.push_back("hide " + std::to_string(i)), st8(circle(i) + 0x44, 0);
        else assert(!"unexpected game call");
        return 0;
    }
    return game_call(nullptr, fn, args);
}
void grid_original(Cpu* c, uint32_t fn) {
    if (fn == 0x026815A8) {
        assert(c->r[3] == kGrid);
        game_layout();
    } else if (fn == 0x026834A4) {  // refreshChartMarks: a square salvaged since shows its circle
        assert(c->r[3] == kGrid);
        for (unsigned i = 0; i < 49; i++)
            if (complete[i] && !ld8(cell(i) + 0x10)) {
                st8(cell(i) + 0x10, 1), st8(circle(i) + 0x44, 1);
                circle_calls.push_back("game shows " + std::to_string(i));
            }
    } else original(c, fn);
}
void grid_hook(uint32_t fn) {
    Cpu c{};
    c.r[3] = kGrid;
    hooked.at(fn)(&c);
}
unsigned shown_circles() {
    unsigned n = 0;
    for (unsigned i = 0; i < 49; i++) n += ld8(circle(i) + 0x44) & 1;
    return n;
}
void test_circles() {
    set_game_calls(grid_original, grid_call);
    build_grid();
    complete[3 * 7 + 4] = true;  // the game's own: square 1,0 salvaged
    grid_hook(0x026815A8);
    grid_hook(0x026834A4);
    assert(shown_circles() == 1);
    // an owned chart's marker in square 0,-2: nothing
    assert(give("map", "old") == kResultOwned && mark("map", "old", "spot", 1547, -202539, kIconStory, "Spot") == kResultOwned);
    circle_calls.clear();
    grid_hook(0x026834A4);
    assert(circle_calls.empty() && shown_circles() == 1);
    // finished: the red circle of its own square (cell 0,-2 = 1 * 7 + 3), at the spot
    assert(finish("map", "old") == kResultDone);
    auto shown = shown_markers();
    assert(shown.size() == 1 && shown[0].done && shown[0].marker == "spot");
    grid_hook(0x026834A4);
    const unsigned own = 1 * 7 + 3;
    assert((circle_calls == std::vector<std::string>{"show " + std::to_string(own)}));
    // x: 10 + 1547 / 100000 * 100 = 11.547; y: 20 - (-202539 / 100000) * 50 = 121.2695
    assert(close_to(cx(own), 11.547f) && close_to(cy(own), 121.2695f) && shown_circles() == 2);
    // every update: placed, shown once
    circle_calls.clear();
    grid_hook(0x026834A4);
    assert(circle_calls.empty() && close_to(cx(own), 11.547f));
    // the game's layout pass resets every circle; the next refresh shows the marker's again
    grid_hook(0x026815A8);
    assert(close_to(cx(own), game_x(own)) && shown_circles() == 1);
    grid_hook(0x026834A4);
    assert(close_to(cx(own), 11.547f) && shown_circles() == 2);
    // the game wants that square's circle (its treasure salvaged): the game's, at its place; the marker
    // takes another square's circle
    complete[own] = true;
    circle_calls.clear();
    grid_hook(0x026834A4);
    assert(close_to(cx(own), game_x(own)) && close_to(cy(own), game_y(own)) && ld8(circle(own) + 0x44));
    assert((circle_calls == std::vector<std::string>{"game shows " + std::to_string(own), "show 0"}));
    assert(close_to(cx(0), 11.547f) && shown_circles() == 3);
    complete[own] = false;
    grid_hook(0x026815A8);
    grid_hook(0x026834A4);
    assert(close_to(cx(own), 11.547f) && close_to(cx(0), game_x(0)));
    // a full state: the circles at the game's places (their visibility is healed after a load)
    capture_begin();
    assert(close_to(cx(own), game_x(own)) && close_to(cy(own), game_y(own)));
    capture_end();
    assert(close_to(cx(own), 11.547f));
    state_loaded();
    st32(circle(own) + 0x1C, f32_as_u32(game_x(own)));  // the state as captured: the circle shown at the game's place
    circle_calls.clear();
    unmark("map", "old", "spot");
    grid_hook(0x026834A4);
    unsigned hides = 0;
    for (const std::string& s : circle_calls) hides += s.rfind("hide ", 0) == 0;
    assert(hides == 49 - 1 && shown_circles() == 1);  // only the game's salvaged square
    // a removed marker gives its circle back
    assert(mark("map", "old", "spot", 1547, -202539, kIconStory, "Spot") == kResultDone);
    grid_hook(0x026834A4);
    assert(shown_circles() == 2);
    circle_calls.clear();
    unmark("map", "old", "");
    grid_hook(0x026834A4);
    assert((circle_calls == std::vector<std::string>{"hide " + std::to_string(own)}) && close_to(cx(own), game_x(own)) && shown_circles() == 1);
    set_game_calls(original, game_call);
}

}  // namespace

// the zoomed square balloon: the tracked step, else a finished chart's marker in that square, borrows the
// game's red circle while the game doesn't show it, at the spot, with the game's mapping; it gives it back
void test_balloon() {
    constexpr uint32_t kIcon = kArena + 0x6000, kOwner = kArena + 0x6400, kInfo = kArena + 0x6800, kCircle = kArena + 0x6C00,
                       kParent = kArena + 0x7000, kLayout = kArena + 0x7400, kAnims = kArena + 0x7800;
    st32(kIcon + 0x164, kOwner), st32(kOwner + 0x118, kInfo);
    st8(kInfo + 4, 0), st8(kInfo + 5, uint8_t(-2));         // Windfall's square: cell 0,-2
    st16(kInfo + 6, uint16_t(int16_t(-500))), st16(kInfo + 8, 1000);  // the island's point in it
    st32(kIcon + 0xA8, kCircle), st32(kIcon + 0x94, kParent), st32(kIcon + 0x44, kLayout), st32(kLayout + 0xD4, kAnims);
    st32(kParent + 0x34, f32_as_u32(2.0f)), st32(kParent + 0x38, f32_as_u32(2.0f));
    st32(kIcon + 0x150, f32_as_u32(0.01f)), st32(kIcon + 0x154, f32_as_u32(0.01f));
    st32(kIcon + 0x144, 1);  // zoomed in (detail)
    tracked_on = true, tracked_x = 1547.0f, tracked_z = -202539.0f;
    set_tracked_source(tracked_source);
    auto run = [&] {
        Cpu c{};
        c.r[3] = kIcon;
        hooked.at(0x026396FC)(&c);
    };
    calls.clear();
    run();
    // (1547 - 0 + 500) * 0.01 * 2 = 40.94; -((-202539 + 200000 - 1000) * 0.01 * 2) = 70.78
    // (the first update of a balloon after a state load (test_hooks) hides the circle the state may hold)
    assert((calls == std::vector<std::string>{"balloon", "circle hidden", "circle shown"}));
    assert(close_to(u32_as_f32(ld32(kCircle + 0x1C)), 40.94f) && close_to(u32_as_f32(ld32(kCircle + 0x20)), 70.78f));
    calls.clear();
    run();
    assert((calls == std::vector<std::string>{"balloon"}));  // shown once, placed every update
    // another square: the circle is the game's again
    st8(kInfo + 4, 1);
    calls.clear();
    run();
    assert((calls == std::vector<std::string>{"balloon", "circle hidden"}));
    st8(kInfo + 4, 0);
    run();
    // the game shows its own circle (that square's treasure is salvaged): never touched
    st8(kIcon + 0x158, 1);
    st32(kCircle + 0x1C, f32_as_u32(5.0f));
    calls.clear();
    run();
    assert((calls == std::vector<std::string>{"balloon"}) && close_to(u32_as_f32(ld32(kCircle + 0x1C)), 5.0f));
    st8(kIcon + 0x158, 0);
    // untracked: a finished chart's marker in this square has the circle; an owned one has not
    tracked_on = false;
    assert(give("bal", "c") == kResultOwned && mark("bal", "c", "m", 2047, -201539, kIconStory, "") == kResultOwned);
    calls.clear();
    run();
    assert((calls == std::vector<std::string>{"balloon"}));
    assert(finish("bal", "c") == kResultDone);
    calls.clear();
    run();
    // (2047 + 500) * 0.02 = 50.94; -((-201539 + 200000 - 1000) * 0.02) = 50.78
    assert((calls == std::vector<std::string>{"balloon", "circle shown"}));
    assert(close_to(u32_as_f32(ld32(kCircle + 0x1C)), 50.94f) && close_to(u32_as_f32(ld32(kCircle + 0x20)), 50.78f));
    // the tracked step comes first
    tracked_on = true;
    run();
    assert(close_to(u32_as_f32(ld32(kCircle + 0x1C)), 40.94f));
    tracked_on = false;
    unmark("bal", "c", "");
    calls.clear();
    run();
    assert((calls == std::vector<std::string>{"balloon", "circle hidden"}));
    // a state load: a circle the state holds is hidden once, if the game doesn't show it
    state_loaded();
    calls.clear();
    run();
    run();
    assert((calls == std::vector<std::string>{"balloon", "circle hidden", "balloon"}));
    set_tracked_source(nullptr);
}

int main() {
    assert(map(kGlobals, 0x10000) && map(kArena, 0x10000));
    story_store::set_memory(kArena + 0xE000);
    test_record();
    test_services();
    test_hooks();
    test_circles();
    test_balloon();
    std::puts("story_chart_test: ok");
    return 0;
}
