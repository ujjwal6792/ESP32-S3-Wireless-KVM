# C++ Basics For This Project

## Why There Are `.h` And `.cpp` Files

In this repo, most modules are split into two files:

- `.h` means "header"
- `.cpp` means "implementation"

The header tells other files what exists.
Examples:

- a `struct`
- a function name
- a namespace
- constants that other files may use

The `.cpp` file contains the actual code that runs.

Example pattern:

```cpp
// display.h
namespace display {
bool begin();
void showStatus();
}
```

```cpp
// display.cpp
#include "display.h"

namespace display {
bool begin() { return true; }
void showStatus() {}
}
```

Why this helps:

- `main.cpp` stays smaller
- code is grouped by responsibility
- multiple files can use the same declarations
- compile errors are easier to localize

## What `#include` Does

`#include` copies declarations into the current file before compilation.

Example:

```cpp
#include "tft_display.h"
```

That lets `main.cpp` call:

```cpp
tft_display::begin(...);
```

without knowing how `begin()` is implemented internally.

## Why `namespace` Is Used

Namespaces prevent name collisions.

Example:

```cpp
namespace oled_ssd1306 {
  bool begin(...);
}
```

and

```cpp
namespace tft_display {
  bool begin(...);
}
```

Both modules can have a function called `begin()` because their full names are different:

- `oled_ssd1306::begin`
- `tft_display::begin`

## What A `struct` Is

A `struct` groups related values together.

Example from this project:

```cpp
struct Status {
  uint8_t slot_1based;
  const char *profile;
  const char *state;
  bool eco;
  bool connected;
};
```

That lets you pass one object instead of five separate arguments.

## Why Some Variables Are `static` Or Kept In Anonymous Namespaces

Inside a `.cpp` file, many helpers are private to that file only.

This project uses:

```cpp
namespace {
  // private helpers and private globals
}
```

That means code outside that `.cpp` file cannot access those internals directly.

This is useful because:

- it reduces accidental coupling
- it keeps the public API small
- it makes refactoring safer

## Mental Model For This Repo

- `src/main.cpp` is the orchestrator
- `src/oled_ssd1306.*` owns the OLED
- `src/tft_display.*` owns the TFT

`main.cpp` decides *when* to update displays.
The display modules decide *how* to draw.
