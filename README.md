# swan

The <u>sw</u>iss <u>a</u>rmy k<u>n</u>ife program for Windows. One program to replace many, featuring:

| Component | State |
| - | - |
| [File Explorer](#explorer) | MVP near completion |
| File Finder | Not started |
| yt-dlp Frontend | Not started |
| YouTube client | Not started |
| Terminal | Not started |

# Explorer

## Problems with Windows File Explorer

- Serious performance issues in certain cases
- Lack of multiple panes per window, annoying to manage multiple windows
- Lack of useful filtering
  - There is "Search", but it's always recursive and generally very slow or straight up broken at times
- No bulk renaming
- Inability to tell if a directory has things inside without looking inside (file? directories?)
- Inability to see/kill processes which are locking a file

## Feature List

### General/Navigation

- [x] Unicode support
- [x] Multiple explorer panes
- [x] Sort by any column - name, size, entry type, creation date, last modified date, etc.
- [x] Full support for unix separator - `/` instead of `\\`
- [x] Create empty files and directories
- [ ] File preview

<!-- TODO: GIF demo -->
- Basic navigation with double clicks
  - [x] Change to a directory by double clicking it
  - [x] Open a file or shortcut to file by double clicking it
  - [x] Navigate through directory shortcut by double clicking it
  - [x] Go to parent directory with up arrow or optional `..` directory

<!-- TODO: GIF demo -->
- Advanced selection with single clicks and modifiers
  - [x] `Left click` a directory to make it the current selection (clears any previous selection)
  - [x] `Ctrl + left click` a directory to add it to current selection (does not clear previous selection)
  - [x] `Shift + left click` a directory to add range to current selection (does not clear previous selection)
  - [x] `Escape` to clear current selection

- Various refresh modes
  - [x] `Manual` - button or Ctrl-r
  - [x] `Automatic` - auto refreshes on configured interval
  - [x] `Adaptive` - automatic for directories with <= N entries where N is user defined, falls back to manual for directories with > N entries
  - [x] Selection maintained between refreshes

<!-- TODO: GIF demo -->
- Context menu for directory entries (accessed with right click)
  - [x] Copy file/directory name - e.g. `file.cpp`
  - [x] Copy file/directory path - e.g. `C:\directory\file.cpp`
  - [x] Reveal in File Explorer - for when you need the odd functionality we don't yet have
  - [x] Rename
  - [ ] Create shortcut
  - [ ] Custom commands

<!-- TODO: GIF demo -->
- Track directory history
  - [x] Go back/forward in history with left/right arrows
  - [x] View and select from history with popup window

### Pins

<!-- TODO: GIF demo -->

- [x] Pin/unpin directories
- [ ] Reorder pins

### Filtering

<!-- TODO: GIF demo -->
- Fast, powerful filtering
  - Several match modes:
    - [x] `Contains` mode - searches for substring
    - [x] `RegExp` mode - full Regular Expression support
    - [ ] `Glob` mode - basic wildcards like * and ?
  - [x] Case sensitivity toggle
  - [ ] Match polarity (i.e. == vs. !=)
  - [ ] Discriminate by entry type - e.g. filter for files only, which match pattern

### Bulk File Operations

<!-- TODO: GIF demo -->
- [ ] Copy files
  - [ ] Undo
- [ ] Copy directories
  - [ ] Undo
- [ ] Cut (move) files
  - [ ] Undo
- [ ] Cut (move) directories
  - [ ] Undo
- [ ] Delete files
  - [ ] Undo
- [ ] Delete directories
  - [ ] Undo

- Bulk renaming
  - [x] Preview of current pattern's before/after transformation
  - [x] Arbitrary counter with configurable start and step values
  - [x] Refer to current name minus extension using `<name>` (e.g. if `file.cpp`, `<name>` = `file`)
  - [x] Refer to current extension using `<ext>` (e.g. if `file.cpp`, `<ext>` = `cpp`, blank for directories)
  - [x] Refer to size in bytes using `<bytes>` (= 0 for directories)
  - [ ] Freeform mode
  - [ ] Transactional
  - [ ] Undo

### Advanced Features

<!-- TODO: GIF demo -->
- Tools for dealing with locked files
  - [ ] See process blocking an operation
  - [ ] Bring blocking process into focus
  - [ ] Kill process blocking an operation

<!-- TODO: GIF demo -->
- Show directory preview
  - [ ] Number of direct child files
  - [ ] Number of direct child directories
  - [ ] Longest directory chain depth
  - [ ] Recursive size of all children

## Performance TODOs

- [ ] Implement fixed_string_allocator to reduce memory waste from swan_path_t

## Attributions

<a href="https://www.flaticon.com/free-icons/swan" title="swan icons">Swan icons created by Freepik - Flaticon</a>
<a href="https://www.flaticon.com/free-icons/lightweight" title="lightweight icons">Lightweight icons created by Icongeek26 - Flaticon</a>
<a href="https://www.flaticon.com/free-icons/origami" title="origami icons">Origami icons created by smalllikeart - Flaticon</a>
<a href="https://www.flaticon.com/free-icons/bird" title="bird icons">Bird icons created by berkahicon - Flaticon</a>
