#pragma once

// The Synth1 panel: the Linux counterpart of
// Windows_Build/Source/WindowsEditor.cpp's canvas (itself a transcription of
// Shared/SynthPanelView.swift). Same design canvas (770x560), same control
// geometry, same theme slots — only the drawing API differs (GDI+ -> LICE),
// so a change to one port's layout belongs in the other.
//
// Coordinates are always design pixels; the Canvas applies the
// design -> device scale. Text goes through LinuxText (system FreeType), so
// it is hinted like every other Linux UI.
//
// Input is handled here too, in design coordinates, and every visual change
// lands in a dirty rectangle (takeDirty) the view turns into X damage.

#include "EditorCore/OxoSynth2Bridge.h"
#include "EditorCore/Themes.hpp"

#include "LinuxCanvas.hpp" // LICE + the embedded text (see there)

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxo {

// A design-space rectangle.
struct PanelRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    void unite(const PanelRect &o);
    void inflate(float d) { x -= d; y -= d; w += 2 * d; h += 2 * d; }
};

class LinuxPanel {
public:
    enum class Kind { Knob, Toggle, Cycle, Display, Wave, Radio, Badge, Label };
    struct Control {
        Kind kind = Kind::Label;
        int id = -1, value = 0;
        float x = 0, y = 0, w = 0, h = 0;
        std::string label;
        int symbol = 0;
    };

    static constexpr float kDesignWidth = 770.f, kDesignHeight = 560.f;

    explicit LinuxPanel(const OxoSynth2Hooks *hooks);
    ~LinuxPanel();

    LinuxPanel(const LinuxPanel &) = delete;
    LinuxPanel &operator=(const LinuxPanel &) = delete;

    // ---- theme (session) ----
    void adoptTheme();                       // hooks_.themeText, else the last picked
    void setTheme(const themes::Palette &p); // this instance, and the next one's default
    const themes::Palette &theme() const { return theme_; }

    // ---- drawing, design coordinates ----
    void drawStatic(Canvas &cr) const; // panel, sections, controls, LCD frame, chrome
    void drawDynamic(Canvas &cr);      // values, LEDs, waves, LCD text, voice grid

    // ---- parameter access ----
    int value(int id) const;
    std::string formatted(int id) const;
    std::string patchName() const;

    // ---- input (design coordinates) ----
    void mouseDown(float x, float y, bool doubleClick);
    void mouseMove(float x, float y, bool fine);
    void mouseUp(float x, float y);
    bool wheel(int direction, float x, float y);
    void rightClick(float x, float y);
    void setHover(float x, float y);
    PanelRect takeDirty() {
        PanelRect r = dirty_;
        dirty_ = {};
        return r;
    }

    // ---- keyboard (browser keys, SynthModel.browserKey) ----
    enum class Key { Escape, Return, Backspace, Delete, Home, End, Up, Down, Left, Right };
    bool keyDown(Key key);               // true = handled
    void textInput(const std::string &utf8); // the write dialog's name field

    // ---- theme editor (ThemeDialog): text colour values only, no picker ----
    bool themeDialogOpen() const { return themeDialog_; }
    void openThemeEditor();
    void closeThemeEditor();

    // ---- in-panel file dialog ----
    // There is no toolkit file chooser here (and no portal dependency): the
    // picker is drawn in the panel like the browser, listing the current
    // folder's sub-folders and the files the mode wants.
    enum class FileMode { None, OpenPatch, SavePatch, Import, ExportBank };
    // What the one in-panel text field edits.
    enum class EditTarget { None, PatchName, FileName, ThemeColor, ThemeName };
    bool fileDialogOpen() const { return file_.mode != FileMode::None; }

    // ---- in-panel menus ----
    // The mac draws its LogoMenu / EffectTypeMenu inside the panel and Windows
    // uses native popups; with no toolkit here the menu is drawn in-panel.
    bool menuOpen() const { return menu_.open; }
    // The view's current width (the logo menu ticks the matching UI size).
    void setViewWidth(int width) { viewWidth_ = width; }
    // A UI size picked in the logo menu: the view asks the host to resize.
    int takeRequestedWidth() {
        const int width = requestedWidth_;
        requestedWidth_ = 0;
        return width;
    }

    // ---- patch browser / write dialog (PatchBrowserView) ----
    bool browserOpen() const { return browser_; }
    bool writing() const { return writing_; }
    void openBrowser(bool writing = false);
    void closeBrowser();
    void startLibraryScan(); // background rescan on attach / open
    void stepPatch(int direction); // < >: next / previous non-empty slot

    // ---- 30 Hz host polling: values, patch, library, flash expiry ----
    bool poll(uint64_t nowMs);

private:
    void constructLayout();
    void knob(int id, const char *label, float sx, float sy, float x, float y, float size = 24);
    void button(int id, const char *label, Kind kind, float sx, float sy, float x, float y, float width = 40);
    void display(int id, float sx, float sy, float x, float y, float width = 30);
    void radio(int id, float sx, float sy, float x, float y, std::vector<std::string> choices,
               int first = 0, bool horizontal = false, bool trailing = false);
    void waves(int id, float sx, float sy, float x, float y, const std::vector<int> &values,
               const std::vector<int> &shapes);
    void label(float x, float y, const char *text);
    void badge(float x, float y, const char *text, int id = -1);

    void drawControlStatic(Canvas &cr, const Control &c) const;
    void drawControlDynamic(Canvas &cr, const Control &c, bool hovered);
    // ToggleButton (Shared/SynthControls.swift): a 5 px LED, a 3 px gap and
    // the label, the whole group centred in the button. Yields the LED's
    // centre X, the label's left edge and its width.
    void toggleLayout(Canvas &cr, const Control &c, float &ledX, float &textX, float &textWidth) const;
    void drawWave(Canvas &cr, const Control &c);
    void drawChrome(Canvas &cr, int id, bool hovered) const;
    void drawVoiceGrid(Canvas &cr);
    void drawLcdText(Canvas &cr);

    void touch(const PanelRect &r) { dirty_.unite(r); }
    void invalidateParam(int id);
    void setValueOnce(int id, int v);
    void cycle(int id, int step);
    void show(int id); // LCD "name: value" for 1.5 s
    int hit(float x, float y) const; // control index under a point, -1 = none
    int chromeAt(float x, float y) const; // chrome button id under a point, 0 = none
    static PanelRect bounds(const Control &c);
    static PanelRect chromeRect(int id);
    static PanelRect lcdRect() { return {50, 484, 432, 70}; }
    static PanelRect browserRect() { return {6, 34, 758, 444}; }
    // The write dialog's proposed "User" bank the library lacks (a last row;
    // the write creates the folder). Never passed to a hook.
    static constexpr int kNewUserBank = -3;

    // ---- the library (PatchManager::Json, the same shape the other ports read) ----
    struct Bank {
        std::string name, source;
        bool readOnly = false;
        std::vector<std::string> slots; // "" = empty slot
    };
    void refreshBanks();
    void adoptLibrary();
    int userBank() const;
    bool offersNewUserBank() const;
    int bankRowCount() const;
    int bankRowOf(int bank) const;
    void scrollBankIntoView(int bank);
    int bankBySource(const std::string &source) const;
    std::string slotText(int bank, int slot) const; // "NNN:name"
    int displayBankNumber(int bank) const;
    int bankRowAt(float x, float y) const;
    int slotAt(float x, float y) const;
    void browserClick(float x, float y, bool doubleClick);
    void slotClick(int index, bool doubleClick);
    void loadSlot(int bank, int slot);
    void drawBrowser(Canvas &cr);
    void drawNameField(Canvas &cr);
    // Menus
    enum class MenuAction {
        None, InitializePatch, LoadPatchFile, SavePatchFile, ProgramChange, AllNotesOff, Theme, SetParam, MidiLearn,
        UISize, ExportBank, ShowInLibrary, DeleteBank, DeletePatch, ThemeEditor
    };
    struct MenuItem {
        std::string label;
        MenuAction action = MenuAction::None;
        int param = 0; // SetParam: the parameter
        int value = 0; // theme index / the parameter's new value
        bool checked = false;
        bool separator = false;
    };
    struct MenuState {
        bool open = false;
        std::vector<MenuItem> items;
        float x = 0, y = 0, w = 0, h = 0; // design coords
        int columns = 1;
        int hover = -1;
    };
    void openMainMenu();
    void openLogoMenu(); // click on the title: UI size + themes
    void openFileDialog(FileMode mode);
    void refreshFiles();
    void drawFileDialog(Canvas &cr);
    void drawThemeEditor(Canvas &cr);
    bool themeClick(float x, float y, bool doubleClick);
    void focusField(EditTarget target, const std::string &text, bool selectAll);
    void applyThemeColor(); // the colour field -> the palette (live)
    void saveTheme();
    void deleteTheme();
    static PanelRect themeColorRect() { return {70, 366, 110, 20}; }
    static PanelRect themeNameRect() { return {240, 366, 210, 20}; }
    PanelRect fieldRect() const;
    bool fileClick(float x, float y, bool doubleClick); // true = taken
    bool fileWheel(int direction);
    void acceptFile();
    void closeFileDialog();
    void navigateTo(const std::filesystem::path &dir);
    void openThemeMenu();
    void openTypeMenu(int id, float x, float y); // effect / delay / chorus types
    void openSourceMenu(int id); // ctrl src (86, 88): none / bend / pressure / CC 0..127
    void openAssignMenu(int id); // ctrl assign (87, 89): Synth1's 51 targets
    void layoutMenu(int columns, float width);
    void drawMenu(Canvas &cr);
    bool menuClick(float x, float y); // true = the menu took the click
    bool menuMove(float x, float y);  // true = the hover changed
    void closeMenu();
    void runMenuAction(const MenuItem &item);
    // MIDI learn (SynthControls' Learnable, SynthModel's learn)
    void buildLearnRects();
    void setLearn(bool on);
    void learnSelect(int id);
    int learnHit(float x, float y) const; // hotspot under a point, -1 = none
    void drawLearn(Canvas &cr);
    void commitWrite();     // PatchBrowserView.commitWrite
    void showLibraryMenu(float x, float y, int bank, int slot); // Export / Show / Delete
    void askDelete(int bank, int slot); // the in-panel confirmation
    void drawConfirm(Canvas &cr);
    bool confirmClick(float x, float y); // true = taken
    void dragMove(float x, float y);
    void dragEnd(bool drop);
    PanelRect dragLabelRect(Canvas &cr) const;
    void newBank();         // "New bank...": a uniquely named folder, then select it
    void editKey(Key key);  // caret movement / deletion in the name field

    const OxoSynth2Hooks *hooks_ = nullptr;
    std::vector<Control> controls_;
    themes::Palette theme_ = themes::Synth2();
    std::array<uint32_t, themes::kSlotCount> colors_{};
    std::array<int, 100> knownValues_{};
    int knownSerial_ = -1;
    uint64_t knownVoiceStates_ = 0;
    std::string flashText_, bankText_;
    uint64_t flashUntil_ = 0;
    int hovered_ = -1;
    int hoveredChrome_ = 0;
    int dragId_ = -1;
    float dragStartY_ = 0;
    int dragStartValue_ = 0;
    PanelRect dirty_;
    float versionLeft_ = 593.f;

    MenuState menu_;
    int viewWidth_ = int(kDesignWidth);
    int requestedWidth_ = 0; // logo menu -> the view's host resize

    struct FileState {
        FileMode mode = FileMode::None;
        std::filesystem::path dir;
        std::vector<std::string> entries; // "name/" for a folder
        int scroll = 0, selected = -1;
        std::string title, accept;
    };
    FileState file_;
    bool showHidden_ = false; // the file dialog's "Hidden" toggle

    // Theme editor state.
    bool themeDialog_ = false;
    int themeRow_ = -1;       // the selected ThemeColor row
    std::string themeName_;   // the save / delete name
    EditTarget editTarget_ = EditTarget::None;
    int exportBank_ = -1; // FileMode::ExportBank: the bank to export

    // Browser drag and drop (PatchBrowserView's SlotDrop / BankDrop): a press
    // on a patch becomes a drag past the threshold, else a click on release.
    struct SlotPress {
        bool active = false, dragging = false;
        int bank = -1, slot = -1;
        std::string source; // the bank's: found again after a rescan
        float startX = 0, startY = 0, x = 0, y = 0;
        std::string label;  // "NNN:name", drawn under the pointer
    };
    SlotPress press_;
    int dropSlot_ = -1, dropBank_ = -1;

    // The in-panel confirmation (bank / patch delete).
    struct ConfirmState {
        bool open = false;
        std::string text;
        MenuAction action = MenuAction::None;
        int bank = -1, slot = -1;
    };
    ConfirmState confirm_;

    // MIDI learn: one hotspot per patch param, around all its controls.
    static constexpr int kLearnCount = 99; // patch params (SynthModel.paramCount)
    bool midiLearn_ = false;
    int learnTarget_ = -1;
    int learnPrevTarget_ = -1; // before a click: a double click restores it
    std::array<PanelRect, kLearnCount> learnRects_{};
    std::vector<int> learnOrder_;           // learnable ids, larger hotspots first
    std::array<int, kLearnCount> learnCC_{}; // learned CC per param (0 = none)
    int lastCcToken_ = 0;

    // Browser state (PatchBrowserView).
    bool browser_ = false;
    bool writing_ = false;
    std::string editText_;   // the write dialog's patch name
    size_t caret_ = 0;
    bool nameSelected_ = false; // the field opened with its text selected (mac)
    std::vector<Bank> banks_;
    std::vector<int> bankOrder_;
    std::string libraryRoot_;
    int knownRevision_ = 0;
    int selectedBank_ = -1, selectedSlot_ = -1;
    int bankScroll_ = 0, slotScroll_ = 0;

    // Text is drawn through the Canvas (LinuxText: system FreeType).
};

} // namespace oxo
