# Compatibility log

Everything a game asks for that this runtime cannot do (or does differently), and every error the game hits, is
counted in one table and written to the log with the prefix `[compat]`. It exists so that making a game work
starts from a list instead of from a screenshot.

## Reading it

The first time a problem happens it is logged in full:

```
[compat] canvas2d: set shadowBlur -- stored, has no effect on drawing (value 8) (t=41.3s frame 2480)
```

After that it is only counted. The line repeats at 10, 100, 1000... occurrences (`[compat] canvas2d: set shadowBlur x100`),
and a table of everything, most frequent first within each category, is printed once a minute (when something
changed; the first 40 entries) and in full at shutdown:

```
[compat] ---- summary at 300 s, frame 17000: 9 distinct, 5212 total ----
[compat]     4120x exception     TypeError: undefined is not a function @ foo@.../Foo.js:88:12 -- ... (first t=12.0s frame 700)
```

A run that ends without that block (a crash, a kill) still has every first occurrence in the log.

## Categories

| category | what it is |
|---|---|
| `exception` | an error nobody caught: in a frame callback, a script, a timer or the loader. Key is the message with numbers folded plus the first stack frame, so one failing plugin is one line with a count |
| `rejection` | a promise rejected with no handler (QuickJS: at once; JavaScriptCore: after the microtask drain) |
| `console-error`, `console-warn` | what the game, its plugins and our shims print with `console.error` / `console.warn`, numbers folded |
| `webgl` | a script asked for a WebGL context (`getContext(webgl)`), probed a parameter or extension, or used a member the stub context does not have (`gl.createShader`, ...) |
| `canvas2d` | 2D context features that are stored but not drawn (`shadowBlur`, `lineCap`, `imageSmoothingEnabled = false`, ...), unsupported composite operations, `createPattern`, `clip()` to a non-rectangle, `toDataURL`, missing methods (`setLineDash`, `roundRect`, ...), canvas memory limit |
| `render` | GPU side: device/pipeline/texture creation failed, blend modes beyond normal/add/multiply/screen, image handle limit |
| `shader` | plugin shader that could not be built: the reason (GLSL rewrite or compiler) and, once per shader, its source. Drawing continues unfiltered |
| `asset` | image that could not be read or decoded |
| `video` | video limits |
| `api` | anything else a shim refused (unknown context type, ...) |

## Adding a report

From C: `compat_report("category", "stable key", "detail %d", x)` (`engine/error_handler.h`). From a shim:
`globalThis.__compat_report("category", "stable key", "detail")`.

The key decides what counts as one problem. Put in it what identifies the *kind* of problem (the member name, the
unsupported value, the shader hash) and leave out what changes between occurrences (ids, coordinates, timestamps);
those go in the detail, which is shown from the first occurrence only.

## Limits

- 512 distinct problems; after that new ones are only counted (the table says so).
- Errors the game catches and swallows itself are invisible, unless it also prints them.
- Nothing is logged for features that are *missing but never asked for*: this is a log of use, not an audit.
