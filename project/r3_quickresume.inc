// Optional Resistance 3 Quick Resume. No guest-memory patches or save writes.
// Included by r3_pcextra.cpp after the benchmark helpers.
static bool r3_quickresume_has_checkpoint()
{
    const char* root = getenv("PS3_GAMEDATA_ROOT");
    if (!root || !*root) root = "gamedata";
    const char* ids[] = { "BCUS98176", "BCES01118" };
    for (const char* id : ids) {
        char save[MAX_PATH], meta[MAX_PATH];
        snprintf(save, sizeof(save), "%s/dev_hdd0/home/00000001/savedata/%s_SAVE_0/GAME.SAV", root, id);
        snprintf(meta, sizeof(meta), "%s/dev_hdd0/home/00000001/savedata/%s_SAVE_0/PARAM.SFO", root, id);
        WIN32_FILE_ATTRIBUTE_DATA st;
        if (!GetFileAttributesExA(save, GetFileExInfoStandard, &st) ||
            (st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (!st.nFileSizeHigh && !st.nFileSizeLow)) continue;
        if (!GetFileAttributesExA(meta, GetFileExInfoStandard, &st) ||
            (st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (!st.nFileSizeHigh && !st.nFileSizeLow)) continue;
        return true;
    }
    return false;
}
static double r3_quickresume_process_age()
{
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    GetSystemTimeAsFileTime(&now);
    const uint64_t c = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return n >= c ? double(n-c) / 10000000.0 : 0;
}
static void r3_quickresume_inject(uint32_t button, const char* reason, double t)
{
    g_pad_inj_sticks = 0x80808080u;
    g_pad_inj_clear = 0;
    g_pad_inj_btn = button;
    g_pad_inj_until = GetTickCount64() + 180;
    LOG("quick resume: %s at %.1f s", reason, t);
}
static void r3_quickresume_tick(HWND game)
{
    // State -2 not initialized, -1 disabled, 0 dismiss boot prompt, 1 title,
    // 2 main menu, 3 campaign menu, 4 checkpoint load, 5 confirmed, 6 abort.
    static int state = -2;
    static int boots = 0, mode = 0; // 1: fast Campaign menu; 2: resume checkpoint
    static double last = 0, lobby_at = 0;
    if (state == -2) {
        if (s_bench) { state = -1; return; }
        const bool want_resume = envi("QUICK_RESUME", 0) != 0;
        const bool want_menu = envi("FAST_START", 0) != 0;
        if (want_resume && r3_quickresume_has_checkpoint()) mode = 2;
        else if (want_menu) mode = 1;
        else {
            if (want_resume) LOG("quick resume: no checkpoint; normal startup");
            state = -1;
            return;
        }
        state = 0;
        LOG("quick resume: startup mode %s enabled", mode == 2 ? "checkpoint" : "Campaign menu");
    }
    if (state < 0 || state >= 5) return;
    const double t = r3_quickresume_process_age();
    if (t > 900.0) { LOG("quick resume: timed out; normal menu remains available"); state = 6; return; }
    std::string level;
    {
        std::lock_guard<std::mutex> lock(s_lvl_mu);
        level = s_level;
    }
    if (level != "lobby" && state > 0) {
        if (state == 4 && !level.empty() && level != "frontend" && level != "mainmenu" && level.rfind("mp_", 0) != 0) {
            LOG("quick resume: gameplay level %s loaded at %.1f s", level.c_str(), t);
            state = 5;
            return;
        }
        if (state == 4 && t - last > 27.0) {
            LOG("quick resume: campaign load unconfirmed; stopping automation");
            state = 6;
        }
        return;
    }
    if (state == 0) {
        if (level == "lobby") {
            state = 1;
            lobby_at = t;
            LOG("quick resume: menu lobby detected at %.1f s", t);
            return;
        }
        char title[256] = {};
        static bool shader_notice = false;
        if (game && GetWindowTextA(game, title, int(sizeof title)) > 0 && strstr(title, "Compiling shaders")) {
            if (!shader_notice) { LOG("quick resume: waiting for shader compilation"); shader_notice = true; }
            return;
        }
        if (shader_notice) { LOG("quick resume: shader compilation finished at %.1f s", t); shader_notice = false; }
        if (t >= 22.0 && boots < 25 && (boots == 0 || t - last >= 2.0)) {
            r3_quickresume_inject(0x4000u, "dismiss startup prompt", t);
            last = t;
            boots++;
        }
        return;
    }
    if (state == 1 && t - lobby_at >= 1.5) {
        r3_quickresume_inject(0x0008u, "title to main menu", t);
        last = t;
        state = 2;
    } else if (state == 2 && t - last >= 3.0) {
        r3_quickresume_inject(0x4000u, "main menu to campaign", t);
        last = t;
        state = 3;
    } else if (state == 3 && t - last >= 3.0) {
        if (mode == 1) {
            LOG("quick resume: Campaign selection ready at %.1f s; no save loaded", t);
            state = 5;
            return;
        }
        r3_quickresume_inject(0x4000u, "continue saved campaign", t);
        last = t;
        state = 4;
    } else if (state == 4 && t - last >= 27.0) {
        LOG("quick resume: no gameplay confirmation; leaving menu available");
        state = 6;
    }
}
