# Muse gadget apps API 1.1 (draft)

<!-- Generated from api.json by gen_api.py. Don't edit: change api.json. -->

What a WebAssembly app on a Muse gadget can call, the events it gets and the constants it uses. The firmware embeds its machine-readable projection and serves it in parts with app.api; gen_api.py makes the C header apps include, the reference, and the firmware's import table from it.

## Conventions

- Every function is imported from the module "gadget". An app that imports anything else isn't run.
- Integers are 32-bit and signed. Colours are 0xRRGGBB. Opacity runs from 0 (transparent) to 255 (opaque). Angles are whole degrees, clockwise, with 0 at 3 o'clock, as in LVGL. Percentages run from 0 to 100.
- Strings are NUL-terminated UTF-8 in the app's memory. A call copies what it needs before it returns.
- A handle or id is positive; 0 is never a valid one. A function that can fail returns a negative ERR_ constant.
- Nothing blocks. A call that needs the VM, or takes time, returns an operation id at once and finishes later with EV_DONE (a: the id, b: 0 or more on success, or an ERR_). A buffer given to such a call must stay reserved until then.
- Changes to the screen made in one callback appear together on the next frame after it returns. Getters see every change made so far.
- The gadget describes itself: board facts come from sys_info, and its sensors and other hardware from dev_ calls, each with a description the firmware supplies. device.guide gives Muse the same before it writes an app.

## Limits

The same on every gadget that runs apps.

| Limit | Value | |
|---|---|---|
| `module_bytes` | 262,144 | The largest .wasm file. |
| `budget_bytes` | 2,097,152 | The most memory one app takes: its linear memory and stack, and what the firmware holds for it (objects, styles, canvases, images, chat text, device queues). Gadgets give each app this much, or less where PSRAM is short (never under 1 MB): device.guide capabilities.wasm.budget_kb and sys_info(INFO_MEM_BUDGET) say how much. |
| `instructions` | 4,000,000 | Instructions per callback, well under a second. Past that the app traps and stops. |
| `instructions_start` | 16,000,000 | Instructions for _initialize and on_start together. |
| `objects` | 256 | UI objects alive at once, the root included. |
| `styles` | 32 | Shared styles. |
| `timers` | 8 | Timers; ids 1 to 8. |
| `ops` | 4 | Operations in flight (VM reads and writes, chat sends, image loads). |
| `devices_open` | 4 | Devices streaming at once (dev_open). |
| `device_queue` | 64 | Readings kept per open device; the oldest are dropped past that. |
| `device_fields` | 8 | Values in one reading. |
| `action_params` | 8 | Numbers one action takes. |
| `chat_messages` | 8 | Chat messages kept for the app. |
| `chat_text_bytes` | 4,096 | Text kept per chat message; longer messages are cut and marked. |
| `text_bytes` | 4,096 | A text property or string argument. |
| `read_bytes` | 65,536 | One file_read or state_load. |
| `state_bytes` | 65,536 | All of one app's state files on the VM. |
| `name_chars` | 32 | A state name: a-z, 0-9, '-', '_' and '.'. |
| `log_line_bytes` | 200 | One sys_log line; longer ones are cut. |
| `image_pixels` | 0 | Decoded images and canvases together: no more pixels than the screen has (0 here means the screen's own count). |
| `chart_series` | 4 | Series per chart. |
| `line_points` | 64 | Points per line object. |
| `frame_rate` | 30 | on_frame calls per second, at most. |
| `object_pixels` | 4,096 | An object's position, width and height in pixels, either way; larger values are clamped. |
| `shadow_pixels` | 64 | Shadow width, spread and offsets, line dashes, and radii other than RADIUS_CIRCLE; larger values are clamped. |
| `styles_per_object` | 32 | The app's styles on one object: shared ones from ui_style_add plus each selector ui_style sets; past that ERR_LIMIT. |

## What an app exports

| Export | | |
|---|---|---|
| `on_start()` | required | Called once the app is loaded: build the screen here. |
| `on_event(type, a, b)` | optional | Called for each event (EV_ constants; the table under Events says what a and b hold). |
| `on_frame(now_ms)` | optional | Called up to sys_frame_rate times a second while the screen is on, with sys_millis(). Leave it out unless the app animates something itself: widgets, animations and timers don't need it. |
| `on_stop(reason)` | optional | Called before the app is closed, unless it trapped. Save state here: state_save calls made in on_stop finish (for up to 2 s) before the app's memory is freed. |

## Events

`on_event(type, a, b)`:

| Event | a | b | |
|---|---|---|---|
| `EV_UI` | handle | UI_ bit | Something happened to an object the app asked about with ui_on, or an image load finished. |
| `EV_BUTTON` | BUTTON_ | BTN_ | A button. |
| `EV_KNOB` | steps |  | The knob turned: positive clockwise. |
| `EV_TOUCH_DOWN` | x | y | A touch began on the root, or on an object that isn't clickable. |
| `EV_TOUCH_MOVE` | x | y | The touch moved. |
| `EV_TOUCH_UP` | x | y | The touch ended. |
| `EV_GESTURE` | GESTURE_ |  | A swipe. |
| `EV_TIMER` | timer id |  | A timer fired. |
| `EV_DONE` | op id | result | An operation finished: b is 0 or more on success (bytes read, say), or an ERR_. |
| `EV_CHAT` | CHAT_ | message id or value | The chat changed, after chat_open. |
| `EV_MUSE` | MUSE_ |  | Muse's mode changed. |
| `EV_DEV_DATA` | device | readings waiting | Readings to take with dev_take, after dev_open. At most one per frame per device. |
| `EV_DEV_EVENT` | device | event index | A device's event (dev_event gives the index), after dev_open. |
| `EV_SCREEN` | 1 on, 0 off |  | The screen went off (on_frame stops) or came back. |
| `EV_CONNECTION` | 1 up, 0 down |  | The link to the VM came up or went down. |

## Functions

### System

Time, logging, board facts, timers and the app's own lifecycle.

#### `sys_millis`

```c
int32_t sys_millis(void);
```

A clock for timing. Wraps after 24 days.
- Returns: Milliseconds since the gadget started.
- WAMR signature: `()i`

#### `sys_time`

```c
int64_t sys_time(void);
```

The time of day. 0 until the gadget has set its clock from the VM, soon after it connects.
- Returns: UTC milliseconds since 1970, or 0.
- WAMR signature: `()I`

#### `sys_log`

```c
void sys_log(const char *text);
```

Adds a line to the app's log, which Muse reads with app.logs.

- `text`: The line.
- WAMR signature: `($)`

#### `sys_info`

```c
int32_t sys_info(int32_t key);
```

Board facts and the app's memory use.

- `key`: What to report.
- Returns: The value, or ERR_ARG.
- WAMR signature: `(i)i`

#### `sys_random`

```c
int32_t sys_random(void);
```

From the hardware random number generator.
- Returns: 32 random bits.
- WAMR signature: `()i`

#### `sys_timer`

```c
int32_t sys_timer(int32_t id, int32_t ms, int32_t repeat);
```

Starts or restarts a timer: EV_TIMER after ms milliseconds, while the app runs, screen on or off.

- `id`: 1 to 8.
- `ms`: 10 or more.
- `repeat`: Fire every ms, not once.
- Errors: `ERR_ARG`
- WAMR signature: `(iii)i`

#### `sys_timer_stop`

```c
int32_t sys_timer_stop(int32_t id);
```

Stops a timer.
- Errors: `ERR_ARG`
- WAMR signature: `(i)i`

#### `sys_frame_rate`

```c
int32_t sys_frame_rate(int32_t fps);
```

How often on_frame is called while the screen is on. 30 to start with, if the app exports on_frame.

- `fps`: 0 to 30.
- Errors: `ERR_ARG`
- WAMR signature: `(i)i`

#### `sys_keep_awake`

```c
void sys_keep_awake(int32_t on);
```

Keeps the screen on while on is 1. Use it only while showing something the person is watching.
- WAMR signature: `(i)`

#### `sys_exit`

```c
void sys_exit(void);
```

Ends the app once the current callback returns, after on_stop(STOP_EXIT).
- WAMR signature: `()`

#### `sys_brightness`

```c
int32_t sys_brightness(int32_t percent);
```

Sets the screen's brightness while the app runs.

- `percent`: 1 to 100, or -1 for the person's own setting.
- Errors: `ERR_UNSUPPORTED`, `ERR_ARG`
- WAMR signature: `(i)i`

### UI objects

LVGL widgets, kept by the firmware. Create them once, change their properties, and ask for their events with ui_on. Properties are P_ constants (Properties, below); style properties set this way apply to the main part in the default state.

#### `ui_create`

```c
int32_t ui_create(int32_t type, int32_t parent);
```

Creates a widget with the theme's look.

- `type`: A W_ constant.
- `parent`: UI_ROOT, or another object other than a roller.
- Returns: The new object.
- Errors: `ERR_ARG`, `ERR_HANDLE`, `ERR_LIMIT`, `ERR_NO_MEMORY`
- WAMR signature: `(ii)i`

#### `ui_delete`

```c
int32_t ui_delete(int32_t obj);
```

Deletes an object and its children. Their handles become invalid.
- Errors: `ERR_HANDLE`
- WAMR signature: `(i)i`

#### `ui_clean`

```c
int32_t ui_clean(int32_t obj);
```

Deletes an object's children.
- Errors: `ERR_HANDLE`
- WAMR signature: `(i)i`

#### `ui_set`

```c
int32_t ui_set(int32_t obj, int32_t prop, int32_t value);
```

Sets a property that isn't text.

- `prop`: A P_ constant.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_UNSUPPORTED`
- WAMR signature: `(iii)i`

#### `ui_get`

```c
int32_t ui_get(int32_t obj, int32_t prop);
```

Reads a property that isn't text.
- Returns: The value, or UI_INVALID.
- WAMR signature: `(ii)i`

#### `ui_set_text`

```c
int32_t ui_set_text(int32_t obj, int32_t prop, const char *text);
```

Sets a text property.

- `prop`: A text property: P_TEXT, P_OPTIONS...
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_UNSUPPORTED`, `ERR_NO_MEMORY`
- WAMR signature: `(ii$)i`

#### `ui_get_text`

```c
int32_t ui_get_text(int32_t obj, int32_t prop, void *buf, int32_t buf_size);
```

Reads a text property, such as what was typed into a text area.

- `buf`: Gets the text, cut to fit and always NUL-terminated.
- Returns: The text's full length in bytes.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_UNSUPPORTED`
- WAMR signature: `(ii*~)i`

#### `ui_on`

```c
int32_t ui_on(int32_t obj, int32_t events);
```

Chooses which events of this object come to on_event as EV_UI. Replaces the earlier choice.

- `events`: UI_ bits, 0 for none.
- Errors: `ERR_HANDLE`
- WAMR signature: `(ii)i`

#### `ui_align`

```c
int32_t ui_align(int32_t obj, int32_t align, int32_t dx, int32_t dy);
```

Places an object in its parent, offset by dx, dy.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiii)i`

#### `ui_align_to`

```c
int32_t ui_align_to(int32_t obj, int32_t base, int32_t align, int32_t dx, int32_t dy);
```

Places an object relative to another, once (it doesn't follow base later).

- `base`: Another object.
- `align`: ALIGN_OUT_ puts it outside base.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiiii)i`

#### `ui_set_parent`

```c
int32_t ui_set_parent(int32_t obj, int32_t parent);
```

Moves an object to another parent.

- `parent`: Not a roller, nor the object or one inside it.
- Errors: `ERR_ARG`, `ERR_HANDLE`
- WAMR signature: `(ii)i`

#### `ui_grid`

```c
int32_t ui_grid(int32_t obj, const char *columns, const char *rows);
```

Lays out the object's children in a grid. Sizes are space-separated: pixels, "auto" (fits the content) or "Nfr" (N shares of what's left), up to 8 each: ui_grid(obj, "1fr 1fr 1fr", "auto 1fr"). Place children with ui_grid_cell.

- `columns`: Column sizes.
- `rows`: Row sizes.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(i$$)i`

#### `ui_grid_cell`

```c
int32_t ui_grid_cell(int32_t obj, int32_t col, int32_t col_span, int32_t row, int32_t row_span);
```

Puts a child in a grid cell; the child stretches to fill it.

- `obj`: A child of a grid.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iiiii)i`

#### `ui_scroll_to`

```c
int32_t ui_scroll_to(int32_t obj, int32_t x, int32_t y, int32_t animate);
```

Scrolls an object's content.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiii)i`

#### `ui_scroll_to_view`

```c
int32_t ui_scroll_to_view(int32_t obj, int32_t animate);
```

Scrolls its parents until the object is visible: the newest message in a chat, say.
- Errors: `ERR_HANDLE`
- WAMR signature: `(ii)i`

#### `ui_navigation`

```c
int32_t ui_navigation(int32_t on);
```

On boards without touch: buttons and the knob move focus between the app's buttons, sliders and other inputs (UP and DOWN, or the knob), ENTER presses or edits, BACK sends UI_CANCEL. Buttons it uses don't come as EV_BUTTON while it's on.
- WAMR signature: `(i)i`

#### `ui_text_width`

```c
int32_t ui_text_width(int32_t font, const char *text);
```

How wide a line of text is.
- Returns: Pixels.
- WAMR signature: `(i$)i`

#### `ui_text_height`

```c
int32_t ui_text_height(int32_t font, int32_t width, const char *text);
```

How tall text is, wrapped at width.

- `width`: Wrap width.
- Returns: Pixels.
- WAMR signature: `(ii$)i`

#### `ui_span`

```c
int32_t ui_span(int32_t y, int32_t margin);
```

How much of row y shows, less margin at each end: on a round screen the circle's chord, else the screen's width. Size text and rows near the top and bottom of a round screen by it.

- `y`: A row of the screen.
- `margin`: Pixels to keep clear at each end.
- Returns: Pixels, centred on the screen; 0 off the screen.
- WAMR signature: `(ii)i`

#### `ui_fit_text`

```c
int32_t ui_fit_text(int32_t obj, int32_t width, int32_t font);
```

Fits a label's text on one line width wide: in font, or the next smaller font it fits in, down to FONT_14, then cut short with "...". Sets the label's width, font and long mode. Call it after ui_set_text, and again when the text changes, for any text whose length you don't control: a name, a message, a number that can grow.

- `obj`: A label.
- `width`: The most it may take.
- `font`: The font it would like.
- Returns: The FONT_ it got.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_UNSUPPORTED`
- WAMR signature: `(iii)i`

#### `ui_image_load`

```c
int32_t ui_image_load(int32_t obj, const char *path);
```

Loads a PNG or JPEG into an image object, from the VM or from the gadget's own files (which work offline). EV_UI with UI_LOADED or UI_LOAD_FAILED says when it's done. The decoded image counts against the budget.

- `obj`: An image object.
- `path`: In the app's folder on the VM, or from "/" on the gadget (files.gadget).
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_LIMIT`, `ERR_OFFLINE`
- WAMR signature: `(i$)i`

#### `ui_image_set`

```c
int32_t ui_image_set(int32_t obj, const void *data, int32_t data_len);
```

Shows a PNG or JPEG from the app's memory, decoded at once.

- `obj`: An image object.
- `data`: A PNG or JPEG file.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_NO_MEMORY`
- WAMR signature: `(i*~)i`

#### `ui_line_points`

```c
int32_t ui_line_points(int32_t obj, const void *xy, int32_t xy_len);
```

Sets a line's points.

- `obj`: A line object.
- `xy`: int32_t x, y pairs.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_TOO_LARGE`
- WAMR signature: `(i*~)i`

#### `ui_chart_series`

```c
int32_t ui_chart_series(int32_t chart, int32_t color);
```

Adds a series to a chart.
- Returns: The series, 1 to 4.
- Errors: `ERR_HANDLE`, `ERR_LIMIT`
- WAMR signature: `(ii)i`

#### `ui_chart_set`

```c
int32_t ui_chart_set(int32_t chart, int32_t series, const void *values, int32_t values_len);
```

Sets a series' points.

- `values`: int32_t values, up to P_POINT_COUNT.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(ii*~)i`

#### `ui_chart_push`

```c
int32_t ui_chart_push(int32_t chart, int32_t series, int32_t value);
```

Adds a point to a series, as P_UPDATE_MODE says.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iii)i`

#### `ui_table_cell`

```c
int32_t ui_table_cell(int32_t table, int32_t row, int32_t col, const char *text);
```

Sets a cell's text, adding rows and columns as needed.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_NO_MEMORY`
- WAMR signature: `(iii$)i`

#### `ui_table_column_width`

```c
int32_t ui_table_column_width(int32_t table, int32_t col, int32_t width);
```


- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iii)i`

#### `ui_scale_needle`

```c
int32_t ui_scale_needle(int32_t scale, int32_t line, int32_t length, int32_t value);
```

Points a line at a value on a round scale.

- `line`: A line object, child of the scale.
- `length`: Pixels from the centre.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiii)i`

### Styles

Style properties for other parts and states, and shared styles.

#### `ui_style`

```c
int32_t ui_style(int32_t obj, int32_t selector, int32_t prop, int32_t value);
```

Sets a style property for one part and state: ui_style(slider, PART_INDICATOR, P_BG_COLOR, 0x00C8FF). Each selector counts once toward limits.styles_per_object.

- `selector`: A PART_ plus STATE_ bits.
- `prop`: A style property.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_LIMIT`
- WAMR signature: `(iiii)i`

#### `ui_style_create`

```c
int32_t ui_style_create(void);
```

Creates a shared style, to give many objects the same look with one set of calls.
- Returns: A style.
- Errors: `ERR_LIMIT`, `ERR_NO_MEMORY`
- WAMR signature: `()i`

#### `ui_style_set`

```c
int32_t ui_style_set(int32_t style, int32_t prop, int32_t value);
```

Sets a property in a shared style. Objects using it update.

- `prop`: A style property.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iii)i`

#### `ui_style_add`

```c
int32_t ui_style_add(int32_t obj, int32_t style, int32_t selector);
```

Gives an object a shared style. With the selectors ui_style has set on it, 32 per object (limits.styles_per_object). Properties set on the object itself still win.

- `selector`: A PART_ plus STATE_ bits.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_LIMIT`
- WAMR signature: `(iii)i`

### Animation

Animating a property to a value.

#### `ui_animate`

```c
int32_t ui_animate(int32_t obj, int32_t prop, int32_t to, int32_t ms, int32_t easing);
```

Animates a property from its value now to to. UI_ANIM_DONE follows, if asked for with ui_on. A new animation of the same property replaces the old one.

- `prop`: A property that isn't text.
- `easing`: An EASE_ plus ANIM_ flags.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_LIMIT`
- WAMR signature: `(iiiii)i`

#### `ui_animate_stop`

```c
int32_t ui_animate_stop(int32_t obj, int32_t prop);
```

Stops animations where they are.

- `prop`: 0 for all.
- Errors: `ERR_HANDLE`
- WAMR signature: `(ii)i`

### Canvas

Immediate-mode drawing on a canvas object (W_CANVAS). Drawing shows on the next frame.

#### `cv_fill`

```c
int32_t cv_fill(int32_t canvas, int32_t color);
```

Fills the whole canvas.
- Errors: `ERR_HANDLE`
- WAMR signature: `(ii)i`

#### `cv_opa`

```c
int32_t cv_opa(int32_t canvas, int32_t opa);
```

Opacity for the drawing calls that follow on this canvas. Starts at OPA_COVER.
- Errors: `ERR_HANDLE`
- WAMR signature: `(ii)i`

#### `cv_rect`

```c
int32_t cv_rect(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t color);
```

A filled rectangle; with radius RADIUS_CIRCLE, a circle or pill.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiiiiii)i`

#### `cv_rect_style`

```c
int32_t cv_rect_style(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t style);
```

Draws a rectangle with a shared style: P_RADIUS; P_BG_COLOR, P_BG_OPA, P_BG_GRAD_COLOR and P_BG_GRAD_DIR (GRAD_HOR or GRAD_VER, two colour stops); P_BORDER_COLOR, P_BORDER_WIDTH, P_BORDER_OPA and P_BORDER_SIDE; P_SHADOW_COLOR, P_SHADOW_WIDTH, P_SHADOW_OPA, P_SHADOW_OFFSET_X, P_SHADOW_OFFSET_Y and P_SHADOW_SPREAD. P_OPA and cv_opa multiply the fill, border and shadow opacities. Missing properties give a white fill, square corners and no border or shadow. Other properties are ignored. Border and shadow widths are clamped to 0..64 px, shadow spread to 0..64, and offsets to -4096..4096. Drawing, including shadows, is clipped to the canvas buffer: leave room around the rectangle. It paints once; later style changes affect only later calls, not already drawn pixels. No buffer yet means no drawing. Available since API 1.1.

- `w`: Positive pixels, no wider than the canvas.
- `h`: Positive pixels, no taller than the canvas.
- `style`: A shared style from ui_style_create, painted as it is now.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iiiiii)i`

#### `cv_border`

```c
int32_t cv_border(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, int32_t width, int32_t color);
```

A rectangle's outline, width pixels thick, inside it.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiiiiiii)i`

#### `cv_line`

```c
int32_t cv_line(int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t width, int32_t color);
```

A line with round ends.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiiiiii)i`

#### `cv_arc`

```c
int32_t cv_arc(int32_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t width, int32_t start, int32_t end, int32_t color);
```

An arc, clockwise from start to end.

- `r`: Outer radius.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiiiiiii)i`

#### `cv_triangle`

```c
int32_t cv_triangle(int32_t canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3, int32_t color);
```

A filled triangle.
- Errors: `ERR_HANDLE`
- WAMR signature: `(iiiiiiii)i`

#### `cv_text`

```c
int32_t cv_text(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t font, int32_t color, int32_t align, const char *text);
```

Text, wrapped at w.

- `y`: The top of the first line.
- `w`: Wrap width.
- Returns: The height drawn.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iiiiiii$)i`

#### `cv_pixels`

```c
int32_t cv_pixels(int32_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, const void *data, int32_t data_len);
```

Copies pixels in, clipped to the canvas.

- `data`: w * h RGB565 pixels, row by row, little-endian.
- Errors: `ERR_HANDLE`, `ERR_ARG`
- WAMR signature: `(iiiii*~)i`

### Muse

What Muse is doing, and the talk key.

#### `muse_mode`

```c
int32_t muse_mode(void);
```

What Muse is doing now; EV_MUSE says when it changes.
- WAMR signature: `()i`

#### `muse_caption`

```c
int32_t muse_caption(void *buf, int32_t buf_size);
```

The caption the face would show: what Muse heard, or an error.
- Returns: The caption's full length.
- WAMR signature: `(*~)i`

#### `muse_talk_key`

```c
void muse_talk_key(int32_t on);
```

With 1, the talk button is Muse's push-to-talk again, as outside apps, and its presses don't come as EV_BUTTON. Off when the app starts.
- WAMR signature: `(i)`

### Chat

Following the chat and sending to it, through the gadget's link to the VM. The gadget parses the VM's formats; the app sees messages.

#### `chat_open`

```c
int32_t chat_open(void);
```

Starts following the chat: fetches the latest message, then reports new ones as EV_CHAT. The gadget reconnects by itself. Needs `chat.read`.
- Errors: `ERR_DENIED`, `ERR_OFFLINE`, `ERR_NO_MEMORY`
- WAMR signature: `()i`

#### `chat_close`

```c
void chat_close(void);
```

Stops following the chat and frees its messages. Needs `chat.read`.
- WAMR signature: `()`

#### `chat_latest`

```c
int32_t chat_latest(int32_t n);
```

The app keeps the last chat_messages messages. Needs `chat.read`.

- `n`: 0 for the newest, 1 for the one before...
- Returns: A message id, or 0 if there's no such message.
- WAMR signature: `(i)i`

#### `chat_info`

```c
int32_t chat_info(int32_t id, int32_t key);
```

About a message. Needs `chat.read`.

- `id`: A message id.
- Errors: `ERR_NOT_FOUND`, `ERR_ARG`
- WAMR signature: `(ii)i`

#### `chat_text`

```c
int32_t chat_text(int32_t id, int32_t offset, void *buf, int32_t buf_size);
```

A message's text, without attachments or markup the gadget can't show. Needs `chat.read`.

- `offset`: Byte offset into the text.
- `buf`: Gets the text from offset, cut to fit and NUL-terminated.
- Returns: Bytes copied, not counting the NUL.
- Errors: `ERR_NOT_FOUND`, `ERR_ARG`
- WAMR signature: `(ii*~)i`

#### `chat_send`

```c
int32_t chat_send(const char *text);
```

Sends a message to Muse, as "[<app> on the gadget] text". EV_DONE when the VM has it; the reply comes through chat_open. Needs `chat.send`.
- Errors: `ERR_DENIED`, `ERR_OFFLINE`, `ERR_LIMIT`
- WAMR signature: `($)i`

### Files

Reading files on the VM (the app's own folder, or the home folder with the files.home permission) and the gadget's own files on its flash, which Muse puts there with files.put (paths from "/", with the files.gadget permission).

#### `file_read`

```c
int32_t file_read(const char *path, int32_t offset, void *buf, int32_t buf_size);
```

Reads part of a file on the VM, or on the gadget itself (a path from "/"). EV_DONE's b is the bytes read, 0 at the end of the file, or ERR_NOT_FOUND.

- `path`: Relative to the app's folder, from "~/" for the home folder (files.home), or from "/" for the gadget's own files (files.gadget).
- `buf`: Gets the bytes; keep it reserved until EV_DONE.
- Errors: `ERR_ARG`, `ERR_DENIED`, `ERR_OFFLINE`, `ERR_LIMIT`
- WAMR signature: `($i*~)i`

### State

Small files the app keeps between runs, on the VM under the gadget's folder (devices/<gadget>/state/<app>/).

#### `state_save`

```c
int32_t state_save(const char *name, const void *data, int32_t data_len);
```

Saves a state file on the VM, replacing the last one whole. EV_DONE when it's written.

- `name`: a-z, 0-9, '-', '_' and '.'.
- `data`: Copied during the call.
- Errors: `ERR_ARG`, `ERR_OFFLINE`, `ERR_LIMIT`, `ERR_TOO_LARGE`
- WAMR signature: `($*~)i`

#### `state_load`

```c
int32_t state_load(const char *name, void *buf, int32_t buf_size);
```

Loads a state file. EV_DONE's b is its size, or ERR_NOT_FOUND.

- `buf`: Keep it reserved until EV_DONE.
- Errors: `ERR_ARG`, `ERR_OFFLINE`, `ERR_LIMIT`
- WAMR signature: `($*~)i`

### Devices

Sensors, motors, relays and anything else the firmware has a driver for. The firmware describes each device: a name, its readings (fields with units), its actions (with their parameters) and its events. Apps find devices by name and read, stream or call them through these few functions, so new hardware needs no new imports and no change to this file. Well-known names (Devices, below) have fixed fields and units so apps work across boards; any other name carries its own description.

#### `dev_count`

```c
int32_t dev_count(void);
```

With dev_name, lists them all.
- Returns: How many devices this gadget has.
- WAMR signature: `()i`

#### `dev_name`

```c
int32_t dev_name(int32_t index, void *buf, int32_t buf_size);
```

A device's name, by position.

- `index`: 0 to dev_count() - 1.
- `buf`: Gets the name, NUL-terminated.
- Returns: The name's length.
- Errors: `ERR_ARG`
- WAMR signature: `(i*~)i`

#### `dev_find`

```c
int32_t dev_find(const char *name);
```

Finds a device by name. ERR_DENIED if the app's manifest names it in neither requires nor uses.

- `name`: A well-known name (DEV_ constants) or one the gadget lists.
- Returns: The device.
- Errors: `ERR_NOT_FOUND`, `ERR_DENIED`
- WAMR signature: `($)i`

#### `dev_describe`

```c
int32_t dev_describe(int32_t device, void *buf, int32_t buf_size);
```

The device's description, as device.guide gives it (Devices, below). Apps written for one device rarely need it: Muse already read it.

- `buf`: Gets the description as JSON, cut to fit and NUL-terminated.
- Returns: The description's full length.
- Errors: `ERR_HANDLE`
- WAMR signature: `(i*~)i`

#### `dev_field`

```c
int32_t dev_field(int32_t device, const char *name);
```

Where a field is in a reading.
- Returns: The field's index in a reading's values.
- Errors: `ERR_HANDLE`, `ERR_NOT_FOUND`
- WAMR signature: `(i$)i`

#### `dev_read`

```c
int32_t dev_read(int32_t device, void *reading, int32_t reading_size);
```

The latest reading, without opening the device. ERR_NOT_FOUND until there is one: a sensor that streams starts on the first dev_read (taking a moment), and runs until the app stops or calls dev_close. Its t_ms says how fresh it is. To get every reading, use dev_open and dev_take instead.

- `reading`: One gadget_reading_t.
- Returns: How many values it holds.
- Errors: `ERR_HANDLE`, `ERR_NOT_FOUND`
- WAMR signature: `(i*~)i`

#### `dev_open`

```c
int32_t dev_open(int32_t device, int32_t rate_hz);
```

Starts streaming readings into a queue, and the device's events. EV_DEV_DATA says when readings wait, EV_DEV_EVENT brings events. Close it once the app stops needing it: a running sensor costs battery.

- `rate_hz`: Up to the device's rate; 0 for events only.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_LIMIT`, `ERR_BUSY`
- WAMR signature: `(ii)i`

#### `dev_take`

```c
int32_t dev_take(int32_t device, void *readings, int32_t readings_size);
```

Takes waiting readings off the queue.

- `readings`: gadget_reading_t array.
- Returns: Readings copied, oldest first.
- Errors: `ERR_HANDLE`
- WAMR signature: `(i*~)i`

#### `dev_close`

```c
int32_t dev_close(int32_t device);
```

Stops streaming and events.
- Errors: `ERR_HANDLE`
- WAMR signature: `(i)i`

#### `dev_action`

```c
int32_t dev_action(int32_t device, const char *name);
```

Finds an action by name.
- Returns: The action's index.
- Errors: `ERR_HANDLE`, `ERR_NOT_FOUND`
- WAMR signature: `(i$)i`

#### `dev_call`

```c
int32_t dev_call(int32_t device, int32_t action, const void *params, int32_t params_len, const char *text);
```

Runs an action. It doesn't block: a driver whose action takes time starts it and returns.

- `action`: Its index.
- `params`: float parameters, in the order the description gives.
- `text`: For actions that take text; "" otherwise.
- Returns: The action's result, 0 or more.
- Errors: `ERR_HANDLE`, `ERR_ARG`, `ERR_BUSY`, `ERR_FAILED`
- WAMR signature: `(ii*~$)i`

#### `dev_event`

```c
int32_t dev_event(int32_t device, const char *name);
```

Finds an event by name.
- Returns: The event's index, as EV_DEV_EVENT gives it.
- Errors: `ERR_HANDLE`, `ERR_NOT_FOUND`
- WAMR signature: `(i$)i`

## Widgets

| Type | LVGL | Parts | |
|---|---|---|---|
| `W_OBJ` | `lv_obj` | MAIN, SCROLLBAR | A plain container: a box with a background, border and children. |
| `W_LABEL` | `lv_label` | MAIN, SCROLLBAR, SELECTED | Text. |
| `W_BUTTON` | `lv_button` | MAIN | A container that presses; put a label in it. |
| `W_IMAGE` | `lv_image` | MAIN | A PNG (8-bit channels) or JPEG (baseline) (ui_image_load, ui_image_set). |
| `W_LINE` | `lv_line` | MAIN | A polyline through points (ui_line_points). |
| `W_ARC` | `lv_arc` | MAIN, INDICATOR, KNOB | A ring with a value, which touch can drag. |
| `W_BAR` | `lv_bar` | MAIN, INDICATOR | A progress bar. |
| `W_SLIDER` | `lv_slider` | MAIN, INDICATOR, KNOB | A bar with a knob to drag. |
| `W_SWITCH` | `lv_switch` | MAIN, INDICATOR, KNOB | An on/off toggle; P_CHECKED is its state. |
| `W_CHECKBOX` | `lv_checkbox` | MAIN, INDICATOR | A box with a label; P_CHECKED is its state. |
| `W_ROLLER` | `lv_roller` | MAIN, SELECTED | A drum of options to spin. |
| `W_DROPDOWN` | `lv_dropdown` | MAIN, INDICATOR | A button that opens a list of options. |
| `W_TEXTAREA` | `lv_textarea` | MAIN, SCROLLBAR, SELECTED, CURSOR | Editable text, for a keyboard. |
| `W_KEYBOARD` | `lv_keyboard` | MAIN, ITEMS | An on-screen keyboard that types into a text area. |
| `W_SCALE` | `lv_scale` | MAIN, INDICATOR, ITEMS | Ticks and labels in a line or a dial; ui_scale_needle points a line at a value. |
| `W_SPINNER` | `lv_spinner` | MAIN, INDICATOR | A spinning arc for 'working'. |
| `W_CHART` | `lv_chart` | MAIN, SCROLLBAR, INDICATOR, ITEMS, CURSOR | Line or bar series (ui_chart_series, ui_chart_set, ui_chart_push). |
| `W_TABLE` | `lv_table` | MAIN, ITEMS | Rows and columns of text (ui_table_cell). |
| `W_CANVAS` | `lv_canvas` | MAIN | Pixels to draw on with the cv_ calls. Its buffer is RGB565 and counts against the budget; P_WIDTH and P_HEIGHT size it, up to the screen. |

## Properties

Object properties (any object) and style properties work with `ui_set` and `ui_get`; style properties also with `ui_style` and `ui_style_set`. Widget properties apply to the widgets named.

| Property | Type | Where | Access | |
|---|---|---|---|---|
| `P_X` | coord | object | rw | Left edge, from the parent's content area, or the offset from P_ALIGN. |
| `P_Y` | coord | object | rw | Top edge, or the offset from P_ALIGN. |
| `P_WIDTH` | coord | object | rw |  |
| `P_HEIGHT` | coord | object | rw |  |
| `P_MIN_WIDTH` | coord | object | rw |  |
| `P_MAX_WIDTH` | coord | object | rw |  |
| `P_MIN_HEIGHT` | coord | object | rw |  |
| `P_MAX_HEIGHT` | coord | object | rw |  |
| `P_ALIGN` | align | object | rw | Where in the parent; P_X and P_Y offset it. |
| `P_HIDDEN` | bool | object | rw | Not drawn, and takes no input or space in a layout. |
| `P_CLICKABLE` | bool | object | rw | Takes touch. On for buttons and inputs, off for labels and images. |
| `P_SCROLLABLE` | bool | object | rw | Scrolls when its content is larger. On for containers. |
| `P_CHECKABLE` | bool | object | rw | A click toggles P_CHECKED. |
| `P_EVENT_BUBBLE` | bool | object | rw | Its events also go to its parent (one ui_on on a list instead of one per row). |
| `P_FLOATING` | bool | object | rw | Left out of the parent's layout and scrolling. |
| `P_CHECKED` | bool | object | rw | The checked state: a switch's or checkbox's value. |
| `P_DISABLED` | bool | object | rw | Greyed out; takes no input. |
| `P_SCROLL_DIR` | dir | object | rw |  |
| `P_SCROLLBAR_MODE` | scrollbar_mode | object | rw |  |
| `P_SCROLL_SNAP_X` | snap | object | rw | Which part of a child to stop on after a horizontal scroll. |
| `P_SCROLL_SNAP_Y` | snap | object | rw |  |
| `P_SCROLL_X` | int | object | r | How far it's scrolled; scroll with ui_scroll_to. |
| `P_SCROLL_Y` | int | object | r |  |
| `P_INDEX` | int | object | rw | Its place among its siblings, which sets the drawing order: 0 is drawn first, -1 moves it last. |
| `P_CHILD_COUNT` | int | object | r |  |
| `P_LAYOUT` | layout | object | rw |  |
| `P_FLEX_FLOW` | flex_flow | object | rw |  |
| `P_FLEX_MAIN_PLACE` | place | object | rw | Along the flow. |
| `P_FLEX_CROSS_PLACE` | place | object | rw | Across the flow, within a track (see flex_track_place). |
| `P_FLEX_TRACK_PLACE` | place | object | rw | Where the tracks sit across the flow. A row or column that doesn't wrap is one track, as wide (or tall) as its largest item, so centring its items across the flow takes PLACE_CENTER here and in flex_cross_place. |
| `P_FLEX_GROW` | int | object | rw | A child's share of the free space in its parent's flex track; 0 keeps its own size. |
| `P_BG_COLOR` | rgb | style | rw |  |
| `P_BG_OPA` | opa | style | rw |  |
| `P_BG_GRAD_COLOR` | rgb | style | rw | The gradient's end colour. |
| `P_BG_GRAD_DIR` | grad_dir | style | rw |  |
| `P_BORDER_COLOR` | rgb | style | rw |  |
| `P_BORDER_WIDTH` | int | style | rw |  |
| `P_BORDER_OPA` | opa | style | rw |  |
| `P_BORDER_SIDE` | border_side | style | rw |  |
| `P_OUTLINE_COLOR` | rgb | style | rw |  |
| `P_OUTLINE_WIDTH` | int | style | rw |  |
| `P_OUTLINE_PAD` | int | style | rw | Gap between the object and its outline. |
| `P_SHADOW_COLOR` | rgb | style | rw |  |
| `P_SHADOW_WIDTH` | int | style | rw | Blur width, 0 to 64. Shadows cost drawing time; use them on a few objects. |
| `P_SHADOW_OPA` | opa | style | rw |  |
| `P_SHADOW_OFFSET_X` | int | style | rw | -64 to 64. |
| `P_SHADOW_OFFSET_Y` | int | style | rw | -64 to 64. |
| `P_SHADOW_SPREAD` | int | style | rw | 0 to 64. |
| `P_RADIUS` | int | style | rw | Corner radius, 0 to 64; RADIUS_CIRCLE for round. |
| `P_PAD_ALL` | int | style | w | Sets the four paddings. |
| `P_PAD_LEFT` | int | style | rw |  |
| `P_PAD_RIGHT` | int | style | rw |  |
| `P_PAD_TOP` | int | style | rw |  |
| `P_PAD_BOTTOM` | int | style | rw |  |
| `P_PAD_ROW` | int | style | rw | Gap between rows of a layout. |
| `P_PAD_COLUMN` | int | style | rw | Gap between columns of a layout. |
| `P_TEXT_COLOR` | rgb | style | rw |  |
| `P_TEXT_OPA` | opa | style | rw |  |
| `P_TEXT_FONT` | font | style | rw |  |
| `P_TEXT_ALIGN` | text_align | style | rw |  |
| `P_TEXT_LETTER_SPACE` | int | style | rw |  |
| `P_TEXT_LINE_SPACE` | int | style | rw | Not negative. |
| `P_TEXT_DECOR` | text_decor | style | rw |  |
| `P_LINE_COLOR` | rgb | style | rw |  |
| `P_LINE_WIDTH` | int | style | rw |  |
| `P_LINE_OPA` | opa | style | rw |  |
| `P_LINE_ROUNDED` | bool | style | rw |  |
| `P_LINE_DASH_WIDTH` | int | style | rw | 0 to 64. |
| `P_LINE_DASH_GAP` | int | style | rw | 0 to 64. |
| `P_ARC_COLOR` | rgb | style | rw |  |
| `P_ARC_WIDTH` | int | style | rw |  |
| `P_ARC_OPA` | opa | style | rw |  |
| `P_ARC_ROUNDED` | bool | style | rw |  |
| `P_IMAGE_OPA` | opa | style | rw |  |
| `P_IMAGE_RECOLOR` | rgb | style | rw | Tints an image (or a symbol drawn as one). |
| `P_IMAGE_RECOLOR_OPA` | opa | style | rw |  |
| `P_OPA` | opa | style | rw | The whole object and its children. |
| `P_TRANSFORM_ROTATION` | angle | style | rw | Rotates the drawn object about its pivot. Costs drawing time and memory. |
| `P_TRANSFORM_SCALE` | percent | style | rw | Scales the drawn object about its pivot. |
| `P_TRANSFORM_PIVOT_X` | coord | style | rw |  |
| `P_TRANSFORM_PIVOT_Y` | coord | style | rw |  |
| `P_TRANSLATE_X` | coord | style | rw | Moves the drawn object without moving it in the layout. |
| `P_TRANSLATE_Y` | coord | style | rw |  |
| `P_LENGTH` | int | style | rw | A scale's tick length (PART_INDICATOR major, PART_ITEMS minor). |
| `P_CLIP_CORNER` | bool | style | rw | Clips children to the rounded corners. |
| `P_ANIM_DURATION` | int | style | rw | Milliseconds for a widget's own animations: a label's scrolling, a roller's spin. |
| `P_TEXT` | text | label, checkbox, textarea, dropdown | rw | The text. On a dropdown, fixed text to show in place of the selected option. |
| `P_LONG_MODE` | long_mode | label | rw |  |
| `P_VALUE` | int | arc, bar, slider | rw |  |
| `P_MIN_VALUE` | int | arc, bar, slider, scale, chart | rw | The range's low end; a chart's Y axis. |
| `P_MAX_VALUE` | int | arc, bar, slider, scale, chart | rw |  |
| `P_START_VALUE` | int | bar, slider | rw | The low end in BAR_RANGE mode. |
| `P_BAR_MODE` | bar_mode | bar, slider | rw |  |
| `P_ORIENTATION` | orientation | bar, slider, switch | rw |  |
| `P_ARC_MODE` | arc_mode | arc | rw |  |
| `P_START_ANGLE` | angle | arc | rw | The indicator's start; setting P_VALUE moves it too. |
| `P_END_ANGLE` | angle | arc | rw |  |
| `P_BG_START_ANGLE` | angle | arc | rw | The ring's start. |
| `P_BG_END_ANGLE` | angle | arc | rw |  |
| `P_ROTATION` | angle | arc, scale, image | rw | Turns the whole widget. |
| `P_OPTIONS` | text | roller, dropdown | rw | The options, one per line. |
| `P_SELECTED` | int | roller, dropdown | rw | The selected option, from 0. |
| `P_VISIBLE_ROWS` | int | roller | rw |  |
| `P_INFINITE` | bool | roller | rw | Spins round instead of stopping at the ends. ERR_ARG if letter spacing leaves its options no height. |
| `P_LIST_DIR` | dir | dropdown | rw | Which way the list opens. |
| `P_PLACEHOLDER` | text | textarea | rw | Text shown while it's empty. |
| `P_ONE_LINE` | bool | textarea | rw |  |
| `P_PASSWORD` | bool | textarea | rw | Shows bullets. |
| `P_MAX_LENGTH` | int | textarea | rw |  |
| `P_ACCEPTED_CHARS` | text | textarea | rw | The only characters it takes. |
| `P_CURSOR_POS` | int | textarea | rw |  |
| `P_TEXTAREA` | handle | keyboard | rw | The text area it types into. |
| `P_KEYBOARD_MODE` | keyboard_mode | keyboard | rw |  |
| `P_SCALE_MODE` | scale_mode | scale | rw |  |
| `P_TOTAL_TICKS` | int | scale | rw |  |
| `P_MAJOR_TICK_EVERY` | int | scale | rw | Every nth tick is major and labelled. |
| `P_LABEL_SHOW` | bool | scale | rw |  |
| `P_ANGLE_RANGE` | angle | scale | rw | A dial's sweep. |
| `P_SPIN_TIME` | int | spinner | rw | Milliseconds per turn. |
| `P_ARC_SWEEP` | angle | spinner | rw | The spinning arc's length. |
| `P_CHART_TYPE` | chart_type | chart | rw |  |
| `P_POINT_COUNT` | int | chart | rw | Points per series. |
| `P_UPDATE_MODE` | chart_update | chart | rw |  |
| `P_HOR_DIV_LINES` | int | chart | rw | Horizontal grid lines. |
| `P_VER_DIV_LINES` | int | chart | rw | Vertical grid lines. |
| `P_ROW_COUNT` | int | table | rw |  |
| `P_COLUMN_COUNT` | int | table | rw |  |
| `P_IMAGE_SCALE` | percent | image | rw | Zoom. |
| `P_OFFSET_X` | int | image | rw | Shifts the picture inside the object, wrapping round. |
| `P_OFFSET_Y` | int | image | rw |  |
| `P_INNER_ALIGN` | image_align | image | rw |  |
| `P_Y_INVERT` | bool | line | rw | Y points up from the bottom. |

## Devices

What the firmware says about each device, in device.guide and from dev_describe, as JSON. A driver registers its device with this description; nothing here lists the devices a gadget has.

| Key | Type | |
|---|---|---|
| `name` | string | a-z, 0-9 and '_', unique on the gadget. A well-known name when the device is one, so apps for it work unchanged; otherwise the driver's own. |
| `desc` | string | What it is and where it sits, for Muse: "Soil moisture probe on the Grove port". |
| `fields` | array | Its readings' values, in order: {"name", "unit", "min", "max"}. Up to 8; none for a device that only acts. |
| `rate` | int | The highest rate dev_open takes, in Hz; 0 when it's only read on demand (dev_read). |
| `actions` | array | What it does: {"name", "desc", "params": [{"name", "unit", "min", "max"}], "text": true if it takes text}. |
| `events` | array | Names of what it reports by itself ("tap", "door_open"), in EV_DEV_EVENT's order. |

For example:

```json
{"name": "soil_moisture", "desc": "Capacitive soil moisture probe on the Grove port", "fields": [{"name": "moisture", "unit": "%", "min": 0, "max": 100}], "rate": 1, "actions": [], "events": []}
```

### Well-known names

A device with one of these names has exactly these fields, actions and events, so an app for it runs on any gadget that has one. Fields are in this order.

| Name | Fields | Actions | Events | |
|---|---|---|---|---|
| `temperature` | c (°C) | — | — | Air temperature. |
| `humidity` | rh (%) | — | — | Relative humidity. |
| `pressure` | hpa (hPa) | — | — | Air pressure. |
| `co2` | ppm (ppm) | — | — | CO2. |
| `voc` | index | — | — | Volatile organic compounds, as Sensirion's VOC index (1 to 500). |
| `light` | lux (lx) | — | — | Ambient light. |
| `accel` | x (g), y (g), z (g) | — | tap, double_tap, shake, face_up, face_down | Acceleration in the screen's frame: x right, y up, z out of the screen. Events from the IMU's own feature engine where it has one. |
| `gyro` | x (deg/s), y (deg/s), z (deg/s) | — | — | Rotation rate, in accel's axes. |
| `steps` | count | — | — | Steps counted since dev_open, from the IMU's step counter. |
| `battery` | percent (%), volts (V), charging, usb | — | — | charging and usb are 0 or 1; volts is 0 where the board can't measure it. |
| `vibration` | — | buzz(ms ms) | — | A vibration motor. |
| `speaker` | — | tone(hz Hz, ms ms, volume %) | — | Beeps on the gadget's speaker; ERR_BUSY while Muse listens or speaks. |

## Constants

- **error**: `ERR_ARG`, `ERR_HANDLE`, `ERR_NO_MEMORY`, `ERR_LIMIT`, `ERR_BUSY`, `ERR_OFFLINE`, `ERR_DENIED`, `ERR_UNSUPPORTED`, `ERR_TOO_LARGE`, `ERR_NOT_FOUND`, `ERR_TIMEOUT`, `ERR_FAILED`. Why a call failed. Every one is negative.
- **info**: `INFO_API_VERSION`, `INFO_SCREEN_WIDTH`, `INFO_SCREEN_HEIGHT`, `INFO_ROUND`, `INFO_SAFE_INSET`, `INFO_TOUCH`, `INFO_BUTTONS`, `INFO_KNOB`, `INFO_MEM_BUDGET`, `INFO_MEM_USED`, `INFO_CONNECTED`. What sys_info reports.
- **stop**: `STOP_USER`, `STOP_REPLACED`, `STOP_CALL`, `STOP_VIDEO`, `STOP_EXIT`, `STOP_UPDATE`. Why an app is being closed (on_stop).
- **button**: `BUTTON_PRIMARY`, `BUTTON_SECONDARY`, `BUTTON_UP`, `BUTTON_DOWN`, `BUTTON_LEFT`, `BUTTON_RIGHT`, `BUTTON_ENTER`, `BUTTON_BACK`. Buttons, by role. Which exist depends on the board (INFO_BUTTONS). The power key always closes the app and is never reported. Holding BUTTON_PRIMARY (the talk button) for 2 s closes it too: device.guide names it as exit_hold; the app sees its BTN_DOWN and BTN_LONG, then on_stop, and never its BTN_UP. If the app gives the talk button back to Muse with muse_talk_key(1), that hold is reserved for talking instead; provide an on-screen Close control or use app.stop.
- **button_action**: `BTN_DOWN`, `BTN_UP`, `BTN_LONG`. What a button did (EV_BUTTON's b).
- **gesture**: `GESTURE_LEFT`, `GESTURE_RIGHT`, `GESTURE_UP`, `GESTURE_DOWN`. Swipe directions (EV_GESTURE).
- **muse_mode**: `MUSE_IDLE`, `MUSE_LISTENING`, `MUSE_THINKING`, `MUSE_SPEAKING`, `MUSE_ERROR`. What Muse is doing (muse_mode, EV_MUSE).
- **chat_event**: `CHAT_MESSAGE`, `CHAT_TEXT`, `CHAT_DONE`, `CHAT_BUSY`, `CHAT_CONNECTED`. What changed in the chat (EV_CHAT's a).
- **chat_info**: `CHAT_INFO_ROLE`, `CHAT_INFO_LENGTH`, `CHAT_INFO_DONE`, `CHAT_INFO_TRUNCATED`. What chat_info reports about a message.
- **chat_role**: `ROLE_USER`, `ROLE_MUSE`, `ROLE_APP`. Who wrote a message.
- **ui_event**: `UI_CLICKED`, `UI_PRESSED`, `UI_RELEASED`, `UI_LONG_PRESSED`, `UI_VALUE_CHANGED`, `UI_FOCUSED`, `UI_DEFOCUSED`, `UI_READY`, `UI_CANCEL`, `UI_SCROLL_END`, `UI_ANIM_DONE`, `UI_LOADED`, `UI_LOAD_FAILED`. Object events, as bits for ui_on; EV_UI's b holds one.
- **part**: `PART_MAIN`, `PART_SCROLLBAR`, `PART_INDICATOR`, `PART_KNOB`, `PART_SELECTED`, `PART_ITEMS`, `PART_CURSOR`. Parts of a widget, for ui_style and ui_style_add. Combine with a STATE_.
- **state**: `STATE_DEFAULT`, `STATE_CHECKED`, `STATE_FOCUSED`, `STATE_PRESSED`, `STATE_DISABLED`, `STATE_EDITED`. States, for ui_style and ui_style_add.
- **align**: `ALIGN_DEFAULT`, `ALIGN_TOP_LEFT`, `ALIGN_TOP_MID`, `ALIGN_TOP_RIGHT`, `ALIGN_BOTTOM_LEFT`, `ALIGN_BOTTOM_MID`, `ALIGN_BOTTOM_RIGHT`, `ALIGN_LEFT_MID`, `ALIGN_RIGHT_MID`, `ALIGN_CENTER`, `ALIGN_OUT_TOP_LEFT`, `ALIGN_OUT_TOP_MID`, `ALIGN_OUT_TOP_RIGHT`, `ALIGN_OUT_BOTTOM_LEFT`, `ALIGN_OUT_BOTTOM_MID`, `ALIGN_OUT_BOTTOM_RIGHT`, `ALIGN_OUT_LEFT_TOP`, `ALIGN_OUT_LEFT_MID`, `ALIGN_OUT_LEFT_BOTTOM`, `ALIGN_OUT_RIGHT_TOP`, `ALIGN_OUT_RIGHT_MID`, `ALIGN_OUT_RIGHT_BOTTOM`. Where to put an object, as in LVGL.
- **text_align**: `TEXT_ALIGN_AUTO`, `TEXT_ALIGN_LEFT`, `TEXT_ALIGN_CENTER`, `TEXT_ALIGN_RIGHT`.
- **font**: `FONT_DEFAULT`, `FONT_14`, `FONT_16`, `FONT_20`, `FONT_28`, `FONT_48`. The fonts every gadget has: Montserrat, with LVGL's symbols (SYM_).
- **layout**: `LAYOUT_NONE`, `LAYOUT_FLEX`, `LAYOUT_GRID`.
- **flex_flow**: `FLEX_ROW`, `FLEX_COLUMN`, `FLEX_ROW_WRAP`, `FLEX_COLUMN_WRAP`, `FLEX_ROW_REVERSE`, `FLEX_COLUMN_REVERSE`.
- **place**: `PLACE_START`, `PLACE_END`, `PLACE_CENTER`, `PLACE_SPACE_EVENLY`, `PLACE_SPACE_AROUND`, `PLACE_SPACE_BETWEEN`. How flex places items and tracks.
- **dir**: `DIR_NONE`, `DIR_LEFT`, `DIR_RIGHT`, `DIR_TOP`, `DIR_BOTTOM`, `DIR_HOR`, `DIR_VER`, `DIR_ALL`.
- **scrollbar_mode**: `SCROLLBAR_OFF`, `SCROLLBAR_ON`, `SCROLLBAR_ACTIVE`, `SCROLLBAR_AUTO`.
- **snap**: `SNAP_NONE`, `SNAP_START`, `SNAP_END`, `SNAP_CENTER`.
- **long_mode**: `LONG_WRAP`, `LONG_DOTS`, `LONG_SCROLL`, `LONG_SCROLL_CIRCULAR`, `LONG_CLIP`. What a label does with text that doesn't fit its size.
- **grad_dir**: `GRAD_NONE`, `GRAD_VER`, `GRAD_HOR`.
- **text_decor**: `DECOR_NONE`, `DECOR_UNDERLINE`, `DECOR_STRIKETHROUGH`.
- **border_side**: `BORDER_NONE`, `BORDER_BOTTOM`, `BORDER_TOP`, `BORDER_LEFT`, `BORDER_RIGHT`, `BORDER_FULL`.
- **arc_mode**: `ARC_NORMAL`, `ARC_SYMMETRICAL`, `ARC_REVERSE`.
- **bar_mode**: `BAR_NORMAL`, `BAR_SYMMETRICAL`, `BAR_RANGE`.
- **orientation**: `ORIENT_AUTO`, `ORIENT_HORIZONTAL`, `ORIENT_VERTICAL`.
- **keyboard_mode**: `KB_TEXT_LOWER`, `KB_TEXT_UPPER`, `KB_SPECIAL`, `KB_NUMBER`.
- **scale_mode**: `SCALE_HORIZONTAL_TOP`, `SCALE_HORIZONTAL_BOTTOM`, `SCALE_VERTICAL_LEFT`, `SCALE_VERTICAL_RIGHT`, `SCALE_ROUND_INNER`, `SCALE_ROUND_OUTER`.
- **chart_type**: `CHART_NONE`, `CHART_LINE`, `CHART_BAR`.
- **chart_update**: `UPDATE_SHIFT`, `UPDATE_CIRCULAR`.
- **image_align**: `IMAGE_CENTER`, `IMAGE_TOP_LEFT`, `IMAGE_STRETCH`, `IMAGE_TILE`, `IMAGE_CONTAIN`, `IMAGE_COVER`. How an image fills an image object larger or smaller than itself.
- **easing**: `EASE_LINEAR`, `EASE_IN`, `EASE_OUT`, `EASE_IN_OUT`, `EASE_OVERSHOOT`, `EASE_BOUNCE`, `EASE_STEP`. Animation curves; add ANIM_ flags.
- **anim_flag**: `ANIM_REPEAT`, `ANIM_REVERSE`.

## Manifest

A JSON object in the custom section gadget.manifest of the .wasm, which GADGET_MANIFEST() in gadget.h writes from one string literal. The gadget reads it before running the app.

| Field | Type | Required | |
|---|---|---|---|
| `api` | string | yes | The API version the app was written for, "1.0". A gadget serving an older minor version, or another major one, refuses the app. |
| `name` | string | yes | The app's folder name under ~/workspace/gadgets/apps/: a-z, 0-9 and '-'. |
| `title` | string | no | A name for people: the gadget's launcher shows it under the app's icon. |
| `icon` | string | no | Its glyph in the gadget's launcher, on a coloured disc: one of timer, clock, calendar, check, list, note, chat, mic, music, heart, pulse, moon, sun, cloud, drop, leaf, bolt, star, spark, smile, game, dice, target, bricks, book, chart, compass, globe, cup, bell, home, pin, camera, flag, wind. Without one the launcher shows the title's first letter. |
| `color` | string | no | The disc's colour: one of red, orange, yellow, green, mint, teal, blue, indigo, purple, pink, brown or gray. Without one the gadget picks one from the app's id. |
| `requires` | array | no | Features and device names it can't work without. The gadget refuses the app if one is missing. |
| `uses` | array | no | Permissions, and device names it uses when they're there. A device in neither list can't be found (ERR_DENIED). |

Features: `touch` (A touch screen.), `round` (A round screen.), `rect` (A rectangular screen.), `buttons2` (Both BUTTON_PRIMARY and BUTTON_SECONDARY.), `nav` (UP, DOWN, ENTER and BACK, or a knob.).

Permissions: `chat.read` (Follow the chat (chat_open and the other chat_ reads).), `chat.send` (Send messages to Muse (chat_send).), `files.home` (Read files outside the app's own folder (file_read with "~/").), `files.gadget` (Read the gadget's own files (file_read and ui_image_load with a path from "/").).
