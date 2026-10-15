// Mod charts and their markers on the sea chart (story_chart.h, docs/mod-sdk-v2.md "Charts and markers").
//
// How the game marks story destinations (HD decomp MAP-sea_chart#1, hd_screen_sea_chart_menu.cpp;
// GameCube: dMenu_Fmap_c::checkMarkCheck1/2/3):
//   - The sea chart menu (hdSeaChartMenu_c, the MAP screen of the GamePad, also shown on the TV) has
//     eight story-mark panes (mpMenuPanes, +0x50): null panes holding the white ring the game puts on
//     the island to go to next (Dragon Roost, Forest Haven, Greatfish, the three triangle islands, the
//     Earth and Wind Temples). Their places are fixed in the layout; the game never moves them.
//   - updateProgressIcons() (02678B74) shows the ones the story asks for, from event flags, every
//     update. While the screen is busy (opening, zooming, closing), it calls hideMenuPanes()
//     (02678B00) instead, which hides all eight.
//   - The chart area is the main pane (+0x70): the 7 x 7 squares, square D4 at its position, one
//     square (100000 world units) = its size / 7, y up (the game places Link's marker the same way,
//     updateMarkerPanePosition).
//   - Treasure and other charts are save bits (dSv_player_map_c: owned, opened, complete). The sea
//     chart shows nothing for an owned treasure chart; once its treasure is salvaged (complete), the
//     game's red hand-drawn circle (P_TreasureGet_NN, one per square) marks the salvage spot
//     (hdSeaChartGrid_c::updateChartMarkLayout 026815A8 places it, refreshChartMarks 026834A4 shows
//     it through the cell's "TreasureGet" animation). A mod chart has no game item: its ownership is
//     a record of the story-mod book (story_mail_book.h).
//
// The tracked quest's ring: after the game's updateProgressIcons, the first story-mark pane the game
// is not showing carries the notebook's tracked step: it is moved there and shown. Before the game's
// next update the pane goes back to its place and is hidden again, so the game always finds its panes
// as it left them and the game's own marks win.
//
// Mod chart markers behave like the game's treasure charts: nothing on the sea chart while the chart
// is only owned; once the mod marks the chart done (wwhd_chart_finish), the game's red circle at each
// marker's spot. The circle is the TreasureGet pane of a square whose own circle the game does not
// show (the marker's own square first): after the game's refreshChartMarks it is moved to the spot
// and shown with its own animation; it goes back when the marker goes or the game wants that square's
// circle. The zoomed square (the balloon) shows the circle of a finished chart's marker in its square
// the same way. Full save states never contain a moved pane (capture_begin/capture_end); a circle
// shown at a state's capture is hidden again after the state is loaded (heal).
#include "story_chart.h"
#include "story_mail_book.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <cmath>
#include <mutex>

#include "guest_addr.h"
#include "story_store.h"
#include "runtime.h"

namespace story_chart {
namespace {

// canonical USA addresses, translated for the running build (guest_addr.h)
const uint32_t kSavePtr = GD(0x101F84DC);     // save base pointer (d_com_inf_game)
const uint32_t kProgress = GC(0x02678B74);    // hdSeaChartMenu_c::updateProgressIcons
const uint32_t kHidePanes = GC(0x02678B00);   // hdSeaChartMenu_c::hideMenuPanes
const uint32_t kDestroy = GC(0x02677B90);     // hdSeaChartMenu_c::destroy(flags)
const uint32_t kMarkLayout = GC(0x026815A8);  // hdSeaChartGrid_c::updateChartMarkLayout
const uint32_t kMarkRefresh = GC(0x026834A4); // hdSeaChartGrid_c::refreshChartMarks (every update)
// hdSeaChartGrid_c (hd_sea_chart_components.h): its 49 cells, the chart area, the owner of the panes
constexpr uint32_t kGridCells = 0, kGridTouchPane = 0xC8, kGridMapOwner = 0x10C, kOwnerMarkPanes = 0x84;
constexpr unsigned kCells = 49;
// hdSeaChartCell_c: its square (s8 x, y), whether its circle shows, its animation set, entry and slot
constexpr uint32_t kCellCoord = 4, kCellResource = 0xC, kCellMarkActive = 0x10, kCellAnims = 0x54, kCellMarkEntry = 0x5C,
                   kCellMarkSlot = 0x6C;
// The zoomed square balloon (hdBtnMapIcon_c, layout BtnMapBalloon_00): its P_TreasureGet_00 pane is the
// game's red hand-drawn circle, which marks the salvage spot of that square's treasure chart once the
// treasure is salvaged (isSeaChartComplete 0268A7A0, position from findSeaChart 0268A720 and the save's
// random salvage point; the decomp names the functions refreshFish / updateFish / positionFishOnMap /
// positionFishInDetail;
// shown by stopping animation 13 slot 7 at its end, hidden by resetting it). While the game doesn't show
// it, the tracked quest's step borrows it for its exact spot in that square.
const uint32_t kIconUpdate = GC(0x026396FC);  // hdBtnMapIcon_c::update()
const uint32_t kAnimStop = GC(0x02005708);    // mapIconStop(animSet, anim, slot)
const uint32_t kAnimReset = GC(0x02005674);   // mapIconReset(animSet, anim, slot)
constexpr uint32_t kIconParent = 0x94, kIconTreasure = 0xA8, kIconPosition = 0xE4, kIconMapMode = 0x144,
                   kIconTranslateScale = 0x148, kIconPointScale = 0x150, kIconFishVisible = 0x158, kIconOwner = 0x164;
constexpr uint32_t kOwnerInfo = 0x118;              // HdMapIconOwner::mpInfo (HdMapInfo: +4 cell s8 x,y; +6 offset s16 x,y)
constexpr uint32_t kPartsLayout = 0x44, kLayoutAnimSet = 0xD4;
constexpr uint32_t kFishAnim = 13, kFishSlot = 7;
constexpr uint32_t kPaneScale = 0x34;
constexpr uint32_t kIconPrimary = 0xB8;               // L_LinkPos_00: Link's icon in the balloon (trace: checks the mapping)
const uint32_t kLinkPtr = GD(0x1046F0B0) + 0x5B34;  // g_dComIfG_gameInfo.play mpPlayerPtr[0]

// hdSeaChartMenu_c (hd_screen_sea_chart_menu.h) and library pane fields (hdPartsPaneSRT_c)
constexpr uint32_t kMenuPanes = 0x50, kMainPane = 0x70;
constexpr unsigned kStoryPanes = 8;
constexpr uint32_t kPaneTranslate = 0x1C, kPaneSize = 0x3C, kPaneFlags = 0x44;
constexpr uint8_t kVisible = 0x01, kMatrixDirty = 0x10;
constexpr float kSquaresPerSide = 7.0f;

struct Marker {
    std::string mod, chart, id, label;
    uint16_t key;  // the chart's record key
    float x, z;
    uint32_t icon;
};
// a story-mark pane carrying a marker
struct Applied {
    uint32_t pane;
    unsigned index;        // in the menu's story-mark panes
    uint32_t tx, ty;       // the game's place (raw bits)
    uint32_t mx, my;       // the marker's place (raw bits)
};

std::mutex g_mu;
std::vector<Marker> g_markers;  // placement order
std::map<uint32_t, std::vector<Applied>> g_applied;  // per menu (game thread)
bool g_imported = false, g_installed = false;
bool g_hidden = false;  // hideMenuPanes ran during the game's update
TrackedSource g_tracked = nullptr;
void (*g_original)(Cpu*, uint32_t) = nullptr;
uint32_t (*g_call)(Cpu*, uint32_t, std::initializer_list<uint32_t>) = nullptr;
std::map<uint32_t, uint32_t> g_borrowed;  // balloon -> its animation set, while a mod spot uses its circle
// a square's TreasureGet circle carrying a finished chart's marker
struct Circle {
    unsigned cell;   // whose pane and animation
    uint32_t pane;
    uint32_t tx, ty; // the game's place (raw bits)
    uint32_t mx, my; // the marker's place (raw bits)
};
std::map<uint32_t, std::vector<Circle>> g_circles;  // per sea chart grid (TV, GamePad, warp map)
bool g_heal_grids = false, g_heal_balloons = false;  // a state was loaded: circles of its capture may still show
std::vector<uint32_t> g_healed;                      // grids and balloons healed since

bool trace() {
    static const bool on = [] { const char* e = getenv("WWHD_CHART_TRACE"); return e && *e == '1'; }();
    return on;
}

uint32_t save_base() { return ld32(kSavePtr); }
bool have_save() { return save_base() != 0; }
story_mail::Book book() { return story_store::book(); }
// the chart's record (kind kRecordMarker): 1 owned, 2 finished, 0 none (story_chart.h)
uint8_t chart_value(const story_mail::Book& b, uint16_t key) { return b.get(story_mail::kRecordMarker, key); }
// the places of the markers whose chart is finished: the game's red circle marks them
struct Spot {
    float x, z;
};
std::vector<Spot> done_spots() {
    std::vector<Spot> out;
    if (!have_save()) return out;
    story_mail::Book b = book();
    if (!b.usable()) return out;
    std::lock_guard lock(g_mu);
    for (const Marker& m : g_markers)
        if (chart_value(b, m.key) == kChartDone) out.push_back({m.x, m.z});
    return out;
}
int32_t result_of(uint8_t v) { return v == kChartOwned ? kResultOwned : v == kChartDone ? kResultDone : kResultNone; }
bool valid_label(const std::string& label) {
    std::u16string ignored;
    return label.size() <= kMaxLabelBytes && story_mail::append_utf16(label, false, ignored);
}

// the panes of one menu back as the game left them: in place and hidden
void revert(uint32_t menu, const std::vector<Applied>& applied) {
    for (const Applied& a : applied) {
        if (ld32(menu + kMenuPanes + 4 * a.index) != a.pane) continue;  // not that menu's pane any more
        st32(a.pane + kPaneTranslate, a.tx);
        st32(a.pane + kPaneTranslate + 4, a.ty);
        st8(a.pane + kPaneFlags, uint8_t((ld8(a.pane + kPaneFlags) & ~kVisible) | kMatrixDirty));
    }
}
void show(const Applied& a) {
    st32(a.pane + kPaneTranslate, a.mx);
    st32(a.pane + kPaneTranslate + 4, a.my);
    st8(a.pane + kPaneFlags, uint8_t(ld8(a.pane + kPaneFlags) | kVisible | kMatrixDirty));
}

// the markers on the story-mark panes the game left hidden
void apply(uint32_t menu) {
    std::vector<Applied>& applied = g_applied[menu];
    applied.clear();
    const uint32_t main = ld32(menu + kMainPane);
    if (!main || !have_save()) return;
    story_mail::Book b = book();
    if (!b.usable()) return;
    struct Place {
        float x, z;
    };
    std::vector<Place> shown;
    bool tracked = false;
    if (Place t{}; g_tracked) {  // the notebook's tracked quest comes first
        std::string label;
        if (g_tracked(t.x, t.z, label)) shown.push_back(t), tracked = true;
    }
    if (shown.empty()) return;
    const float cx = float(ldf32(main + kPaneTranslate)), cy = float(ldf32(main + kPaneTranslate + 4));
    const float sx = float(ldf32(main + kPaneSize)) / kSquaresPerSide, sy = float(ldf32(main + kPaneSize + 4)) / kSquaresPerSide;
    size_t next = 0;
    for (unsigned i = 0; i < kStoryPanes && next < shown.size(); i++) {
        const uint32_t pane = ld32(menu + kMenuPanes + 4 * i);
        if (!pane || (ld8(pane + kPaneFlags) & kVisible)) continue;  // the game's own mark
        const Place& m = shown[next++];
        Applied a{pane, i, ld32(pane + kPaneTranslate), ld32(pane + kPaneTranslate + 4),
                  f32_as_u32(cx + m.x / kSquare * sx), f32_as_u32(cy - m.z / kSquare * sy)};
        show(a);
        applied.push_back(a);
    }
    // the tracked quest's ring, logged when it changes (not only with the trace): what the player sees
    {
        static std::string last_tracked;
        char now[128] = "";
        if (tracked && !applied.empty() && applied[0].index < kStoryPanes)
            snprintf(now, sizeof now, "the tracked quest's ring is on story pane %u at %.1f,%.1f", applied[0].index, u32_as_f32(applied[0].mx),
                     u32_as_f32(applied[0].my));
        else if (tracked)
            snprintf(now, sizeof now, "the tracked quest's ring waits: every story pane shows a mark of the game");
        if (last_tracked != now) {
            if (*now) LOG("[chart] %s", now);
            last_tracked = now;
        }
    }
    if (trace()) {
        static std::map<uint32_t, std::string> last;
        std::string now;
        for (const Applied& a : applied) {
            char line[96];
            snprintf(line, sizeof line, " pane %u at %.1f,%.1f;", a.index, u32_as_f32(a.mx), u32_as_f32(a.my));
            now += line;
        }
        if (next < shown.size()) now += " " + std::to_string(shown.size() - next) + " waiting for a free pane";
        if (last[menu] != now) LOG("[chart] menu %08X:%s", menu, now.empty() ? " no markers" : now.c_str());
        last[menu] = now;
    }
}

// a cell's circle: shown (the animation stopped at its end) or hidden (reset), as the game does it
// (hdSeaChartCell_c::resetMarkAnimation / activateMarkAnimation)
void cell_circle(const Cpu& keep, uint32_t cell, bool shown) {
    const uint32_t anims = ld32(cell + kCellAnims), entry = ld32(cell + kCellMarkEntry), slot = ld32(cell + kCellMarkSlot);
    if (!anims || entry == 0xFFFFFFFFu || slot == 0xFFFFFFFFu) return;
    Cpu call = keep;
    g_call(&call, shown ? kAnimStop : kAnimReset, {anims, entry, slot});
}
uint32_t grid_pane(uint32_t grid, unsigned i) {
    const uint32_t owner = ld32(grid + kGridMapOwner);
    return owner ? ld32(owner + kOwnerMarkPanes + 4 * i) : 0;
}
// a circle back as the game left it: at the game's place, hidden (unless the game shows it by now)
void give_back_circle(const Cpu& keep, uint32_t grid, const Circle& c) {
    const uint32_t cell = ld32(grid + kGridCells + 4 * c.cell);
    if (grid_pane(grid, c.cell) != c.pane) return;  // not that grid's pane any more
    st32(c.pane + kPaneTranslate, c.tx);
    st32(c.pane + kPaneTranslate + 4, c.ty);
    st8(c.pane + kPaneFlags, uint8_t(ld8(c.pane + kPaneFlags) | kMatrixDirty));
    if (cell && !ld8(cell + kCellMarkActive)) cell_circle(keep, cell, false);
}
// the finished charts' markers on the circles of squares whose own circle the game does not show
void place_circles(const Cpu& keep, uint32_t grid) {
    std::vector<Circle>& circles = g_circles[grid];
    const uint32_t touch = ld32(grid + kGridTouchPane);
    auto cell_at = [&](unsigned i) { return ld32(grid + kGridCells + 4 * i); };
    // the game shows a square's circle by now (its treasure was salvaged): the game's again, in place
    std::erase_if(circles, [&](const Circle& c) {
        const uint32_t cell = cell_at(c.cell);
        if (cell && !ld8(cell + kCellMarkActive) && grid_pane(grid, c.cell) == c.pane) return false;
        if (grid_pane(grid, c.cell) == c.pane) {
            st32(c.pane + kPaneTranslate, c.tx);
            st32(c.pane + kPaneTranslate + 4, c.ty);
            st8(c.pane + kPaneFlags, uint8_t(ld8(c.pane + kPaneFlags) | kMatrixDirty));
        }
        return true;
    });
    std::vector<Spot> spots = touch ? done_spots() : std::vector<Spot>{};
    // the chart area: 7 x 7 squares, the centre square at the area's position, y up
    // (updateChartMarkLayout: position = area + (square * 100000 + offset) * square size / 100000)
    const float ox = touch ? u32_as_f32(ld32(touch + kPaneTranslate)) : 0, oy = touch ? u32_as_f32(ld32(touch + kPaneTranslate + 4)) : 0;
    const float sx = touch ? u32_as_f32(ld32(touch + kPaneSize)) / kSquaresPerSide : 0;
    const float sy = touch ? u32_as_f32(ld32(touch + kPaneSize + 4)) / kSquaresPerSide : 0;
    std::vector<bool> used(kCells, false);
    std::vector<Circle> next;
    for (const Spot& m : spots) {
        const uint32_t mx = f32_as_u32(ox + m.x / kSquare * sx), my = f32_as_u32(oy - m.z / kSquare * sy);
        const int cx = int(std::lround(m.x / kSquare)), cz = int(std::lround(m.z / kSquare));
        auto free_cell = [&](unsigned i) {
            const uint32_t cell = cell_at(i);
            return cell && !used[i] && !ld8(cell + kCellMarkActive) && grid_pane(grid, i) &&
                   ld32(cell + kCellAnims) && ld32(cell + kCellMarkEntry) != 0xFFFFFFFFu;
        };
        int pick = -1;
        for (unsigned i = 0; i < kCells && pick < 0; i++)  // the marker's own square first
            if (free_cell(i) && int8_t(ld8(cell_at(i) + kCellCoord)) == cx && int8_t(ld8(cell_at(i) + kCellCoord + 1)) == cz) pick = int(i);
        for (const Circle& c : circles)  // else the circle it already has
            if (pick < 0 && free_cell(c.cell) && c.mx == mx && c.my == my) pick = int(c.cell);
        for (unsigned i = 0; i < kCells && pick < 0; i++)
            if (free_cell(i)) pick = int(i);
        if (pick < 0) continue;  // every circle is the game's: the marker waits
        used[pick] = true;
        const uint32_t pane = grid_pane(grid, unsigned(pick));
        auto had = std::find_if(circles.begin(), circles.end(), [&](const Circle& c) { return c.cell == unsigned(pick); });
        Circle c = had != circles.end() ? *had : Circle{unsigned(pick), pane, ld32(pane + kPaneTranslate), ld32(pane + kPaneTranslate + 4), 0, 0};
        c.mx = mx, c.my = my;
        if (ld32(pane + kPaneTranslate) != mx || ld32(pane + kPaneTranslate + 4) != my) {
            st32(pane + kPaneTranslate, mx);
            st32(pane + kPaneTranslate + 4, my);
            st8(pane + kPaneFlags, uint8_t(ld8(pane + kPaneFlags) | kMatrixDirty));
        }
        if (had == circles.end()) {
            cell_circle(keep, cell_at(unsigned(pick)), true);
            LOG("[chart] a finished chart's circle is on square %d,%d's circle at %.1f,%.1f (world %.0f,%.0f)",
                int8_t(ld8(cell_at(unsigned(pick)) + kCellCoord)), int8_t(ld8(cell_at(unsigned(pick)) + kCellCoord + 1)),
                u32_as_f32(mx), u32_as_f32(my), m.x, m.z);
        }
        next.push_back(c);
    }
    for (const Circle& c : circles)
        if (std::none_of(next.begin(), next.end(), [&](const Circle& n) { return n.cell == c.cell; })) give_back_circle(keep, grid, c);
    circles = std::move(next);
}

Marker* find(const std::string& mod, const std::string& chart, const std::string& id) {
    for (Marker& m : g_markers)
        if (m.mod == mod && m.chart == chart && m.id == id) return &m;
    return nullptr;
}

}  // namespace

// the same 16-bit hash as the letters' keys; kinds keep the two apart
uint16_t chart_key(const std::string& mod, const std::string& chart) { return story_mail::letter_key(mod, chart); }

int32_t give(const std::string& mod, const std::string& chart) {
    if (!story_mail::valid_id(chart)) return kResultInvalid;
    if (!have_save()) return kResultNoRoom;
    story_mail::Book b = book();
    const uint16_t key = chart_key(mod, chart);
    const uint8_t v = chart_value(b, key);
    if (v) return result_of(v);
    if (!b.set(story_mail::kRecordMarker, key, kChartOwned)) {
        LOG("[chart] %s: no room for chart \"%s\" in this save file", mod.c_str(), chart.c_str());
        return kResultNoRoom;
    }
    LOG("[chart] %s: Link got chart \"%s\"", mod.c_str(), chart.c_str());
    return kResultOwned;
}

int32_t finish(const std::string& mod, const std::string& chart) {
    if (!story_mail::valid_id(chart)) return kResultInvalid;
    if (!have_save()) return kResultNoRoom;
    story_mail::Book b = book();
    const uint16_t key = chart_key(mod, chart);
    const uint8_t v = chart_value(b, key);
    if (v == kChartDone) return kResultDone;
    if (v && v != kChartOwned) return kResultInvalid;  // a value this version doesn't know
    if (!b.set(story_mail::kRecordMarker, key, kChartDone)) return kResultNoRoom;
    LOG("[chart] %s: chart \"%s\" finished", mod.c_str(), chart.c_str());
    return kResultDone;
}

int32_t state(const std::string& mod, const std::string& chart) {
    if (!story_mail::valid_id(chart)) return kResultInvalid;
    if (!have_save()) return kResultNone;
    return result_of(chart_value(book(), chart_key(mod, chart)));
}

int32_t mark(const std::string& mod, const std::string& chart, const std::string& marker, float x, float z,
             uint32_t icon, const std::string& label) {
    if (!story_mail::valid_id(chart) || !story_mail::valid_id(marker) || icon != kIconStory || !valid_label(label))
        return kResultInvalid;
    if (!(x >= -kSeaHalf && x <= kSeaHalf && z >= -kSeaHalf && z <= kSeaHalf)) return kResultInvalid;  // NaN too
    {
        std::lock_guard lock(g_mu);
        if (Marker* m = find(mod, chart, marker)) {
            m->x = x, m->z = z, m->label = label;
        } else {
            unsigned mine = 0;
            for (const Marker& m : g_markers) mine += m.mod == mod;
            if (mine >= kMaxMarkersPerMod || g_markers.size() >= kMaxMarkers) return kResultNoRoom;
            g_markers.push_back({mod, chart, marker, label, chart_key(mod, chart), x, z, icon});
            if (trace()) LOG("[chart] %s: marker \"%s\" of chart \"%s\" at %.0f, %.0f", mod.c_str(), marker.c_str(), chart.c_str(), x, z);
        }
    }
    return state(mod, chart);
}

int32_t unmark(const std::string& mod, const std::string& chart, const std::string& marker) {
    if (!story_mail::valid_id(chart) || (!marker.empty() && !story_mail::valid_id(marker))) return kResultInvalid;
    {
        std::lock_guard lock(g_mu);
        std::erase_if(g_markers, [&](const Marker& m) { return m.mod == mod && m.chart == chart && (marker.empty() || m.id == marker); });
    }
    return state(mod, chart);
}

void set_tracked_source(TrackedSource source) { g_tracked = source; }

std::vector<MarkerView> shown_markers() {
    std::vector<MarkerView> out;
    if (!have_save()) return out;
    if (float x, z; g_tracked) {
        std::string label;
        if (g_tracked(x, z, label)) out.push_back({"", "notebook", "tracked", label, x, z, kIconStory});
    }
    story_mail::Book b = book();
    std::lock_guard lock(g_mu);
    for (const Marker& m : g_markers)
        if (chart_value(b, m.key) == kChartDone) out.push_back({m.mod, m.chart, m.id, m.label, m.x, m.z, m.icon, true});
    return out;
}

void note_import() { g_imported = true; }
bool imported() { return g_imported; }
bool installed() { return g_installed; }
void set_game_calls(void (*original)(Cpu*, uint32_t), uint32_t (*call)(Cpu*, uint32_t, std::initializer_list<uint32_t>)) {
    g_original = original;
    g_call = call;
}

void install(const std::function<std::string(uint32_t)>& replaced_by, const std::function<void(uint32_t, void (*)(Cpu*))>& replace) {
    if (!g_imported || g_installed || !g_original) return;
    const std::pair<uint32_t, void (*)(Cpu*)> hooks[] = {
        {kProgress, hooks::progress_icons}, {kHidePanes, hooks::hide_panes}, {kDestroy, hooks::destroy},
        {kIconUpdate, hooks::icon_update}, {kMarkLayout, hooks::mark_layout}, {kMarkRefresh, hooks::mark_refresh}};
    // all or nothing: a pane moved without the hook that puts it back would stay moved
    for (const auto& [addr, fn] : hooks) {
        std::string other = replaced_by(addr);
        if (!other.empty()) {
            LOG("[chart] %s replaces %08X: mod chart markers are not shown this session", other.c_str(), addr);
            return;
        }
    }
    for (const auto& [addr, fn] : hooks) replace(addr, fn);
    g_installed = true;
    LOG("[chart] the sea chart shows markers of mod charts");
}

// (the circles' animations can't be run here: a circle stays shown at the game's place in the state,
// and is hidden again after the state is loaded, see heal_grid)
void capture_begin() {
    if (!g_installed) return;
    for (const auto& [menu, applied] : g_applied) revert(menu, applied);
    for (const auto& [grid, circles] : g_circles)
        for (const Circle& c : circles)
            if (grid_pane(grid, c.cell) == c.pane) st32(c.pane + kPaneTranslate, c.tx), st32(c.pane + kPaneTranslate + 4, c.ty);
}
void capture_end() {
    if (!g_installed) return;
    for (const auto& [menu, applied] : g_applied)
        for (const Applied& a : applied)
            if (ld32(menu + kMenuPanes + 4 * a.index) == a.pane) show(a);
    for (const auto& [grid, circles] : g_circles)
        for (const Circle& c : circles)
            if (grid_pane(grid, c.cell) == c.pane) st32(c.pane + kPaneTranslate, c.mx), st32(c.pane + kPaneTranslate + 4, c.my);
}
void state_loaded() {
    g_applied.clear();
    g_borrowed.clear();
    g_circles.clear();
    g_healed.clear();
    g_heal_grids = g_heal_balloons = true;
}

uint32_t g_chart_menu = 0;  // the sea chart menu (hdSeaChartMenu_c) since it last ran, until destroyed
uint32_t sea_chart_menu() { return g_chart_menu; }

namespace hooks {

// hdBtnMapIcon_c::update(): the game places its circle (salvaged treasure) first; while the game doesn't show
// it and the tracked step's spot is in this balloon's square, the circle goes to the spot instead, placed
// with the same mapping the game uses for its own circle
void icon_update(Cpu* c) {
    const uint32_t icon = c->r[3];
    Cpu keep = *c;
    g_original(c, kIconUpdate);
    const uint32_t owner = ld32(icon + kIconOwner), pane = ld32(icon + kIconTreasure), layout = ld32(icon + kPartsLayout);
    const uint32_t info = owner ? ld32(owner + kOwnerInfo) : 0;
    const uint32_t anims = layout ? ld32(layout + kLayoutAnimSet) : 0;
    auto give_back = [&] {
        auto it = g_borrowed.find(icon);
        if (it == g_borrowed.end()) return;
        if (!ld8(icon + kIconFishVisible)) {
            Cpu call = keep;
            g_call(&call, kAnimReset, {it->second, kFishAnim, kFishSlot});
        }
        g_borrowed.erase(it);
        if (trace()) LOG("[chart] balloon %08X: the circle is the game's again", icon);
    };
    // a world position in this balloon: relative to the island's point in its square, then the game's
    // own scales (positionFishInDetail / positionFishOnMap). The square of a position, as the game counts
    // squares: position / 100000, rounded (the sea's centre square is 0,0)
    const uint32_t parent = ld32(icon + kIconParent);
    const uint32_t mode = ld32(icon + kIconMapMode);
    auto place = [&](float x, float z, float& px, float& py) {
        const int cell_x = int(std::lround(x / kSquare)), cell_y = int(std::lround(z / kSquare));
        if (!info || cell_x != int8_t(ld8(info + 4)) || cell_y != int8_t(ld8(info + 5))) return false;
        const float rx = x - float(cell_x) * kSquare - float(int16_t(ld16(info + 6)));
        const float ry = z - float(cell_y) * kSquare - float(int16_t(ld16(info + 8)));
        const float psx = parent ? u32_as_f32(ld32(parent + kPaneScale)) : 1.0f, psy = parent ? u32_as_f32(ld32(parent + kPaneScale + 4)) : 1.0f;
        px = rx * u32_as_f32(ld32(icon + kIconPointScale)) * psx;
        py = -(ry * u32_as_f32(ld32(icon + kIconPointScale + 4))) * psy;
        if (mode == 0 || mode == 4) {  // on the map: plus the balloon's own offset
            px += u32_as_f32(ld32(icon + kIconPosition)) * u32_as_f32(ld32(icon + kIconTranslateScale));
            py -= u32_as_f32(ld32(icon + kIconPosition + 4)) * u32_as_f32(ld32(icon + kIconTranslateScale + 4));
        }
        return true;
    };
    if (trace() && info) {  // the mapping checked against the game's own placement of Link's icon
        static std::map<uint32_t, std::string> last;
        const uint32_t link = ld32(kLinkPtr), primary = ld32(icon + kIconPrimary);
        float lx = 0, ly = 0;
        char line[200] = "";
        if (link && primary && (ld8(primary + kPaneFlags) & kVisible) &&
            place(u32_as_f32(ld32(link + 0x314)), u32_as_f32(ld32(link + 0x31C)), lx, ly))
            snprintf(line, sizeof line, "balloon %08X cell %d,%d mode %u: Link's icon at %.1f,%.1f, the mapping says %.1f,%.1f", icon,
                     int8_t(ld8(info + 4)), int8_t(ld8(info + 5)), mode, u32_as_f32(ld32(primary + kPaneTranslate)),
                     u32_as_f32(ld32(primary + kPaneTranslate + 4)), lx, ly);
        if (*line && last[icon] != line) LOG("[chart] %s", line), last[icon] = line;
    }
    // after a state load: a circle the state was captured with (the game would show it hidden)
    if (g_heal_balloons && anims && std::find(g_healed.begin(), g_healed.end(), icon) == g_healed.end()) {
        g_healed.push_back(icon);
        if (!ld8(icon + kIconFishVisible)) {
            Cpu call = keep;
            g_call(&call, kAnimReset, {anims, kFishAnim, kFishSlot});
        }
    }
    float x = 0, z = 0, px = 0, py = 0;
    std::string label;
    if (!pane || !info || !anims || ld8(icon + kIconFishVisible)) return give_back();
    // the tracked step's spot in this square first, else a finished chart's marker in it
    bool found = g_tracked && g_tracked(x, z, label) && place(x, z, px, py);
    const bool tracked = found;
    if (!found)
        for (const Spot& m : done_spots())
            if (place(m.x, m.z, px, py)) {
                x = m.x, z = m.z, found = true;
                break;
            }
    if (!found) return give_back();
    const int cell_x = int(std::lround(x / kSquare)), cell_y = int(std::lround(z / kSquare));
    st32(pane + kPaneTranslate, f32_as_u32(px));
    st32(pane + kPaneTranslate + 4, f32_as_u32(py));
    st32(pane + kPaneTranslate + 8, 0);
    st8(pane + kPaneFlags, uint8_t(ld8(pane + kPaneFlags) | kMatrixDirty));
    if (trace()) {  // the circle following a moving resident
        static float last_x = 1e9f, last_y = 1e9f;
        if (std::fabs(px - last_x) > 0.5f || std::fabs(py - last_y) > 0.5f)
            LOG("[chart] balloon %08X: the %s circle at %.1f,%.1f (world %.0f,%.0f)", icon, tracked ? "tracked quest's" : "finished chart's", px, py, x,
                z),
                last_x = px, last_y = py;
    }
    if (!g_borrowed.count(icon)) {
        Cpu call = keep;
        g_call(&call, kAnimStop, {anims, kFishAnim, kFishSlot});
        g_borrowed[icon] = anims;
        LOG("[chart] the %s circle is in the zoomed square %d,%d at %.1f,%.1f (mode %u)", tracked ? "tracked quest's" : "finished chart's", cell_x,
            cell_y, px, py, mode);
    }
}

// updateProgressIcons(): the game's story marks, then the mod markers on the panes left free
void progress_icons(Cpu* c) {
    const uint32_t menu = c->r[3];
    g_chart_menu = menu;
    auto it = g_applied.find(menu);
    if (it != g_applied.end()) {
        revert(menu, it->second);
        it->second.clear();
    }
    g_hidden = false;
    g_original(c, kProgress);
    if (!g_hidden) apply(menu);  // busy: the game hid every story mark, the markers stay hidden too
}

// hideMenuPanes(): the screen is busy
void hide_panes(Cpu* c) {
    g_hidden = true;
    g_original(c, kHidePanes);
}

// destroy(flags): forget the menu's panes (and its balloons and grids)
void destroy(Cpu* c) {
    if (c->r[3] == g_chart_menu) g_chart_menu = 0;
    g_applied.erase(c->r[3]);
    g_borrowed.clear();
    g_circles.clear();
    g_original(c, kDestroy);
}

// updateChartMarkLayout(): the game puts every circle in its place and shows or hides it (complete or
// not); the finished charts' circles are placed again by the next refreshChartMarks
void mark_layout(Cpu* c) {
    const uint32_t grid = c->r[3];
    g_circles.erase(grid);
    g_original(c, kMarkLayout);
    if (trace()) {  // the mapping checked against the game's own placement of a square's circle
        const uint32_t touch = ld32(grid + kGridTouchPane);
        for (unsigned i = 0; touch && i < kCells; i++) {
            const uint32_t cell = ld32(grid + kGridCells + 4 * i), pane = grid_pane(grid, i);
            if (!cell || !pane || (!int8_t(ld8(cell + kCellCoord)) && !int8_t(ld8(cell + kCellCoord + 1)))) continue;
            const float x = float(int8_t(ld8(cell + kCellCoord))) * kSquare + float(int16_t(ld16(cell + kCellResource)));
            const float z = float(int8_t(ld8(cell + kCellCoord + 1))) * kSquare + float(int16_t(ld16(cell + kCellResource + 2)));
            LOG("[chart] grid %08X square %d,%d: the game's circle at %.1f,%.1f, the mapping says %.1f,%.1f (entry %u slot %u)", grid,
                int8_t(ld8(cell + kCellCoord)), int8_t(ld8(cell + kCellCoord + 1)), u32_as_f32(ld32(pane + kPaneTranslate)),
                u32_as_f32(ld32(pane + kPaneTranslate + 4)),
                u32_as_f32(ld32(touch + kPaneTranslate)) + x / kSquare * u32_as_f32(ld32(touch + kPaneSize)) / kSquaresPerSide,
                u32_as_f32(ld32(touch + kPaneTranslate + 4)) - z / kSquare * u32_as_f32(ld32(touch + kPaneSize + 4)) / kSquaresPerSide,
                ld32(cell + kCellMarkEntry), ld32(cell + kCellMarkSlot));
            break;
        }
    }
}

// refreshChartMarks() (every update): the game's circles, then the finished charts' circles
void mark_refresh(Cpu* c) {
    const uint32_t grid = c->r[3];
    Cpu keep = *c;
    g_original(c, kMarkRefresh);
    if (g_heal_grids && std::find(g_healed.begin(), g_healed.end(), grid) == g_healed.end()) {
        g_healed.push_back(grid);  // after a state load: hide the circles the game would not show
        for (unsigned i = 0; i < kCells; i++)
            if (const uint32_t cell = ld32(grid + kGridCells + 4 * i); cell && !ld8(cell + kCellMarkActive)) cell_circle(keep, cell, false);
    }
    place_circles(keep, grid);
}

}  // namespace hooks
}  // namespace story_chart
