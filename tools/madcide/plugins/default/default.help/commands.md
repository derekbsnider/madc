# Command descriptions

What each command does, by its registry name: the `menus` help topic
shows these beside every menu item.

## new
Opens a new, untitled buffer.

## editfile
Opens a file into a buffer of its own, or switches to it when it is open.

## recent
Lists the files you opened or saved most recently, newest first, and opens
the one you choose.

## save
Saves the buffer to its file, then checks it.

## saveas
Saves the buffer under a new name.

## saveall
Saves every modified buffer.

## close
Closes the buffer, asking first when it has unsaved changes.

## closeall
Closes every buffer, asking about each one with unsaved changes.

## savequit
Saves the buffer and leaves the editor.

## quit
Leaves the editor, asking first about unsaved changes.

## undo
Undoes the last edit.

## redo
Redoes the edit Undo took back.

## cut
Moves the selection to the clipboard.

## copy
Copies the selection to the clipboard.

## paste
Inserts the clipboard at the cursor.

## selectall
Selects the whole buffer.

## find
Searches the buffer for text.

## findnext
Finds the next match of the last search.

## replace
Replaces text, one match at a time or all of them.

## gotoline
Moves the cursor to a line by number.

## indent
Indents the selected lines one step.

## dedent
Removes one step of indentation from the selected lines.

## togglecomment
Comments the selected lines out, or back in.

## autoindent
Turns autoindent on or off. While it is on, Enter starts the new line
with the current line's indentation, one step deeper after a line that
opens a block.

## mark
Marks where a block starts.

## bend
Marks where a block ends.

## blockcopy
Copies the marked block to the cursor.

## blockmove
Moves the marked block to the cursor.

## blockdel
Deletes the marked block.

## delline
Deletes the line the cursor is on.

## delword
Deletes the word to the right of the cursor.

## insertfile
Inserts a file's text at the cursor.

## outline
Shows the definitions in the buffer; choosing one moves the cursor to it.

## mdpreview
Shows the Markdown buffer being edited as it reads, beside it, and keeps it current as the buffer changes; choosing a line moves the cursor to its source, and choosing a link opens the file it names.

## problems
Shows the problems the last check found; choosing one moves the cursor to it.

## output
Shows the messages of the last build.

## terminal
Shows the terminal, where a program started by Build ▸ Run runs.

## shell
Opens a command shell in the terminal.

## repl
Shows the REPL (a Read–eval–print loop), where code is typed and run one
entry at a time.

## replrun
Runs the buffer's program in the REPL: a fresh session, the buffer
loaded, then its main.

## replstop
Stops what the REPL is running.

## replinterrupt
Interrupts what the REPL is running: it returns to the prompt at its next
loop, and the names you defined stay. Copy with nothing selected in the
REPL's input does the same, as Ctrl+C does in a terminal. A program waiting
inside a library call cannot stop there: interrupt it again, and the REPL
starts a fresh session.

## repleof
Ends the standard input of what the REPL is running, as Ctrl+D does in a
terminal: what the input line holds is sent first, then the program's next
read sees the end of its input. What you run after it reads new input.

## replclear
Clears the REPL's transcript.

## build
Lists the ways to check, build and run the buffer or the project.

## language
Chooses the language standard programs are compiled as.

## progargs
Sets the arguments a run passes to the program.

## project
Lists the project's files.

## openproject
Opens a project file.

## projaddcur
Adds the buffer's file to the project.

## keystyle
Chooses the key style: which keys run which commands.

## options
Shows the editor's options, such as the tab width and the theme.

## modes
Shows the editor's modes.

## view
Shows the program as the compiler sees it: its tree, its C, its assembly.
On a Markdown file, shows the text as it reads, with the formatting
characters hidden; editing there changes the file.

## panel
Shows or hides the panel below the editor.

## refresh
Redraws the screen.

## fontlarger
Makes the window's text one size larger. The size is remembered for the next session. A terminal keeps its own font.

## fontsmaller
Makes the window's text one size smaller.

## fullscreen
Puts the window in full screen, or takes it out. A terminal keeps its own window.

## splitw
Splits the window in two.

## nextw
Moves to the next window.

## prevw
Moves to the previous window.

## groww
Makes the window taller.

## shrinkw
Makes the window shorter.

## killwin
Closes the window; its buffer stays open.

## onlywin
Closes every window but this one.

## gitchanges
Shows how the buffer differs from its file's last commit, unsaved edits
included.

## githistory
Lists the commits that changed the file; choosing one shows the file as
that commit holds it.

## gitblame
Says which commit last changed the cursor's line, and who made it.

## helpcontents
Opens the help at its contents.

## helptopic
Opens a help topic by name.

## help
Lists the key style's bindings.

## about
Shows the program's version and where it came from.
