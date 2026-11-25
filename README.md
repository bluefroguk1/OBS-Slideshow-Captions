# OBS Slideshow Captions

Show a caption under each picture in an OBS **Image Slideshow**, automatically.

Slideshow Caption is a filter for OBS Studio 33 that you add to a **Text
source**. Whenever the slideshow moves to the next image, the filter works out
a caption (from the filename, a text file, or the picture's own metadata) and
puts it into the Text source, with an optional fade between captions.

Typical uses: artist credits for an art slideshow, photo titles, "now showing"
labels.

## Quick start

1. [Install the plugin](#install) and restart OBS.
2. Add an **Image Slideshow** source to your scene and pick your images.
3. Add a **Text** source (GDI+ on Windows, FreeType2 on any platform) with
   **Read from file** turned off. Position and style it however you like.
4. Right-click the Text source → **Filters** → **+** → **Slideshow Caption**.
5. Set **Image Slideshow** to your slideshow, and choose a **Caption source**
   (see below).
6. The caption appears when the slideshow next changes slide. If the slideshow
   was already running, wait for the next slide, or advance it manually.

The filter only changes the Text source's text. Its font, colour, position,
size and other filters stay exactly as you set them.

## How it works

OBS's Image Slideshow announces each new image to anything listening. The
filter listens, finds the file path of the image now on screen, works out a
caption using the **Caption source** you chose, and writes it into the Text
source's text. Because it only edits the text, you keep full control of the
look: font, colour, outline, position and bounds are set on the Text source as
usual.

Things that follow from this:

- The filter sits on the **Text source**, not on the slideshow.
- It starts working on the next slide change after it's added or its
  settings change, so advance the slideshow once to see the result.
- Captions are read in the background, so a slow or large file won't stall
  OBS.
- If no caption can be found for an image, the Text source is cleared.

## Settings reference

| Setting | What it does |
| --- | --- |
| **Image Slideshow** | The slideshow to follow. Only Image Slideshow sources are listed. |
| **Caption source** | Where captions come from (see [below](#choosing-where-captions-come-from)). |
| **Use** / **Separator** | Filename mode: which part of the name to show, and the text that splits it. |
| **Filename regex** | Regex mode: the pattern to match against the filename. |
| **Caption template** | Regex and metadata modes: how to lay out the result, e.g. `{authors} — {title}`. |
| **Shared text file** | Shared-file mode: the list of filenames and captions. |
| **Caption transition** | How the old caption changes to the new one (see [Transitions](#transitions)). |
| **Match slideshow duration** | Use the slideshow's own transition time instead of a custom duration. |
| **Duration (ms)** | Length of the caption transition, 0 to 20000 ms. |
| **Preview on the Text source** | Plays two sample captions so you can judge the transition. |

Settings only show when they apply to the chosen caption source and
transition.

## Install

1. Download the latest `slideshow-captions-…-windows-x64-setup.exe` from the
   [Releases](../../releases/latest) page.
2. Close OBS, then run the installer.
3. Start OBS Studio 33 or newer.

**Manual install:** download the `.zip` from the same page instead and copy
its `slideshow-caption` folder into `C:\ProgramData\obs-studio\plugins`. The
`.dll` and the `data` folder should sit side by side inside it. For portable
OBS, use the `plugins` folder inside your OBS folder.

## Choosing where captions come from

| Caption source | Best when |
| --- | --- |
| **Filename** | Your files are already named for the caption, e.g. `Bluey - Bandit.png` |
| **Filename regex** | Filenames follow a pattern you want to pick apart |
| **Text file beside each image** | Each image has its own caption file |
| **Shared text file** | You'd rather keep all captions in one list |
| **Embedded metadata** | Your images carry title/author info (EXIF, IPTC, XMP) |

If a caption can't be found, the Text source is cleared.

### Filename

Pick what part of the name to show with **Use**:
**Before separator**, **After separator** or **Whole filename**. The default
**Separator** is ` - ` (space, dash, space). For `Bluey - Bandit.png`:

| Use | Caption |
| --- | --- |
| Before separator | Bluey |
| After separator | Bandit |
| Whole filename | Bluey - Bandit |

The file extension is never included. If the separator is empty or not in the
name, the whole filename is used.

### Text file beside each image

Put a text file with the same name next to the image: `photo.txt` for
`photo.jpg`. Its contents become the caption, and multiple lines are kept.
Save it as UTF-8.

### Shared text file

Create one UTF-8 text file with one line per image: the image's filename, a
**Tab** character, then the caption. See the
[example file](examples/shared-captions.txt).

```text
sunrise.jpg	Sunrise over the bay
harbour.jpg	Fishing boats at the harbour
```

Choose this file in **Shared text file**. If your images are in sub-folders,
use the path relative to the text file, e.g. `summer/sunrise.jpg`. A matching
relative path wins over a bare filename.

### Embedded metadata

Choose **Embedded metadata**, then type what you want into **Caption
template**, using these tokens in braces:

| Token | What it shows |
| --- | --- |
| `{title}` | Title |
| `{subject}` | Subject |
| `{tags}` | Tags / keywords |
| `{comments}` | Comments |
| `{authors}` | Author / artist / creator |

Examples: `{authors}` or `{authors} — {title}`. Anything else you type,
spaces and punctuation included, appears as written. Missing fields are left
blank.

To see what an image contains, use **Properties → Details** in Windows
Explorer.

### Filename regex (advanced)

Regular expressions let you extract exactly the piece you want. Enter the
pattern in **Filename regex** and use **Caption template** to say how to show
the result. The pattern is matched against the filename without its extension.

| Goal | Filename regex | Caption template |
| --- | --- | --- |
| `Bluey - Bandit.png` → **Bluey** | `^.*?(?=\s*-)` | `{0}` |
| Names with hyphens, e.g. `Bluey-Smith - Bandit.png` → **Bluey-Smith** | `^.*?(?=\s+-\s+)` | `{0}` |
| `Ada - Sunrise.jpg` → **Ada — Sunrise** | `^(?<artist>.+?) - (?<title>.+)$` | `{artist} — {title}` |

`{0}` is the whole match; `{name}` inserts a named group. Type patterns exactly
as shown (a single backslash).

## Transitions

**Caption transition** controls how one caption changes into the next.

| Transition | What happens |
| --- | --- |
| **Instant** | The caption simply switches. |
| **Custom fade** | The old caption fades out, then the new one fades in. |
| **Match slideshow style** | Copies the slideshow's own Cut, Fade, Swipe or Slide look. |
| **OBS show/hide** | Uses the Show and Hide transitions you set on the Text item in OBS's Sources panel. |

Each has a **Duration (ms)**. Turn on **Match slideshow duration** to use the
slideshow's transition time instead.

**Match slideshow style** transitions both captions together. When duration
matching is also enabled, it follows the linked slideshow's live transition
progress when OBS exposes it.

With **Match slideshow style**, enable **Use text box for swipe** to move the
Swipe mask across the Text scene item's bounding box. The mask includes empty
space around a short caption. With the option off, the mask travels only across
the outgoing and incoming caption textures' combined width. The difference is
most visible when the text box is wider than both captions. The filter uses
the caption texture as the swipe area when the scene item has no bounds, is
cropped, or has multiple visible placements.

### Try it first

**Preview on the Text source** plays two sample captions so you can judge the
transition, then restores your caption. It changes the real Text source, so it
will show on stream if that scene is live. Preview in a test scene, or when the
scene isn't in Program.

## Troubleshooting

| Problem | Try this |
| --- | --- |
| Filter isn't in the list | Check the folder layout above, make sure OBS is version 33 or newer, then restart OBS. |
| Slideshow isn't in the dropdown | Only **Image Slideshow** sources are listed. Reopen the filter after adding it. |
| Caption stays empty | Wait for the next slide change. Then check the file naming (or metadata) matches the chosen **Caption source**. |
| Odd characters | Save text files as UTF-8. |
| Regex shows nothing | The pattern didn't match. A bad pattern is reported in the OBS log (**Help → Log Files**). |

## Licence

Built for OBS Studio 33 or newer. Licensed under the GNU GPL v2 (see [LICENSE](LICENSE)).
