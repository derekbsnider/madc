# Universal Application Interaction and Rendering Abstraction

**Status:** Proposed consolidated design  
**Date:** 2026-08-24  
**Purpose:** Define a minimal, extensible model for applications that can be
presented through line-mode text, a full TUI, a GUI, a web browser, an API,
voice, or future rendering systems without rewriting the application logic.

## Relationship to the Earlier Rendering Plan

This document is a successor to `rendering-abstraction.md`.

It retains the strongest parts of that design:

- semantic-first rendering;
- progressive capability levels;
- per-user accessibility and presentation preferences;
- one-way state flow and differential rendering;
- a semantic projection tree rather than platform-specific widgets;
- line-mode text as the universal minimum presentation;
- full TUI, graphical, native-widget, and spatial renderers as progressively
  richer realizations of the same application.

It adds a more fundamental application-interaction layer above the semantic
rendering tree:

```text
Resource + State
       |
       v
Actor + Context
       |
       v
Available Actions / Affordances
       |
       v
Semantic Projection
       |
       v
Capability-aware Frontend
```

The earlier file also references the approved
`2026-08-20-data-hub-projection-rendering.md` design. This document was first
drafted without it and incorporated only the changes explicitly recorded in
the earlier file:

- capability resolution is per connection rather than per binary;
- semantic content is value-first, using `madc::value`;
- classifications such as type, role, and action are registry-interned IDs;
- the library surface precedes any special `render {}` language syntax.

**2026-08-24 owner review:** the hub design's Decided items are now
incorporated — see "Decisions Incorporated (2026-08-24)" near the end of
this document — and the phased execution plan lives in
[2026-08-24-ui-interaction-rework-and-texteditor.md](2026-08-24-ui-interaction-rework-and-texteditor.md).

## Vision

An application should describe:

1. what resources exist;
2. what is currently true about them;
3. which resource is acting as the user or actor;
4. what actions are presently possible;
5. what information and actions should be presented for the current task.

It should not need to begin by describing buttons, menus, windows, cursor
positions, HTML elements, or terminal escape sequences.

The same application semantics should support:

- a scrolling Telnet-style line interface;
- a cursor-addressable TUI;
- a native GUI;
- a web browser;
- an API client;
- an AI or automation agent;
- a voice interface;
- a graphical or spatial game.

The physical interface is the final interpretation of the application, not
its primary definition.

## Core Principle

The fundamental interaction is:

```text
Actor --Action--> Target
```

or in code-oriented notation:

```cpp
actor -> action(target, arguments)
```

The Subject-Verb-Object model is useful conceptually, but `target` is the
clearer implementation term because the target may itself be another user or
actor.

Examples:

```cpp
player  -> move(room);
user    -> edit(document);
admin   -> suspend(account);
member  -> message(other_member);
player1 -> attack(player2);
customer-> buy(product, { .quantity = 2 });
```

Some actions involve additional resources with named semantic roles:

```cpp
player -> use(
    target     = door,
    instrument = brass_key
);

user -> move(
    target      = file,
    destination = directory
);

customer -> transfer(
    target = destination_account,
    source = source_account,
    amount = 100.00
);
```

`Actor`, `Target`, `Source`, `Destination`, and `Instrument` are roles played
by resources within an invocation. They do not require mutually exclusive
base classes.

## Goals

1. Represent games, editors, shops, BBSes, configuration tools, report
   generators, and ordinary business applications with the same primitives.
2. Separate application meaning from presentation technology.
3. Derive visible controls and navigation from data, actions, relationships,
   state, permissions, and context.
4. Allow the available action set to change dynamically.
5. Treat line-mode input and sequential output as first-class, not as an
   afterthought.
6. Permit rich TUI and GUI frontends without weakening the application model
   to the least common denominator.
7. Make the same action definitions usable by UI frontends, APIs, automation,
   and agents.
8. Keep domain state, interaction state, and presentation state separate.
9. Permit specialization through domain types, traits, registries, and
   composition.
10. Make authorization and precondition checking part of action execution,
    not merely visibility logic in the UI.

## Non-Goals

This design does not initially require:

- a custom graphical drawing engine;
- a custom terminal-control library;
- a new network protocol;
- a mandatory HTTP server;
- a rigid hierarchy of screens and widgets;
- a universal natural-language parser;
- special `render {}` syntax in Phase 1;
- every renderer to support every interaction technique.

Existing libraries can implement terminal grids, native widgets, layout,
text shaping, graphics, and network transport beneath this abstraction.

## 1. Layered Architecture

The system is divided into three conceptual layers.

### 1.1 Application Semantics

```text
Application
    Resource
    State
    Actor
    Action
    Rule
```

This layer defines what exists and what can happen.

It contains no terminal, GUI, browser, or widget assumptions.

### 1.2 Interaction Semantics

```text
Context
Affordance
Invocation
Projection
```

This layer determines:

- what is relevant to the actor now;
- what the actor is currently able to do;
- what the current task should expose;
- what semantic input is required.

### 1.3 Presentation

```text
Frontend
Renderer
Input Adapter
Surface
Render Profile
```

This layer determines:

- how information is laid out;
- how actions are physically exposed;
- how physical input maps back to semantic invocations;
- how much of the current projection can be updated in place.

### 1.4 Full Pipeline

```text
                     APPLICATION MODEL

                 Resources + Relationships
                          |
                          v
                    Current State
                          |
              +-----------+-----------+
              |                       |
            Actor                  Context
              |                       |
              +-----------+-----------+
                          |
                          v
                  Affordance Resolver
                          |
                          v
                  Available Actions
                          |
                          v
                  Semantic Projection
                          |
             +------------+-------------+
             |            |             |
          Line Mode       TUI        GUI / Web
             |            |             |
             +------------+-------------+
                          |
                     Physical Input
                          |
                          v
                      Invocation
                          |
                   validate + execute
                          |
                          v
                     State Change
```

## 2. Basic Building Blocks

### 2.1 `Application`

An application is a container for:

```cpp
struct Application {
    ResourceStore   resources;
    TypeRegistry    types;
    ActionRegistry  actions;
    PolicyRegistry  policies;
    ProjectionRegistry projections;
};
```

The exact implementation may be static, dynamic, graph-backed, relational,
or assembled from multiple data sources.

The abstraction only requires that resources can be identified, inspected,
related, and used in action invocations.

### 2.2 `Resource`

A resource is anything meaningful to the application.

Examples:

```text
User
Player
Room
Item
Message
File
TextDocument
Customer
Product
Order
Configuration
ReportDefinition
RunningJob
EditorSession
```

A value-first representation is suitable for the generic layer:

```cpp
using ResourceId = interned_id;
using TypeId     = interned_id;
using RelationId = interned_id;
using PropertyId = interned_id;

struct Resource {
    ResourceId id;
    TypeId     type;

    map<PropertyId, madc::value> values;
    map<RelationId, list<ResourceId>> relations;
};
```

A resource is not necessarily a database row. It may represent:

- a persistent domain entity;
- a derived query result;
- a virtual object;
- a local file;
- an external service;
- an interaction session;
- a running operation;
- a logical location;
- a collection or projection.

#### Domain Extension

Applications may add strongly typed wrappers or registered schemas:

```cpp
struct PlayerData {
    int score;
    int level;
    ResourceId location;
    list<ResourceId> inventory;
};

struct TextDocumentData {
    string path;
    TextBuffer text;
    bool modified;
    bool read_only;
};
```

The generic value-and-registry layer provides interoperability. Typed domain
layers provide validation, performance, and convenient programming.

### 2.3 `Actor`

An actor is a resource currently capable of initiating an action.

This is preferably a capability or role rather than a mandatory root-class
split:

```cpp
struct ActorFacet {
    ResourceId identity;
    PermissionSet permissions;
    PreferenceSet preferences;
};
```

A resource may acquire or lose actor capability according to the application.

Examples:

```text
Human user
Game player
NPC
Scheduled task
System service
Automation agent
Remote API client
```

Domain types can extend the concept:

```text
Resource
  + Actor capability
      + Player properties: score, level, inventory, location
```

The connected user and the semantic actor are often the same, but need not be.
A user may direct a character, service account, robot, or administrative
executor.

### 2.4 `State`

State is what is true now.

The design separates three categories.

#### Domain State

Meaningful to the application itself:

```text
player.location = room:B
door.locked = true
order.status = awaiting-payment
document.text = "..."
document.modified = true
```

#### Interaction State

Meaningful to the current task or session:

```text
selected resource
current editor caret
current text selection
current workflow step
pending action arguments
active search query
current interaction mode
```

#### Presentation State

Meaningful only to a frontend:

```text
terminal cursor coordinates
hovered widget
scrollbar thumb position
window rectangle
cell dirty flags
animation frame
physical focus ring
```

Domain and interaction state belong above the renderer. Presentation state
must not leak into the application model.

### 2.5 `Action`

An action is a reusable semantic operation.

```cpp
using ActionId = interned_id;

struct ActionDefinition {
    ActionId id;
    TypeId   target_type;

    ParameterSchema parameters;
    ResultSchema    result;

    Availability (*check)(const Invocation&);
    ActionResult (*execute)(Invocation&);
};
```

Examples:

```text
move
look
examine
take
open
save
insert_text
delete_range
buy
refund
approve
restart
generate_report
```

CRUD actions are useful defaults:

```text
create
read
update
delete
```

but domain actions are equally fundamental:

```text
move
attack
publish
checkout
compile
send
approve
refund
```

The action model must not force every operation into CRUD terminology.

#### Action Parameters

Parameters are semantic values and resource roles, not UI controls:

```cpp
action rename {
    target: File;

    parameters: {
        name: string {
            required: true;
        }
    };
}
```

The frontend later decides whether `name` is collected through:

- a line prompt;
- a TUI field;
- a native textbox;
- a web form;
- voice input;
- a programmatic API argument.

### 2.6 `Invocation`

An invocation is a specific attempt to perform an action.

```cpp
struct Invocation {
    ResourceId actor;
    ActionId   action;
    ResourceId target;

    map<interned_id, madc::value> arguments;
    ContextId context;
};
```

Examples:

```text
Derek -> move(Library)
Derek -> take(BrassKey)
Derek -> save(foo.cpp)
Admin -> suspend(Account42)
Alice -> message(Derek, text="Hello")
```

The action must validate the invocation at execution time even if the UI
previously advertised it as available.

### 2.7 `Context`

Context is the semantic scope relevant to an actor right now.

```cpp
using ContextId = interned_id;
using ModeId    = interned_id;

struct Context {
    ContextId id;

    ResourceId actor;
    ResourceId focus;
    list<ResourceId> scope;

    ModeId mode;
    map<interned_id, madc::value> interaction_state;
};
```

For a game:

```text
actor: Player Derek
focus: Stone Hall
scope:
    Derek
    Stone Hall
    Brass Key
    North Exit
mode: normal-play
```

For an editor:

```text
actor: Derek
focus: foo.cpp
scope:
    foo.cpp
    editor session
mode: insert
interaction state:
    caret = line 7, column 1
    selection = none
```

Context is not merely a visual screen. It determines which resources and
interaction rules are currently relevant.

### 2.8 `Affordance`

An affordance is an action currently available to an actor in a particular
context.

An action is reusable and abstract. An affordance is bound and contextual.

```cpp
struct Affordance {
    ActionId   action;
    ResourceId target;

    ResourceId provider;       // optional: why this is available
    map<interned_id, madc::value> bound_arguments;

    Availability availability;
    string label;
    string description;
};
```

Examples:

```text
Action definition:
    move(Room)

Current affordance:
    label: "Go north"
    action: move
    target: Library
    provider: NorthExit
```

```text
Actor: Derek
Action: unlock
Target: Door
Provider: BrassKey
```

```text
Actor: Derek
Action: buy
Target: Sword
Provider: Shop
```

```text
Actor: Derek
Action: save
Target: foo.cpp
Provider: EditorContext
```

The `provider` concept captures an important lesson from MUD systems: actions
may become available because of the actor, the target, the location, an item,
a role, a mode, or another contextual resource.

#### Resolving Affordances

Conceptually:

```cpp
AffordanceSet resolve_affordances(const Context& context) {
    return gather_application_actions(context)
         + gather_actor_actions(context)
         + gather_focus_actions(context)
         + gather_related_resource_actions(context)
         + gather_mode_actions(context)
         - prohibited_actions(context);
}
```

The actual implementation may use registries, graph traversal, type traits,
policy rules, inheritance, composition, or precompiled dispatch tables.

### 2.9 `Rule` and `Availability`

A rule determines whether an invocation is currently permitted and what must
be true for it to succeed.

```cpp
struct Availability {
    bool visible;
    bool enabled;
    string reason;
};
```

A frontend may:

- hide unavailable actions;
- show them disabled;
- explain why they are unavailable;
- expose them to diagnostics or help systems.

Example:

```text
move(player, room:C)

requires:
    player.location is room:B
    room:B connects to room:C
    connecting door is unlocked
```

Knowing the identity or URI of `room:C` does not make the move legal.
Resource addressability and transition reachability are distinct.

## 3. Projection: The Logical User Interface

### 3.1 Projection Rather Than Physical Screen

A projection selects and organizes what the actor should perceive and what
may be done next.

It is the logical screen, view, report, prompt, editor, or interaction surface.

```cpp
using RoleId = interned_id;

struct ProjectionNode {
    RoleId role;
    Binding binding;
    madc::value content;
    NodeState state;

    list<AffordanceRef> actions;
    list<ProjectionNode> children;

    PresentationHints hints;
};
```

`Binding` connects a node to its semantic origin:

```text
resource
resource property
relationship
collection
interaction-state value
affordance
action parameter
```

The renderer should not need to rediscover what displayed content means.

### 3.2 Minimal Semantic Roles

The initial projection vocabulary should remain small:

```text
Group
Heading
Content
Value
Collection
Choice
EditValue
EditText
Status
Navigation
Progress
Image
Separator
```

Roles describe meaning, not physical controls.

Examples:

```text
Choice      -> numbered menu, arrow-key list, buttons, links, voice choices
EditValue   -> line prompt, TUI field, textbox, date picker, API argument
EditText    -> line editor, TUI editor, GUI text area, browser editor
Action      -> command, hotkey, menu item, button, hyperlink, API operation
Collection  -> lines, table, listbox, cards, graph, map
Progress    -> periodic text, TUI bar, GUI bar, notification stream
```

### 3.3 `Screen`, `Menu`, and `TextEditor`

A logical `Screen` can be treated as a root `Projection`.

However, `Menu` and `TextEditor` should not normally form a rigid inheritance
hierarchy such as:

```text
Screen
  + Menu
  + TextEditor
```

A real editor screen may contain a menu, editable text, status information,
search input, and dialogs at the same time.

Composition is more expressive:

```text
Projection / Screen
  Group
    Choice       // menu or command set
    EditText     // document editing
    Status       // mode, line, column
    EditValue    // search prompt
```

Therefore:

```text
Menu
    = a Choice projection over affordances

Text editor
    = an EditText projection bound to a TextDocument and EditorContext

Screen
    = the current root projection

Physical screen
    = a frontend-specific realization of that projection
```

Convenience classes or domain-specific projection types may still be built on
these primitives.

### 3.4 Interaction Patterns

A small set of interaction patterns can cover most applications:

```text
Present
    communicate information

Choose
    select one or more alternatives

Provide
    supply a value or resource reference

Edit
    modify an existing value or resource

Navigate
    follow a relationship or change interaction context

Invoke
    perform an action

Confirm
    explicitly approve an action
```

`Confirm` may internally be a constrained `Choose`, but remains useful as a
semantic role because frontends and accessibility systems may treat it
specially.

## 4. Frontends and Capabilities

### 4.1 Frontend Responsibilities

A frontend combines:

1. a renderer;
2. an input adapter;
3. a surface and connection;
4. capability and user-preference negotiation.

```cpp
struct Frontend {
    RenderProfile profile;

    void present(const Projection& projection);
    PhysicalInput receive();

    Invocation interpret(
        const PhysicalInput& input,
        const Context& context,
        const AffordanceSet& affordances
    );
};
```

The application produces semantic projections and invocations. The frontend
owns physical layout, terminal control, widget construction, and event
translation.

### 4.2 Capabilities Are Per Connection

Capabilities must be resolved per connection/session, not per binary.

One running server may simultaneously support:

```text
Telnet line-mode user
ANSI TUI user
web-browser user
screen-reader user
API client
administrative GUI
```

The JIT may compile and cache specialized render variants for common profiles,
but correctness cannot assume that the whole process has one presentation
capability level.

### 4.3 Surface Capabilities

Output and input capabilities should be described independently.

```cpp
struct SurfaceCapabilities {
    // Output model
    bool sequential_output;
    bool addressable_output;
    bool mutable_output;
    bool multiple_regions;

    // Input model
    bool line_input;
    bool key_input;
    bool pointer_input;
    bool touch_input;
    bool voice_input;

    // Presentation facilities
    bool color;
    bool styles;
    bool unicode;
    bool images;
    bool graphics_2d;
    bool native_widgets;
    bool spatial_3d;

    // Logical viewport
    int width;
    int height;
};
```

A line-mode Telnet connection may still have a known logical width and height
even though it cannot update previous output.

### 4.4 Retained Capability Levels

The earlier five levels remain useful as coarse presets.

#### Level 0: Sequential Text

```text
Targets:
    dumb terminal
    non-interactive Telnet line mode
    stdout
    logs
    printer stream
    voice linearization

Output:
    sequential
    not addressable
    not mutable

Typical input:
    whole line
```

#### Level 1: Addressable Character Grid

```text
Targets:
    VT100+
    curses/ncurses
    SSH TUI

Output:
    addressable cells
    in-place updates
    attributes and optional color

Typical input:
    individual keys
    optional mouse
```

#### Level 2: 2D Graphics and Text Layout

```text
Targets:
    Canvas
    Cairo
    Skia
    PDF
    SVG
    printer page
```

#### Level 3: Native or Declarative Widgets

```text
Targets:
    GTK
    Qt
    Cocoa
    WinUI
    browser DOM
    mobile UI
```

#### Level 4: GPU / 3D / Spatial

```text
Targets:
    WebGPU
    Vulkan
    Metal
    DirectX
    game engines
    XR
```

The individual capability flags remain authoritative. A renderer need not fit
perfectly into one level.

### 4.5 User and Accessibility Profile

The effective presentation is determined by:

```text
surface capabilities
+ user preferences
+ accessibility requirements
+ current interaction context
```

Examples:

```text
prefer line mode on a capable terminal
keyboard-only operation
screen-reader linearization
high contrast
large text
reduced motion
monochrome output
no Unicode box drawing
```

The same semantic projection can therefore be rendered differently for two
users on equivalent hardware.

## 5. Generic Application Loop

The universal interaction loop is:

```cpp
while (session.connected()) {
    Context context = build_context(session);

    AffordanceSet affordances =
        resolve_affordances(context);

    Projection projection =
        project(context, affordances);

    frontend.present(projection);

    PhysicalInput input =
        frontend.receive();

    Invocation invocation =
        frontend.interpret(input, context, affordances);

    ActionResult result =
        validate_and_execute(invocation);

    apply(result);
}
```

For a reactive frontend, presentation and input may be event-driven rather
than a literal blocking loop, but the semantic cycle remains the same:

```text
state
 -> context
 -> affordances
 -> projection
 -> physical interaction
 -> invocation
 -> validated state transition
```

## 6. Example One: Simple MUD in Line Mode

### 6.1 Domain Resources

```cpp
struct PlayerData {
    int score;
    int level;
    ResourceId location;
    list<ResourceId> inventory;
};

struct RoomData {
    string name;
    string description;
    map<string, ResourceId> exits;
};

struct ItemData {
    string name;
    string description;
    bool takeable;
    ResourceId location;
};
```

The world state might contain:

```text
Player: Derek
    score: 25
    level: 2
    location -> Stone Hall

Room: Stone Hall
    contents -> Brass Key
    north -> Library

Item: Brass Key
    takeable: true
    location -> Stone Hall
```

### 6.2 Actions

```text
look
    target: Resource

examine
    target: Resource

move
    target: Room
    requires:
        target is connected to actor.location
        route is passable
    effect:
        actor.location = target

take
    target: Item
    requires:
        item.location == actor.location
        item.takeable
    effect:
        item.location = actor

inventory
    target: actor
```

### 6.3 Context and Affordance Resolution

Current context:

```text
actor: Derek
focus: Stone Hall
scope:
    Derek
    Stone Hall
    Brass Key
    North Exit
mode: normal-play
```

Resolved affordances:

```text
look(Stone Hall)
examine(Brass Key)
take(BrassKey)
move(Library), label="Go north", provider=NorthExit
inventory(Derek)
```

There is no direct `move` affordance to an unrelated room, even if the actor
or client knows that room's ID.

### 6.4 Semantic Projection

```cpp
Projection current_room(Context context, AffordanceSet actions) {
    return group({
        heading(context.focus.name),
        content(context.focus.description),
        collection("You see", context.visible_contents),
        collection("Exits", context.available_exits),
        choice(actions.primary)
    });
}
```

The projection contains no numbered choices, ANSI sequences, cursor
coordinates, or Telnet assumptions.

### 6.5 Line-Mode Rendering

```text
Stone Hall
==========

A narrow stone hall stretches into darkness.

You see:
  a brass key

Exits:
  north

1) Examine the brass key
2) Take the brass key
3) Go north
4) View inventory

>
```

The line-mode frontend may recognize:

```text
3
north
go north
```

as equivalent physical expressions of:

```cpp
Invocation {
    actor  = Derek,
    action = move,
    target = Library
}
```

The server validates the invocation and changes domain state:

```text
Derek.location:
    Stone Hall -> Library
```

The next logical screen is emitted sequentially:

```text
Library
=======

Dusty books fill shelves from floor to ceiling.

You see:
  a wooden desk
  an old lantern

Exits:
  south

1) Examine the desk
2) Take the lantern
3) Go south
4) View inventory

>
```

The application remains screen-aware. The physical output is append-only.

### 6.6 Same Game Through a TUI or GUI

The same projection and affordances could be rendered as:

```text
TUI:
    room description panel
    inventory panel
    selectable exits
    status bar

GUI:
    graphical room
    object list or scene objects
    clickable exits
    score and level display

3D:
    rendered room and actual paths
```

All invoke the same semantic actions.

## 7. Example Two: JOE-Like TUI Text Editor

### 7.1 Resources

```cpp
struct TextDocumentData {
    string path;
    TextBuffer text;
    bool modified;
    bool read_only;
};

struct EditorSessionData {
    ResourceId document;
    TextPosition caret;
    TextRange selection;
    EditorMode mode;
    SearchState search;
};
```

The document and editor session are separate resources or state components.

### 7.2 State Ownership

| State | Examples | Owner |
|---|---|---|
| Domain state | document text, path, modified flag | application/document |
| Interaction state | caret, selection, insert/overwrite mode, search query | editor context/session |
| Presentation state | terminal cell positions, dirty rows, physical cursor | TUI frontend |

The caret is interaction state because it affects semantic editing and could
also be represented by a line editor. The terminal cursor is presentation
state because it exists only in one frontend.

### 7.3 Editor Actions

```text
insert_text
    target: TextDocument
    parameters:
        position
        text

delete_range
    target: TextDocument
    parameters:
        range

replace_range
    target: TextDocument
    parameters:
        range
        text

move_caret
    target: EditorSession
    parameters:
        direction or position

save
    target: TextDocument
    requires:
        document is writable

search
    target: EditorSession
    parameters:
        pattern

quit
    target: EditorSession
```

The active interaction mode changes the available affordances:

```text
read-only document:
    remove insert, delete, replace, save

selection exists:
    add cut and copy

search mode:
    printable input edits the search pattern
    rather than the document

modified document:
    quitting may require confirmation
```

### 7.4 Semantic Projection

```cpp
Projection editor_view(EditorContext context) {
    return group({
        heading(context.document.path),

        edit_text(
            binding   = context.document.text,
            caret     = context.caret,
            selection = context.selection
        ),

        status({
            context.mode,
            context.caret.line,
            context.caret.column,
            context.document.modified
        }),

        choice({
            affordance(save, context.document),
            affordance(search, context.editor_session),
            affordance(quit, context.editor_session)
        })
    });
}
```

### 7.5 TUI Rendering

```text
+-- foo.cpp -------------------------------------------------- [modified] -+
|                                                                    |
|  1  #include <iostream>                                            |
|  2                                                                 |
|  3  int main() {                                                   |
|  4      std::cout << "Hello";                                      |
|  5      return 0;                                                   |
|  6  }                                                              |
|                    _                                               |
|                                                                    |
+--------------------------------------------------------------------+
| INSERT    Ln 7, Col 1                 ^S Save  ^F Find  ^Q Quit     |
+--------------------------------------------------------------------+
```

The TUI input adapter translates physical events:

```text
Printable "x"
    -> insert_text(document, position=caret, text="x")

Left arrow
    -> move_caret(editor_session, direction=left)

Ctrl-S
    -> save(document)

Ctrl-F
    -> enter search interaction context

Ctrl-Q
    -> quit(editor_session)
```

A run of printable keys may be coalesced into one semantic invocation:

```cpp
insert_text(
    target   = document,
    position = caret,
    text     = "hello"
);
```

The application should not be forced to process five heavyweight domain
transactions merely because the terminal supplied five key events.

### 7.6 Differential Update

After an edit:

1. the document text changes;
2. the caret changes;
3. affected projection bindings are reevaluated;
4. the TUI renderer compares the previous and current projection;
5. only changed terminal cells and status fields are redrawn.

Cursor movement, dirty-cell tracking, and terminal escape sequences remain
inside the TUI renderer.

### 7.7 Same Editor in Line Mode

The same resources and actions can be presented through an `ed`-like or
friendlier line editor:

```text
Lines 20-25:

20: void save_file() {
21:     ...
22: }
23:
24: int main() {
25:     ...

Commands:
  p <range>       print lines
  c <line>        replace line
  i <line>        insert before line
  d <range>       delete lines
  f <text>        find
  w               save
  q               quit

> c 22
Replacement text:
```

This still produces:

```cpp
replace_range(
    target = document,
    range  = line_22,
    text   = replacement
);
```

The TUI editor and line editor are different physical interaction techniques
for the same semantic document actions.

## 8. How Other Applications Map to the Model

### 8.1 BBS

```text
Actor:
    Member

Resources:
    Board
    Thread
    Message
    Other Member

Actions:
    read
    post
    reply
    edit
    delete
    message

Projection:
    current board, message, or menu
```

### 8.2 Shop

```text
Actor:
    Customer or Staff Member

Resources:
    Product
    Cart
    Order
    Payment

Actions:
    view
    add_to_cart
    remove_from_cart
    checkout
    pay
    refund
```

### 8.3 Configuration Editor

```text
Actor:
    User or Administrator

Resources:
    Configuration
    Section
    Setting

Actions:
    inspect
    modify
    reset
    validate
    save
```

A Boolean setting is not intrinsically a checkbox. It may become:

```text
line mode:  Enable networking? [y/n]
TUI:        [X] Enable networking
GUI:        checkbox or switch
API:        true / false
```

### 8.4 Report Generator

```text
Actor:
    User

Resources:
    DataSet
    Query
    Filter
    ReportDefinition
    GeneratedReport

Actions:
    select_fields
    filter
    sort
    group
    generate
    export
```

### 8.5 File Manager

```text
Actor:
    User

Resources:
    File
    Directory

Actions:
    inspect
    open
    rename
    copy
    move
    delete
```

## 9. API and URI Alignment

Resources should have stable identities that may be URI-like without requiring
HTTP:

```text
player:42
room:library
file:/etc/httpd.conf
customer:1742
order:93822
sqlite:customers/1742
falkor:Person/1742
```

The important properties are:

- identity;
- addressability;
- relationships;
- typed values;
- available actions.

A resource representation and a state transition are distinct.

```text
inspect room:C
```

is not equivalent to:

```text
move player from room:A to room:C
```

The first may be permitted because the room is addressable. The second may be
rejected because no valid transition exists from A to C.

The same semantic action engine should serve:

```text
local UI
remote API
web frontend
command parser
automation agent
```

The API and UI therefore share authorization, validation, action definitions,
and state transitions rather than duplicating them.

## 10. Lessons Incorporated from Existing Systems

### cdebconf

Keep semantic values and questions independent of physical controls. A Boolean
value is not a checkbox; a constrained value is not inherently a dropdown.

### Glk

Separate application semantics from the capabilities of the presentation
environment. A logical screen and line input can survive even when cursor
addressing and in-place updates do not.

### newt / curses-style systems

Use widget, layout, focus, and cell-update machinery inside the TUI backend,
not as the universal application model.

### LambdaMOO

Treat resources as generic objects with properties, relationships, and
behaviour. Resolve actions through the actor, context, and referenced targets
rather than forcing every verb to belong to one conventional OO receiver.

### LPC / LPMud

Allow nearby or related resources to contribute actions dynamically. The set
of things an actor can do changes when location, inventory, relationships, or
mode changes.

### PennMUSH

Distinguish the initiator of an action from the code or service that executes
it. Use reusable policy/lock concepts to gate actions.

### MUCK

Permit actions themselves to have identity and metadata and to be attached to
resources or contexts.

### Evennia

Compose the current command/action set from the session, actor, inventory,
location, nearby resources, and active mode. Normalize multiple physical
protocols into semantic input and output.

### Hypermedia APIs

Represent resources together with relationships and currently available
transitions. Describe action parameters semantically so a human UI, API
client, or agent can collect or supply them differently.

## 11. Design Decisions

| Decision | Choice | Reason |
|---|---|---|
| Primary application model | Resource + Actor + Action + State | Works across games, editors, BBSes, shops, and APIs |
| Core invocation | Actor -> Action(Target, Arguments) | Clear semantic SVO model without overloading `object` |
| Actor and target | Roles played by resources | Another actor may be the target; roles vary per invocation |
| Available UI operations | Contextual affordances | Availability depends on actor, target, state, provider, and mode |
| Menu model | Choice among affordances | Renderer chooses numbers, keys, list selection, buttons, or links |
| Editor model | EditText bound to document and editor context | Supports direct TUI editing and line-mode editing |
| Screen model | Root semantic projection | Logical screen remains independent of physical mutability |
| Presentation construction | Composition over screen-widget inheritance | A screen can simultaneously contain menu, editor, status, and prompts |
| Semantic storage | `madc::value` plus interned IDs | Flexible data-substrate interoperability and extensibility |
| Capability resolution | Per connection/session | One application may serve line, TUI, web, GUI, and API clients together |
| Authorization | Shared action policy and execution validation | UI visibility is not a security boundary |
| Rendering | Capability-aware interpretation of projection | Same semantics, appropriate presentation per target |
| Reactivity | One-way flow with dependency-aware updates | Predictable state and efficient differential rendering |
| Lowest common output | Sequential text | Provides a universal baseline without limiting richer renderers |

## 12. Proposed Minimal Library Surface

The first implementation should expose ordinary library APIs before adding
special language syntax.

```cpp
namespace ui {

struct Resource;
struct ActionDefinition;
struct Invocation;
struct Context;
struct Affordance;
struct ProjectionNode;
struct Frontend;

ResourceId create_resource(TypeId type);
void set(ResourceId resource, PropertyId property, madc::value value);
void relate(ResourceId source, RelationId relation, ResourceId target);

ActionId register_action(ActionDefinition definition);

AffordanceSet resolve_affordances(Context context);
Projection project(Context context, AffordanceSet affordances);

ActionResult invoke(Invocation invocation);

void present(Frontend& frontend, const Projection& projection);

}
```

Possible projection helpers:

```cpp
heading(value);
content(value);
value(binding);
collection(binding);
choice(affordances);
edit_value(binding);
edit_text(binding, editor_context);
status(values);
progress(operation);
group(children);
```

These helpers create semantic projection nodes, not physical widgets.

## 13. Implementation Plan

### Phase 1: Core Resources and Actions

- define interned IDs and `madc::value`-based resources;
- implement resource properties and relationships;
- implement action registry;
- implement invocation validation and execution;
- implement actor and target roles;
- gate: programmatically invoke actions against resources.

### Phase 2: Context and Affordance Resolution

- define `Context`;
- gather actions from application, actor, focus, related resources, and mode;
- implement provider tracking;
- implement visible/enabled/reason availability states;
- implement shared authorization checks;
- gate: query the valid actions for an actor in a given context.

### Phase 3: Semantic Projection and Line Frontend

- define `ProjectionNode`, semantic roles, and bindings;
- implement `Group`, `Heading`, `Content`, `Collection`, `Choice`, and `Status`;
- implement line-mode renderer;
- implement numbered/letter choice mapping;
- implement whole-line input interpretation;
- gate: run the example MUD through an append-only line connection.

### Phase 4: TUI Frontend

- add addressable character-grid backend through ncurses/newt/another library;
- implement layout, focus, key mapping, and differential cell updates;
- map `Choice` to navigable lists and commands;
- preserve semantic invocations above the renderer;
- gate: run the same MUD through a full TUI.

### Phase 5: `EditValue` and `EditText`

- add action-parameter collection;
- implement field validation;
- implement editor interaction context;
- implement caret, selection, search, and editor modes;
- implement key-event coalescing into semantic edit actions;
- gate: run a JOE-like TUI editor and a line-mode editor on the same
  `TextDocument` actions.

### Phase 6: Per-Connection Render Profiles

- negotiate capabilities per session;
- incorporate user and accessibility preferences;
- cache JIT-specialized render paths by profile where useful;
- support simultaneous line-mode and TUI clients in one process;
- gate: two clients see appropriate presentations of the same live state.

### Phase 7: Web / GUI / API Frontends

- serialize projections and affordances for remote clients;
- implement browser or native-widget mapping;
- expose action schemas to API and agent clients;
- ensure all clients invoke the same action engine;
- gate: one application operates correctly through line, TUI, browser, and API.

### Phase 8: Compiler Integration

- statically track projection dependencies;
- generate targeted reevaluation paths;
- introduce optional declarative syntax after the library semantics stabilize;
- dead-code-eliminate unsupported rendering branches within each specialized
  frontend/profile;
- gate: ergonomic source syntax with no loss of semantic introspection.

## 14. Required Invariants

1. A frontend never grants permission to perform an action.
2. Every invocation is validated by the action engine at execution time.
3. Physical controls never become the canonical form of an action.
4. A `Target` is a resource role, not a separate mutually exclusive object
   category.
5. The same semantic invocation can originate from line input, a TUI key, a
   GUI event, a web request, or an agent call.
6. Domain state never depends on terminal cursor positions or widget focus.
7. Presentation state never becomes the sole record of meaningful user work.
8. A line-mode frontend may know the logical screen dimensions while remaining
   append-only.
9. Resource addressability does not imply that every state transition to that
   resource is valid.
10. Hidden affordances remain protected by execution-time policy checks.
11. Menus and editors are semantic compositions, not mandatory subclasses of
    one physical screen implementation.
12. A richer renderer may enhance presentation but must not silently alter the
    meaning of the application action.

## Decisions Incorporated (2026-08-24)

Carried in from the approved hub design
(`2026-08-20-data-hub-projection-rendering.md`, "Decided" section) and the
2026-08-24 owner review; these are settled, not open:

1. **Actions are data with ONE registry and TWO binding kinds — native
   (compiled host function) and script-entity (madc source stored as a
   first-class named code entity) — both first-class and permanent.**
   Whether a given verb's body lives in the hub or in compiled host code
   is a per-application deployment choice. Hot, semantically-stable
   primitives (e.g. `insert_text` at typing cadence) stay native;
   turn-cadence domain/mod logic is the script kind's natural home
   (the Emacs / Neovim / VS Code / Unreal / LPMud convergence).
   Anti-drift mechanism: the texteditor pilot's gate REQUIRES at least
   one verb executing from madc source through the same registry — a
   tracer, not a promise. Settles §15 question 2.
2. **The seam law:** every action binding (`execute` and availability
   `check`) takes a structured `Invocation` whose arguments are
   `madc::value`s and resource handles, and returns a value-shaped
   result. No binding signature may accept or return anything a madc
   script could not. `ActionDefinition`'s C function pointers in §2.5 are
   the native binding kind behind this contract, not the model.
3. **Access model:** keys + levels per domain (owner-specified, hub doc).
   `PermissionSet`/`PolicyRegistry` here ARE that machinery;
   `Availability` checks evaluate the same conditions that gate
   projections. Credentials may derive from data (holding the brass-key
   entity confers the brass-key capability).
4. **Renderer dependency model:** the level-0 text renderer is internal
   to madc and dependency-free; curses and every richer renderer
   (levels 1+) are optional dat-style providers behind the core seams.
5. **Verbs are the serialization unit; the projection/diff stream is the
   wire** (hub doc demand 15). The frontend loop in §5 rides that
   contract: mutations flow through invocations, reads through
   projections over snapshots; the same semantic-diff protocol serves
   threads, processes, and sockets. Phase 1 is single-threaded; the
   contracts are concurrency-ready from day one.
6. **Prose pipeline** (hub doc, ratified): the projection library owns
   composition (authored prose / template composition / future NLG behind
   one seam); the level-0 renderer only typesets.

## 15. Open Design Questions

These questions can remain unresolved until the core examples are running.

1. Should `Actor` be a registered resource trait, a typed wrapper, or both?
2. ~~Should every action be a first-class resource, or should lightweight
   static action definitions also be permitted?~~ **Settled — see
   "Decisions Incorporated (2026-08-24)" item 1.**
3. How should action providers, executors, initiators, and causal chains be
   represented for automation and nested actions?
4. How are conflicting affordances merged when several providers expose the
   same action?
5. Which projection roles are truly primitive and which should be library
   compositions?
6. How much action parsing belongs in the generic line frontend versus an
   application-specific parser?
7. How should remote clients receive projection diffs and action schemas?
8. How should optimistic concurrency and stale affordances be reported?
9. Which portions of context should be persistent, session-local, or derived?
10. How should cached JIT variants be keyed when capabilities and user
    preferences are per connection?

## 16. Success Criteria

The abstraction is successful when all of the following are true:

1. The line-mode MUD and TUI text editor use the same core resource, action,
   context, affordance, invocation, and projection types.
2. The game cannot move a player from A to C when only A-to-B and B-to-C
   transitions are valid, regardless of frontend.
3. The editor can use direct key editing in a TUI and command/range editing in
   line mode without duplicating document mutation logic.
4. A menu is rendered as numbered choices in line mode and as an arrow-key
   selection in the TUI from the same `Choice` projection.
5. An unavailable action is consistently absent or disabled across all
   frontends and is still rejected if manually invoked.
6. Terminal cursor state never appears in game, shop, BBS, configuration, or
   document domain types.
7. A future GUI or web frontend can be added without changing the application
   action definitions.
8. An API or agent can inspect and invoke the same actions without pretending
   to click physical controls.

## Summary

The basic abstraction is:

```text
Resource
    something the application knows about

Actor
    a resource currently able to initiate actions

Action
    a semantic operation

Target
    the primary resource role affected by an invocation

State
    what is currently true

Context
    what is relevant to this actor and task now

Affordance
    an action currently available in that context

Invocation
    a concrete Actor -> Action(Target, Arguments) request

Projection
    the logical organization of information and affordances

Frontend
    the capability-aware presentation and input adapter
```

The application defines resources, state, actions, and rules.

The interaction layer derives context, affordances, and projections.

The frontend decides whether those semantics become line prompts, numbered
menus, a cursor-addressable TUI, native widgets, browser elements, voice,
graphics, or direct API operations.

This preserves the flexibility of an API-first resource model, the contextual
action resolution proven by MUD systems, and the capability-aware semantic
rendering model of the earlier universal rendering plan.
