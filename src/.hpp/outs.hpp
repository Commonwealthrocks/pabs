// outs.hpp
// last updated: 23/05/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
#pragma once
enum class clean_action
{
    None,
    Quick,
    Full
};
enum class abort_action
{
    None,
    Confirmed
};
clean_action render_dialogs();
abort_action cd_abort();
void cd_ok(const char *title, const char *message);
void cd_error(const char *title, const char *message);
void cd_clean_disc();
void __cd_abort(bool is_burn_op = true);
void cd_(); // m; Dude what is this?
            // c; idk but don't you fucking dare touch it
void _trigger_mk_dvd();
void _trigger_mk_cue();
void cd_creators();
void _trigger_about();
void cd_about();
void _trigger_keybinds();
void cd_keybinds();
void _trigger_quick_features();
void cd_quick_features();
bool is_cue_open_perhaps();
void cue_handle_drop(const char *path);
void parse_tooltips(const char *exe_dir);
void _tt_icon(const char *key, const char *popup_id);

// end