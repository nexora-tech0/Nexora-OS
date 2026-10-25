/*
 * Nexora OS - user/shell/desktop.h
 * The desktop: the window under everything else, painted transparently over the wallpaper. It
 * shows the shortcut icons (every pinned application, then Trash) followed by whatever is in the
 * user's Desktop folder, opens them on a double click, and offers the background menu (View,
 * Sort by, Refresh, New, Paste, Open Terminal, Personalize) and the item menu (Open, Open in
 * Files, Rename, Delete to Trash, Properties).
 *
 * The window carries HZ_WS_DESKTOP, so it stays below every other window but still takes the
 * keyboard when it is clicked: F2, Delete, F5, Ctrl+C/X/V, Enter and the arrows act on the
 * selection while no application window is active.
 */
#ifndef SHELL_DESKTOP_H
#define SHELL_DESKTOP_H

#include "shell.h"

namespace shell {

class DesktopView;

class Desktop final : public Window {
public:
    Desktop(int32_t Width, int32_t Height);

    void Reload(); /* re-reads the desktop folder (after a delete, a paste, or a file change) */
    void RefreshPrefs(); /* re-reads desktop.ini: Settings changed the icon size or the sort */
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;

protected:
    void OnContextMenu(Point Screen) override;

private:
    friend class DesktopView;

    void RebuildView(); /* the shortcuts followed by the folder's items, sorted */
    void Open(size_t Index); /* double click, Enter, or the menus' Open */
    void OpenInFiles(const hz::String &Path);
    void ShowItemMenu(size_t Index, Point Screen);
    void ShowBackgroundMenu(Point Screen);
    void ShowViewMenu(Point Screen);
    void ShowSortMenu(Point Screen);
    void ShowNewMenu(Point Screen);
    void DeleteItem(size_t Index);
    void RenameItem(size_t Index);
    void ShowProperties(size_t Index);
    void CopyItem(size_t Index, bool Cut);
    void Paste();
    void NewItem(bool Directory);
    void EmptyTrash();
    void Select(const hz::String &Name); /* selects by file name (after New or Rename) */
    bool OnKey(KeyEvent &E);
    bool Writable() const;
    void LoadPrefs();
    void SavePrefs() const;

    DesktopView *View_ = nullptr;
    hz::String Folder_;      /* the folder the desktop shows */
    hz::String ClipPath_;    /* the file clipboard (a path), belongs to this window */
    bool ClipCut_ = false;
};

/* Creates the desktop window (below the taskbar and the dock) or returns null. */
Desktop *CreateDesktop();

}  // namespace shell

#endif /* SHELL_DESKTOP_H */
