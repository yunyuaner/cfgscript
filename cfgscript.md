# cfgscript — A C Configuration File Preprocessing and Parsing Library with Scripting Capabilities

## 1. Project Overview

`cfgscript` is a **lightweight, embeddable C configuration library** with a core concept:

> **Configuration file = Script preprocessing (macros / conditionals / loops / expressions) + Standard KV configuration parsing**

The overall structure is similar to **C preprocessor (cpp) + ini / kv parser**, but specifically designed for embedded systems, toolchains, firmware configuration, and similar scenarios.

### Key Features

- Pure C implementation (C99)
- No third-party dependencies
- Scripting capabilities:
  - Macro definition / substitution
  - Conditional compilation
  - Loop expansion (supports nesting)
  - Integer expression evaluation
- Strict error checking with errors traceable to **original file + line number**
- Final output is standard KV configuration, convenient for program usage

---

## 2. Overall Architecture Design

```
           ┌─────────────────────┐
           │   input .cfg file   │
           └─────────┬───────────┘
                     │
                     ▼
        ┌─────────────────────────────┐
        │   Preprocessor (script)     │
        │  %define / %if / %for / %set│
        │  Expression eval, macro exp │
        └─────────┬───────────────────┘
                  │(plain text + line map)
                  ▼
        ┌─────────────────────────────┐
        │   KV Parser (config layer)  │
        │   key = value               │
        └─────────┬───────────────────┘
                  ▼
        ┌─────────────────────────────┐
        │     cfg_t runtime struct    │
        │  cfg_get_int / cfg_get_str  │
        └─────────────────────────────┘
```

### Benefits of Two-Stage Design

- **Scripting capabilities don't pollute configuration parsing**
- Expressions and loops can be arbitrarily complex, while final configuration remains simple
- Clear error localization (script stage / parsing stage separation)

---

## 3. Configuration File Syntax Overview

### 3.1 Comments

```cfg
# This is a comment
; This is also a comment
```

---

### 3.2 Regular Configuration Items (KV)

```cfg
key = value
key: value
```

- Automatically trims leading/trailing whitespace
- value supports strings / integers / booleans

---

## 4. Script Directives (Starting with `%`)

### 4.1 `%define` — Define Macro (Text)

```cfg
%define NAME value
```

- `value` will undergo `${}` macro substitution first
- Stored as a string

Example:

```cfg
%define PORT 8080
server.port = ${PORT}
```

---

### 4.2 `%undef` — Undefine Macro

```cfg
%undef NAME
```

---

### 4.3 `%set` — Evaluate Expression and Store as Integer (Highly Recommended)

```cfg
%set NAME expression
```

or

```cfg
%set NAME = expression
```

- expression is a **strict integer expression**
- Evaluated immediately and result is saved
- Used in subsequent `%if` / `%for`

Example:

```cfg
%define A 10
%define B 3
%set C A + B * 2     # C = 16
```

---

### 4.4 `%if / %elif / %else / %endif`

```cfg
%if expression
  ...
%elif expression
  ...
%else
  ...
%endif
```

- expression supports **arithmetic + comparison + logical** operations
- Arbitrary nesting
- Expression errors → immediately report error and abort loading

Example:

```cfg
%if (A + B) > 10 && defined(PORT)
mode = big
%else
mode = small
%endif
```

---

### 4.5 `%for / %endfor`

#### Form 1: List Loop

```cfg
%for i in a,b,c
key.${i} = yes
%endfor
```

#### Form 2: Range Loop (Integer)

```cfg
%for i in 1..4
node.${i}.enable = true
%endfor
```

- Supports **nested for** loops
- Loop variables are regular macros

Example:

```cfg
%for i in 1..2
%for j in 1..3
k.${i}.${j} = ${i}${j}
%endfor
%endfor
```

---

### 4.6 `%include`

```cfg
%include "other.cfg"
%include <path/to/file.cfg>
```

- Relative paths are based on the current file's directory
- Maximum nesting depth: `CFG_INCLUDE_DEPTH` (default 16)

---

## 5. Expression Language (expression)

### 5.1 Supported Operators

#### Arithmetic

```
+   -   *   /   %
```

#### Comparison

```
==  !=  <  <=  >  >=
```

#### Logical

```
!   &&   ||
```

#### Parentheses

```
( ... )
```

---

### 5.2 Operator Precedence (High to Low)

1. `!` `+` `-` (unary)
2. `* / %`
3. `+ -`
4. `< <= > >= == !=`
5. `&&`
6. `||`

---

### 5.3 Built-in Functions

#### `defined(NAME)`

```cfg
%if defined(PORT)
...
%endif
```

---

### 5.4 Error Example (Strict)

```cfg
%if (A / 0) > 1
```

Result:

```
input.cfg:3: in %if: division by zero
```

---

## 6. C API Usage

### 6.1 Load Configuration

```c
#include "cfgscript.h"

cfg_status_t st;
cfg_t* cfg = cfg_load("config.cfg", &st);

if (!cfg) {
    printf("load failed: %s\n", cfg_last_error());
    return -1;
}
```

---

### 6.2 Read Configuration Items

```c
int port = (int)cfg_get_int(cfg, "server.port", 80);
const char* host = cfg_get_str(cfg, "server.host", "localhost");
int enable = cfg_get_bool(cfg, "server.enable", 0);
```

---

### 6.3 Check If Key Exists

```c
if (cfg_has(cfg, "debug.enable")) {
    ...
}
```

---

### 6.4 Get Configuration Origin (Debugging Tool)

```c
const char* file;
int line;

if (cfg_get_origin(cfg, "server.port", &file, &line)) {
    printf("server.port defined at %s:%d\n", file, line);
}
```

---

### 6.5 Free Resources

```c
cfg_free(cfg);
```

---

## 7. Complete Example

```cfg
# example.cfg

%define BASE_PORT 8000
%set NODE_NUM 3

%for i in 1..${NODE_NUM}
node.${i}.port = ${BASE_PORT} + ${i}
node.${i}.enable = true
%endfor

%if NODE_NUM > 2
mode = cluster
%else
mode = single
%endif
```

Final equivalent expansion:

```cfg
node.1.port = 8001
node.1.enable = true
node.2.port = 8002
node.2.enable = true
node.3.port = 8003
node.3.enable = true
mode = cluster
```

---

## 8. Error Handling Conventions

- **Any script error will cause cfg_load() to fail**
- `cfg_last_error()` returns complete error description
- Error messages include:
  - Filename
  - Line number
  - Directive type
  - Specific reason

Example:

```
config/main.cfg:12: in %set: unexpected trailing tokens
```

---

## 9. Design Philosophy Summary

- **Configuration is data, not a program**
- Scripting capabilities are only used to "generate configuration"
- All complex logic is resolved during the "preprocessing stage"
- Runtime API remains extremely simple and stable

