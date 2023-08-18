# swan

The <u>sw</u>iss <u>a</u>rmy k<u>n</u>ife program for Windows. One program to replace many, featuring:

- [ ] Powerful [file explorer](#explorer) (wayyy better than Windows Explorer)
- [ ] Super fast file finder (Windows Explorer? pfff)
- [ ] yt-dlp frontend
- [ ] Fast, useful terminal (unlike Command Prompt)

## Explorer

<img src="resource/preview1.png" />

### Controls

entry = file | directory

- \[Left Click] to set selection to entry
- \[Ctrl] + \[Left Click] to toggle entry selection
- \[Shift] + \[Left Click] to add all entries inclusively between prev-click and current-click to selection
- double \[Left Click] to open entry
- \[Enter] to open selected entry
- \[Ctrl] + \[r] to refresh entries

### What annoys me about Windows Explorer

- Only a single pane
- Bad Shift-click behavior
- Search is wayyy too slow (it's useless)
- No batch renaming
- Lacks ability to see/kill processes which are locking a file
- Can't tell if a folder has stuff inside (files? folders? nested folders?)

### Todo

MVP:

- [x] Remove "." directory
- [x] Double click directory
- [x] Double click file
- [x] Multiple panes
- [x] Manual refresh
- [x] Regex filtering
- [x] Clickable directories in cwd
- [x] Back/forward history buttons
- [x] Ctrl-r to refresh
- [x] "Contains" filtering
- [x] Handle shortcuts (IShellLink)
- [x] History browser
- [x] Sort directory entries
- [x] Unix path separator (/) support
- [x] Persist state
- [x] Pins
- [x] Show when last refreshed
- [x] Column: "last modified"
- [x] Automatic refresh

- [x] Context menu: copy name
- [x] Context menu: copy path
- [x] Context menu: reveal in File Explorer
- [x] Context menu: rename
- [ ] Context menu: custom commands

- [x] Toggleable auto refresh
- [x] Adaptive auto refresh (on for small directories, off for large ones)
- [x] Unicode
- [x] Handle executable .lnk
- [x] Create directory
- [x] Create file
- [x] Basic drive list
- [ ] Bulk renaming

- [ ] Cut file
- [ ] Copy file
- [ ] Delete file
- [ ] Undo file delete

- [ ] Copy directory
- [ ] Move directory
- [ ] Delete directory
- [ ] Undo directory delete

- [ ] Persist file operations history

Not part of MVP:

- [ ] Undo file move
- [ ] Undo file copy
- [ ] Undo directory move
- [ ] Undo directory copy
- [ ] Context menu: create shortcut
- [ ] Filter entry types (directory, file, symlink)
- [ ] Glob filtering
- [ ] Show directory preview (num child files/dirs)
- [ ] File preview

Performance items:
- [ ] Cache pretty size, modified timestamp, created timestamp
