# Text box swipe handover

## Proposed behaviour

Add a **Use text box for swipe** checkbox under **Caption transition → Match slideshow style**. Default it to off. When the selected Image Slideshow uses Swipe, enabling the checkbox makes the caption use the horizontal edges of its OBS scene item's bounding box as the swipe limits. This includes the empty space in a **Maximum size only** box. With the caption box and slideshow aligned on screen, the two transitions use the same spatial limits.

Keep the current transition duration, easing, direction, and hide/reveal sequence. **Match slideshow duration** and **Duration (ms)** continue to control timing. The option applies only to the filter's Swipe effect.

## Implementation notes

- The Swipe mask in [caption-fade.effect](data/caption-fade.effect) currently uses the text texture's normalised horizontal coordinate. Map the bounding box's horizontal edges into that coordinate system and move the mask across that interval. The edges can fall outside the texture, which is expected when the box is wider than the caption.
- Use the scene item's bounding box, content transform, and bounds alignment to determine the actual offset between the box and the rendered caption. Re-evaluate the mapping after the caption swaps because the incoming text can have a different width.
- A single source filter can be rendered through multiple scene items with different bounds. Use per-item geometry where it can be resolved reliably. Fall back to the existing texture-based mask for missing or ambiguous bounds. Keep the existing mask when the option is off.

## Manual test after implementation

1. Open a test scene in OBS Studio 33 or newer. Add an **Image Slideshow** with two same-size images and set its transition to **Swipe**. Use filenames that produce one short caption and one much longer caption. Keep both captions narrower than the box.
2. Add a Text source with the **Slideshow Caption** filter and select the slideshow. Set **Caption source** to **Filename**, choose **Whole filename**, and set **Caption transition** to **Match slideshow style**. For easier inspection, turn off **Match slideshow duration** and set a long custom duration.
3. Right-click the Text source and open **Transform → Edit Transform**. Set **Bounding Box Type** to **Maximum size only**. Set the box width and position so its left and right edges line up with the slideshow in the scene. Use centred alignment inside the box to leave visible empty space on both sides of the short caption.
4. Leave **Use text box for swipe** off and advance the slideshow in both directions to record the baseline. Enable the option and repeat. With the option on, the moving mask should cross the empty box space before reaching the letters or after leaving them, as appropriate for each phase. The caption should stay in place. Use a screen recording and step through frames if the difference is hard to see.
5. Keep the same caption and alignment and change only the box width. Repeat with left and right bounds alignment. The swipe should follow the configured box edges in each case. Check both the outgoing and incoming caption.
6. Compare runs with the option on and off using the same custom duration, then repeat with **Match slideshow duration** enabled. The overall duration and caption swap point should be the same for each pair. The times when individual letters appear or disappear can shift as the mask travels through empty space.
7. Switch the slideshow to Fade, then switch the caption to **Custom fade** and **OBS show/hide**. Check that those effects behave as before. Reopen OBS to verify the checkbox setting persists. Repeat the Swipe check with text sources without bounds and, if available, with both FreeType2 and GDI+.

This branch contains the plan and test steps. Feature implementation and OBS verification are pending.
