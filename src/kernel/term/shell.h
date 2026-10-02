// silicon - the shell. One engine, three personalities: bash, zsh and fish.
#pragma once
#include "console.h"
#include "../fs/fs.h"

class Shell;

enum ShellRequest { SR_START_UI, SR_EXIT, SR_OPEN, SR_ZOOM, SR_WALLPAPER, SR_SETUP };

struct ShellHost {
    virtual ~ShellHost() {}
    virtual void shell_request(Shell* sh, ShellRequest r, const char* arg) = 0;
    virtual bool host_is_ui() = 0;
};

enum ShellFlavor { FL_BASH, FL_ZSH, FL_FISH };

struct ShellVar { char* name; char* value; bool exported; };

class Shell {
public:
    Console* con = nullptr;
    ShellHost* host = nullptr;
    FsNode* cwd = nullptr;
    ShellFlavor flavor = FL_BASH;
    int status = 0;                        // $? of the last command

    void init(Console* c, ShellHost* h);
    void banner();
    void start_prompt() { prompt(); }
    void on_key(const KeyEvent& e);
    void tick();
    void run_line(const char* line);       // execute a full command line (pipes, ;, &&, ||)
    bool busy() const { return prog != nullptr; }
    void out(const char* s);
    void outf(const char* fmt, ...);
    void err(const char* fmt, ...);        // error output: red, and sets status = 1
    void set_flavor(ShellFlavor f, bool greet);
    const char* flavor_name() const;

    // variables and aliases
    const char* get_var(const char* name);
    void set_var(const char* name, const char* value, bool exported = false);
    void unset_var(const char* name);
    const char* get_alias(const char* name);
    void set_alias(const char* name, const char* value);
    void unset_alias(const char* name);
    void source_file(FsNode* f);

    // pipeline stdin for the running command (nullptr when none)
    const char* stdin_text = nullptr;

    void start_program(class ConsoleProgram* p);
    bool prog_keep_screen = false;         // leave a finished program's output on screen

    Vec<char*> history;
    Vec<ShellVar> vars;
    Vec<ShellVar> aliases;
    Vec<ShellFlavor> flavor_stack;

private:
    char line[256];
    int  len = 0, cur = 0;
    i64  input_line = 0;
    int  prompt_len = 0;
    int  hist_idx = 0;
    ConsoleProgram* prog = nullptr;
    char* capture = nullptr;               // output capture (redirects and pipes)
    int  cap_len = 0, cap_cap = 0;
    // ctrl+r reverse search
    bool searching = false;
    char search[64];
    int  search_hit = -1;

    void prompt();
    void redraw();
    void complete();
    int  run_simple(char* text, const char* in, char** out_text);
    void exec(int argc, char** argv);
    void highlight(char* out, int n);      // colorized copy of the input line
    const char* suggestion();              // fish/zsh autosuggestion (rest of a history line) or nullptr
    void begin_capture();
    char* end_capture(int* n);
    bool expand_history(const char* in, char* out, int n);
    friend struct ShellCommands;
};

// console programs (term/programs.cpp)
ConsoleProgram* make_editor(Console* c, FsNode* file, const char* path);
ConsoleProgram* make_snake(Console* c);
ConsoleProgram* make_rain(Console* c);
ConsoleProgram* make_life(Console* c);
ConsoleProgram* make_sleep(Console* c, u32 ms);

// command table (term/shell_cmds.cpp)
typedef void (*CmdFn)(Shell& sh, int argc, char** argv);
struct Command { const char* name; const char* args; const char* help; CmdFn fn; };
const Command* find_command(const char* name);
extern const Command commands[];
extern const int command_count;
bool is_builtin_name(const char* name);
