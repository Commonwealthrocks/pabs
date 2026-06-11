// outs.cpp
/// last updated: 12/06/2026
// win32; cmake -G "Ninja" ..
// win32; ninja
// m; You really need a better naming convention, outs?
#include "outs.hpp"
#include "sfx.hpp"
#include "imgui.h"
#include <string>
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <d3d11.h>
#include "config.hpp"
extern ID3D11ShaderResourceView *tex_about;
struct cue_track
{
    std::string file_path;
    int file_type = 0;  // c; 0 = BINARY, 1 = MOTOROLA, 2 = WAVE, 3 = MP3, 4 = AIFF
    int track_mode = 0; // c; 0 = MODE1/2352, 1 = AUDIO, 2 = MODE2/2352, etc
    std::string index0 = "";
    std::string index1 = "00:00:00";
    std::string title = "";
    std::string performer = "";
    std::string pregap = "";
    std::string postgap = "";
};

static bool cue_dialog = false;
static bool dvd_dialog = false;
static std::vector<cue_track> cue_tracks;
static char cue_title[128] = "";
static char cue_perf[128] = "";
static char dvd_iso_path[MAX_PATH] = "";
static char dvd_layer_break[32] = "1913760";
static std::string cue_error = "";
static std::string dvd_error = "";
static bool cue_origin_paths = false;
static bool cue_is_active = false;
static bool dvd_is_active = false;

static std::unordered_map<std::string, std::string> s_tooltips;
void parse_tooltips(const char *exe_dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sassets\\txt_data\\tooltips", exe_dir);
    std::ifstream f(path);
    if (!f.is_open())
        return;
    std::string line;
    std::string current_key;
    std::string current_value;
    bool in_block = false;
    while (std::getline(f, line))
    {
        if (in_block)
        {
            if (line == "\"\"\"")
            {
                if (!current_value.empty() && current_value.back() == '\n')
                    current_value.pop_back();
                s_tooltips[current_key] = current_value;
                in_block = false;
            }
            else
            {
                current_value += line + "\n";
            }
        }
        else
        {
            size_t eq = line.find("=\"\"\"");
            if (eq != std::string::npos)
            {
                current_key = line.substr(0, eq);
                current_value = "";
                in_block = true;
            }
            else
            {
                size_t simple_eq = line.find('=');
                if (simple_eq != std::string::npos)
                {
                    std::string k = line.substr(0, simple_eq);
                    std::string v = line.substr(simple_eq + 1);
                    size_t pos = 0;
                    while ((pos = v.find("\\n", pos)) != std::string::npos)
                    {
                        v.replace(pos, 2, "\n");
                        pos += 1;
                    }
                    s_tooltips[k] = v;
                }
            }
        }
    }
}
void _tt_icon(const char *key, const char *popup_id)
{
    ImGui::SameLine();
    ImGui::PushID(popup_id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##tt_btn", ImVec2(16, 16));
    bool hovered = ImGui::IsItemHovered();
    if (tex_about)
    {
        ImU32 col = hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 140, 140, 255);
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)tex_about, ImVec2(p.x, p.y + 2), ImVec2(p.x + 16, p.y + 18), ImVec2(0, 0), ImVec2(1, 1), col);
    }
    if (clicked)
    {
        sfx::info_sfx();
        ImGui::OpenPopup(popup_id);
    }
    ImGuiViewport *mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mv->WorkPos.x + mv->WorkSize.x * 0.5f, mv->WorkPos.y + mv->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(popup_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextDisabled("Information");
        ImGui::Separator();
        ImGui::Spacing();
        auto it = s_tooltips.find(key);
        if (it != s_tooltips.end())
            ImGui::TextUnformatted(it->second.c_str());
        else
            ImGui::TextUnformatted("Tooltip not found!!!");
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(3) || ImGui::IsMouseClicked(4))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static bool is_valid_index(const std::string &idx)
{
    if (idx.empty())
        return true;
    int m, s, f;
    if (sscanf(idx.c_str(), "%d:%d:%d", &m, &s, &f) != 3)
        return false;
    if (s >= 60 || f >= 75 || m < 0 || s < 0 || f < 0)
        return false;
    return true;
}

static bool show_ok = false;
static bool show_clean = false;
static bool show_abort = false;
static bool show_abort_burn = false;
static std::string ok_title = "";
static std::string ok_msg = "";
void cd_ok(const char *title, const char *message)
{
    ok_title = title;
    ok_msg = message;
    show_ok = true;
    sfx::info_sfx(); // m; Why do you use two backslahes?
                     // c; cause... the language works like that?
                     // m; Next.JS fixes that btw!
                     // c; SHUT THE FUCK UP
}
void cd_error(const char *title, const char *message)
{
    ok_title = title;
    ok_msg = message;
    show_ok = true;
    if (!_config.mute_error_sfx)
    {
        sfx::error_sfx();
    }
}
void cd_clean_disc()
{
    sfx::info_sfx();
    show_clean = true;
}
void __cd_abort(bool is_burn_op)
{
    sfx::info_sfx();
    show_abort = true;
    show_abort_burn = is_burn_op;
}
clean_action render_dialogs()
{
    clean_action action = clean_action::None;
    if (show_ok)
    {
        ImGui::OpenPopup(ok_title.c_str());
        show_ok = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(ok_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s", ok_msg.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImVec2 button_size(120, 0);
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_size.x) * 0.5f);
        if (ImGui::Button("OK", button_size))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (show_clean)
    {
        ImGui::OpenPopup("Clean disc##clean_dialog");
        show_clean = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Clean disc##clean_dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Do you want to clean the current disc?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImVec2 btn_size(100, 0);
        if (ImGui::Button("Quick clean", btn_size))
        {
            ImGui::CloseCurrentPopup();
            action = clean_action::Quick;
        }
        ImGui::SameLine();
        if (ImGui::Button("Full clean", btn_size))
        {
            ImGui::CloseCurrentPopup();
            action = clean_action::Full;
        }
        ImGui::SameLine();
        if (ImGui::Button("Abort", btn_size))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return action;
}
abort_action cd_abort()
{
    abort_action action = abort_action::None;
    if (show_abort)
    {
        ImGui::OpenPopup("Abort operation?##abort_dialog"); // c; mix case here, should work either way
        show_abort = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Abort operation?##abort_dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (show_abort_burn)
            ImGui::Text("Are you sure you want to abort the current disc operation?\nDoing this while burning data on non -RW discs will\nmake them unsuitable for burning again.");
        else
            ImGui::Text("Are you sure you want to abort the current disc operation?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImVec2 btn_size(80, 0);
        if (ImGui::Button("Yeah", btn_size))
        {
            ImGui::CloseCurrentPopup();
            action = abort_action::Confirmed;
        }
        ImGui::SameLine();
        if (ImGui::Button("Nah", btn_size))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return action;
}
void _trigger_mk_cue()
{
    sfx::info_sfx();
    cue_dialog = true;
    cue_tracks.clear();
    cue_error.clear();
}
void _trigger_mk_dvd()
{
    sfx::info_sfx();
    dvd_dialog = true;
    dvd_iso_path[0] = '\0';
    dvd_error.clear();
}
bool is_cue_open_perhaps()
{
    return cue_is_active;
}
void cue_handle_drop(const char *path)
{
    cue_track t;
    t.file_path = path;
    t.file_type = 2;
    t.track_mode = 1;
    t.index1 = "00:00:00";
    t.performer = cue_perf;
    cue_tracks.push_back(t);
    cue_error.clear();
}
void cd_creators() // m; In reality Common doesn't use these functions one bit,
{                  // >> so I'm adding them more for myself.
    if (cue_dialog)
    {
        cue_is_active = true;
        cue_dialog = false;
    }
    if (cue_is_active)
    {
        ImGuiWindowClass window_class;
        window_class.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&window_class);
        if (ImGui::Begin("Create CUE file", &cue_is_active, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("TITLE", cue_title, sizeof(cue_title)); // c; all of the metadata for most burners has to be fully uppercase
            ImGui::SameLine();                                       // >> https://wiki.hydrogenaudio.org/index.php?title=Cue_sheet
            ImGui::SetNextItemWidth(200);                            // m; Oh, OK.
            ImGui::InputText("PERFORMER", cue_perf, sizeof(cue_perf));
            ImGui::Text("Tracks:");
            ImGui::BeginChild("##cue_tracks", ImVec2(730, 300), true);
            for (size_t i = 0; i < cue_tracks.size(); ++i)
            {
                ImGui::PushID((int)i);

                ImGui::Button("=");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Drag to reorder");
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                {
                    ImGui::SetDragDropPayload("CUE_TRACK", &i, sizeof(size_t));
                    ImGui::Text("Move track %02zu", i + 1);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("CUE_TRACK"))
                    {
                        size_t source_i = *(const size_t *)payload->Data;
                        if (source_i != i)
                        {
                            cue_track temp = cue_tracks[source_i];
                            cue_tracks.erase(cue_tracks.begin() + source_i);
                            cue_tracks.insert(cue_tracks.begin() + i, temp);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::SameLine();
                ImGui::Text("Track %02zu", i + 1);
                ImGui::SameLine();

                const char *path_cstr = cue_tracks[i].file_path.c_str();
                const char *filename = strrchr(path_cstr, '\\');
                if (!filename)
                    filename = strrchr(path_cstr, '/');
                filename = filename ? filename + 1 : path_cstr;

                char buf[MAX_PATH];
                strncpy(buf, filename, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                ImGui::SetNextItemWidth(250);
                ImGui::InputText("##filepath", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
                if (ImGui::IsItemHovered() && ImGui::GetIO().KeyShift)
                    ImGui::SetTooltip("%s", cue_tracks[i].file_path.c_str());

                ImGui::SameLine();
                if (ImGui::Button("Browse..."))
                {
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    char fname[MAX_PATH] = "";
                    ofn.lpstrFile = fname;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = "All files\0*.*\0";
                    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                    const char *init = _config.last_browse_dir.empty() ? nullptr : _config.last_browse_dir.c_str();
                    ofn.lpstrInitialDir = init;
                    if (GetOpenFileNameA(&ofn))
                    {
                        cue_tracks[i].file_path = fname;
                        char dir_buf[MAX_PATH];
                        snprintf(dir_buf, sizeof(dir_buf), "%s", fname);
                        char *slash = strrchr(dir_buf, '\\');
                        if (slash)
                            *slash = '\0';
                        _config.last_browse_dir = dir_buf;
                    }
                }
                ImGui::SetNextItemWidth(100);
                const char *file_types[] = {"BINARY", "MOTOROLA", "WAVE", "MP3", "AIFF"};
                ImGui::Combo("File type", &cue_tracks[i].file_type, file_types, IM_ARRAYSIZE(file_types));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("BINARY: standard raw disc image.\nWAVE: uncompressed audio data.\nMP3 / AIFF: supported by some burners, but WAVE / BINARY is recommended.");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120);
                const char *track_modes[] = {"MODE1/2352", "AUDIO", "MODE2/2352", "MODE1/2048", "MODE2/2048", "MODE2/2336", "CDI/2336", "CDI/2352"};
                if (ImGui::Combo("Track mode", &cue_tracks[i].track_mode, track_modes, IM_ARRAYSIZE(track_modes)))
                {
                    cue_tracks[i].index0 = "";
                    cue_tracks[i].index1 = "00:00:00";
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("MODE1/2352: standard CD-ROM data.\nAUDIO: standard Audio CD track.\nMODE2/2352: CD-ROM XA data.");
                char t_buf[128];
                strncpy(t_buf, cue_tracks[i].title.c_str(), sizeof(t_buf) - 1);
                t_buf[sizeof(t_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputText("Title", t_buf, sizeof(t_buf)))
                    cue_tracks[i].title = t_buf;
                ImGui::SameLine();
                char p_buf[128];
                strncpy(p_buf, cue_tracks[i].performer.c_str(), sizeof(p_buf) - 1);
                p_buf[sizeof(p_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputText("Performer", p_buf, sizeof(p_buf)))
                    cue_tracks[i].performer = p_buf;
                char pg_buf[32];
                strncpy(pg_buf, cue_tracks[i].pregap.c_str(), sizeof(pg_buf) - 1);
                pg_buf[sizeof(pg_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputText("PREGAP", pg_buf, sizeof(pg_buf)))
                    cue_tracks[i].pregap = pg_buf;
                ImGui::SameLine();
                char pog_buf[32];
                strncpy(pog_buf, cue_tracks[i].postgap.c_str(), sizeof(pog_buf) - 1);
                pog_buf[sizeof(pog_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputText("POSTGAP", pog_buf, sizeof(pog_buf)))
                    cue_tracks[i].postgap = pog_buf;
                char idx0_buf[32];
                strncpy(idx0_buf, cue_tracks[i].index0.c_str(), sizeof(idx0_buf) - 1);
                idx0_buf[sizeof(idx0_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputText("INDEX 00", idx0_buf, sizeof(idx0_buf)))
                    cue_tracks[i].index0 = idx0_buf;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("INDEX 00 specifies the start of the pregap (silence) before the track.\nFormat is MM:SS:FF (Frames are 0-74).");
                ImGui::SameLine();
                char idx1_buf[32];
                strncpy(idx1_buf, cue_tracks[i].index1.c_str(), sizeof(idx1_buf) - 1);
                idx1_buf[sizeof(idx1_buf) - 1] = '\0';
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputText("INDEX 01", idx1_buf, sizeof(idx1_buf)))
                    cue_tracks[i].index1 = idx1_buf;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("INDEX 01 specifies the actual start of the track data.\nFormat is MM:SS:FF (frames are 0-74).");
                if (ImGui::Button("Remove track"))
                {
                    cue_tracks.erase(cue_tracks.begin() + i);
                    ImGui::PopID();
                    break;
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (ImGui::Button("Add track"))
            {
                cue_track t;
                t.performer = cue_perf;
                cue_tracks.push_back(t);
                cue_error.clear();
            }
            ImGui::Spacing();
            ImGui::Checkbox("Write absolute origin paths to CUE", &cue_origin_paths);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("How PABS handles provided tracks.\n\nGiven this option is ticked, PABS will append the absolute PATHS\nof selected files to the CUE file. Else; the CUE file will assume\nthat the tracks live in the same directory as the CUE file.");
            ImGui::Separator();
            ImGui::Spacing();
            if (!cue_error.empty())
            {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", cue_error.c_str());
                ImGui::Spacing();
            }
            if (ImGui::Button("Save CUE", ImVec2(100, 0)))
            {
                cue_error.clear();
                bool valid = true;
                if (cue_tracks.empty())
                {
                    cue_error = "Cannot save an empty CUE sheet; add at least one track.";
                    valid = false;
                }
                for (auto &t : cue_tracks)
                {
                    if (t.file_path.empty())
                    {
                        cue_error = "One or more tracks have an empty file path.";
                        valid = false;
                        break;
                    }
                    if (!is_valid_index(t.index0) || !is_valid_index(t.index1) || !is_valid_index(t.pregap) || !is_valid_index(t.postgap))
                    {
                        cue_error = "Invalid time format (MM:SS:FF); seconds < 60, frames < 75.";
                        valid = false;
                        break;
                    }
                }
                if (valid)
                {
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    char fname[MAX_PATH] = "";
                    ofn.lpstrFile = fname;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = "CUE files\0*.cue\0";
                    ofn.lpstrDefExt = "cue";
                    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                    if (GetSaveFileNameA(&ofn))
                    {
                        std::ofstream f(fname);
                        if (cue_title[0])
                            f << "TITLE \"" << cue_title << "\"\n";
                        if (cue_perf[0])
                            f << "PERFORMER \"" << cue_perf << "\"\n";

                        for (size_t i = 0; i < cue_tracks.size(); ++i)
                        {
                            const char *ft = "BINARY";
                            if (cue_tracks[i].file_type == 1)
                                ft = "MOTOROLA";
                            else if (cue_tracks[i].file_type == 2)
                                ft = "WAVE";
                            else if (cue_tracks[i].file_type == 3)
                                ft = "MP3";
                            else if (cue_tracks[i].file_type == 4)
                                ft = "AIFF";
                            const char *tm = "MODE1/2352";
                            if (cue_tracks[i].track_mode == 1)
                                tm = "AUDIO";
                            else if (cue_tracks[i].track_mode == 2)
                                tm = "MODE2/2352"; // m; Some burners won't accept names like MODE2 / 2352
                            else if (cue_tracks[i].track_mode == 3)
                                tm = "MODE1/2048"; // >> so do NOT add gaps between the blokes.
                            else if (cue_tracks[i].track_mode == 4)
                                tm = "MODE2/2048"; // c; might bother me, but sure
                            else if (cue_tracks[i].track_mode == 5)
                                tm = "MODE2/2336";
                            else if (cue_tracks[i].track_mode == 6)
                                tm = "CDI/2336";
                            else if (cue_tracks[i].track_mode == 7)
                                tm = "CDI/2352";

                            const char *base_name = cue_tracks[i].file_path.c_str();
                            if (!cue_origin_paths)
                            {
                                const char *slash = strrchr(base_name, '\\');
                                if (slash)
                                    base_name = slash + 1;
                                else
                                {
                                    slash = strrchr(base_name, '/');
                                    if (slash)
                                        base_name = slash + 1;
                                }
                            }
                            if (*base_name == '\0')
                                base_name = cue_tracks[i].file_path.c_str();
                            f << "FILE \"" << base_name << "\" " << ft << "\n";
                            f << "  TRACK " << ((i + 1) < 10 ? "0" : "") << (i + 1) << " " << tm << "\n";
                            if (!cue_tracks[i].title.empty())
                                f << "    TITLE \"" << cue_tracks[i].title << "\"\n";
                            if (!cue_tracks[i].performer.empty())
                                f << "    PERFORMER \"" << cue_tracks[i].performer << "\"\n";
                            if (!cue_tracks[i].pregap.empty())
                                f << "    PREGAP " << cue_tracks[i].pregap << "\n";
                            if (!cue_tracks[i].index0.empty())
                                f << "    INDEX 00 " << cue_tracks[i].index0 << "\n";
                            if (!cue_tracks[i].index1.empty())
                                f << "    INDEX 01 " << cue_tracks[i].index1 << "\n";
                            if (!cue_tracks[i].postgap.empty())
                                f << "    POSTGAP " << cue_tracks[i].postgap << "\n";
                        }
                        cue_is_active = false;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0)))
            {
                cue_is_active = false;
            }
        }
        ImGui::End();
    }
    if (dvd_dialog)
    {
        dvd_is_active = true;
        dvd_dialog = false;
    }
    if (dvd_is_active)
    {
        ImGuiWindowClass window_class;
        window_class.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&window_class);
        if (ImGui::Begin("Create DVD file", &dvd_is_active, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("ISO path:");
            ImGui::SetNextItemWidth(300);
            ImGui::InputText("##iso_path", dvd_iso_path, sizeof(dvd_iso_path));
            ImGui::SameLine();
            if (ImGui::Button("Browse..."))
            {
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                char fname[MAX_PATH] = "";
                ofn.lpstrFile = fname;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = "ISO files\0*.iso;*.img\0All files\0*.*\0";
                ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                if (GetOpenFileNameA(&ofn))
                {
                    snprintf(dvd_iso_path, sizeof(dvd_iso_path), "%s", fname);
                }
            }
            ImGui::Text("Layer break LBA:");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Make sure you only set a layer break for Dual-Layer (DVD+R DL) ISOs!!!\nSetting this for a Single-Layer (DVD-R/DVD+R) image will cause burn failures.");
            ImGui::SetNextItemWidth(150);
            ImGui::InputText("##layer_break", dvd_layer_break, sizeof(dvd_layer_break));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (!dvd_error.empty())
            {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", dvd_error.c_str());
                ImGui::Spacing();
            }
            if (ImGui::Button("Save DVD", ImVec2(100, 0)))
            {
                dvd_error.clear();
                if (strlen(dvd_iso_path) == 0)
                {
                    dvd_error = "Please browse or enter a path to an ISO file.";
                }
                else
                {
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    char fname[MAX_PATH] = "";
                    if (strlen(dvd_iso_path) > 0)
                    {
                        snprintf(fname, sizeof(fname), "%s", dvd_iso_path);
                        char *ext = strrchr(fname, '.');
                        if (ext)
                            strcpy(ext, ".dvd");
                        else
                            strcat(fname, ".dvd");
                    }
                    ofn.lpstrFile = fname;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = "DVD files\0*.dvd\0";
                    ofn.lpstrDefExt = "dvd";
                    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                    if (GetSaveFileNameA(&ofn))
                    {
                        std::ofstream f(fname);
                        f << "MediaType=DVD\n";
                        f << "LayerBreak=" << dvd_layer_break << "\n";
                        const char *base_name = strrchr(dvd_iso_path, '\\');
                        if (base_name)
                            base_name++;
                        else
                            base_name = dvd_iso_path;
                        f << "ISO=" << base_name << "\n";
                        dvd_is_active = false;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0)))
            {
                dvd_is_active = false;
            }
        }
        ImGui::End();
    }
}
static bool show_about = false;
void _trigger_about()
{
    sfx::info_sfx();
    show_about = true;
}
void cd_about()
{
    if (show_about)
    {
        ImGui::OpenPopup("About PABS##about_modal");
        show_about = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About PABS##about_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.75f, 1.0f), "PABS -");
        ImGui::SameLine();
        ImGui::TextDisabled("PYROFOREVER's actual burning software");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Version:  v0.3a");
        ImGui::Text("Build:    " __DATE__ "  " __TIME__);
        ImGui::Spacing();
        ImGui::TextDisabled("Made by Common n' Mike.");
        ImGui::TextDisabled("Your (least) fav devs!");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("LIGHTNING UK! might... no...\nWILL hate this!");
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 120) * 0.5f);
        if (ImGui::Button("Fair enough", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
static bool show_keybinds = false;
void _trigger_keybinds()
{
    sfx::info_sfx();
    show_keybinds = true;
}
void cd_keybinds()
{
    if (show_keybinds)
    {
        ImGui::OpenPopup("Keybinds##keybinds_modal");
        show_keybinds = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Keybinds##keybinds_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("Global shortcuts:");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + S");
        ImGui::SameLine(180);
        ImGui::Text("-> settings tab");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + W");
        ImGui::SameLine(180);
        ImGui::Text("-> ISO to disc mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + R");
        ImGui::SameLine(180);
        ImGui::Text("-> disc to ISO mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + D");
        ImGui::SameLine(180);
        ImGui::Text("-> files to disc mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + I");
        ImGui::SameLine(180);
        ImGui::Text("-> files to ISO mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + C");
        ImGui::SameLine(180);
        ImGui::Text("-> audio CD mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + V");
        ImGui::SameLine(180);
        ImGui::Text("-> verify disc mode");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + A");
        ImGui::SameLine(180);
        ImGui::Text("-> are you?");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + Q");
        ImGui::SameLine(180);
        ImGui::Text("-> create CUE file");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + SHIFT + P");
        ImGui::SameLine(180);
        ImGui::Text("-> create DVD file");
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "ESC or MOUSE4");
        ImGui::SameLine(180);
        ImGui::Text("-> return bind");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "CTRL + A");
        ImGui::SameLine(180);
        ImGui::Text("-> select all items (in lists)");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 120) * 0.5f);
        if (ImGui::Button("Got it", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
static bool show_quick_features = false;
void _trigger_quick_features()
{
    sfx::info_sfx();
    show_quick_features = true;
}
void cd_quick_features()
{
    if (show_quick_features)
    {
        ImGui::OpenPopup("Quick features##quick_features_modal");
        show_quick_features = false;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x * 0.5f, ImGui::GetMainViewport()->WorkPos.y + ImGui::GetMainViewport()->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quick features##quick_features_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::Text("Quick features:");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Settings are volatile in the modes and only truly apply in the settings tab itself.\nIn the modes they revert to normal when discarding.");
        ImGui::Spacing();
        ImGui::Text("SHIFT + Clear in the log window will clear the log file itself too.");
        ImGui::Spacing();
        ImGui::Text("In the settings tab, you can read the tooltips about most of them;\nwhich you should, OK?");
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 120) * 0.5f);
        if (ImGui::Button("Got it", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// end