// config.h - all user facing settings live here and in slideshow.ini
#pragma once
#include "dskv_pch.h"

namespace dskv {

enum class Fit { Cover = 0, Contain = 1, Stretch = 2 };

struct Config {
    // images
    std::wstring path;                 // folder with pictures ("" = Pictures known folder)
    std::wstring ini_path;             // set by -config to use another .ini
    bool    recursive       = true;
    std::wstring exts        = L".jpg;.jpeg;.png;.bmp;.gif;.tif;.tiff;.heic;.webp";
    bool    sort_desc       = false;
    bool    repeat          = true;   // false = stop after one pass through the folder
    bool    shuffle         = true;
    bool    exif_auto_rotate = false;  // set 1 if photos show sideways

    // timing
    int     interval_s      = 30;      // seconds per image
    bool    random_interval  = false;
    int     random_jitter   = 30;      // +/- percent
    int     transition_ms   = 900;     // crossfade duration
    unsigned cur_step_ms    = 0;       // resolved length of the current step

    // motion (Ken Burns)
    std::wstring anim        = L"zoom"; // none | zoom | pan
    double    anim_zoom      = 1.10;   // 1.0 - 2.0
    bool    anim_alt         = true;   // alternate in / out
    double    anim_fps       = 0;      // 0 = only while a transition runs

    // geometry / look
    bool    half_screen      = false;  // ignore saved rect, cover half of the work area
    int     left = 0, top = 0, width = 0, height = 0;
    bool    have_rect        = false;
    int     corner           = 12;
    int     shadow           = 18;
    int     border           = 1;
    int     border_a         = 26;     // 0-255
    std::wstring bg          = L"#0B0F14";
    int     min_side         = 160;    // skip tiny images
    Fit     fit              = Fit::Cover;

    // caption
    bool    caption          = true;
    std::wstring cap_fmt     = L"{name}  ·  {i}/{n}";
    int     cap_size         = 15;     // px
    std::wstring cap_font    = L"Segoe UI";
    int     cap_h            = 40;     // scrim height
    int     cap_a            = 235;     // text alpha
    bool    cap_hover        = false;  // only show while the pointer is over it

    // behaviour
    bool    click_through    = true;
    bool    auto_bottom      = true;
    bool    topmost          = false;  // opt-in: float above every window instead
    int     bottom_check_ms  = 1000;  // how often the "stay on the desktop" nudge runs
    bool    pause_fs         = true;
    bool    pause_on_lock    = true;
    bool    resume_after_pause = true;
    bool    auto_start       = false;
    bool    show_in_taskbar  = false;
    std::wstring hotkeys     = L"auto";  // auto | none
    bool    log              = false;

    // edit mode
    bool    edit             = false;
    int     edit_border_a    = 190;
    bool    show_grip        = true;
    int     grip_size        = 26;
};

} // namespace dskv
