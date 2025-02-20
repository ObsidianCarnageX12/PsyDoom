#include "d_main.h"

#include "Base/d_vsprintf.h"
#include "Base/i_drawcmds.h"
#include "Base/i_file.h"
#include "Base/i_main.h"
#include "Base/i_misc.h"
#include "Base/s_sound.h"
#include "Base/w_wad.h"
#include "Base/z_zone.h"
#include "cdmaptbl.h"
#include "FatalErrors.h"
#include "FileUtils.h"
#include "Finally.h"
#include "Game/g_game.h"
#include "Game/p_info.h"
#include "Game/p_spec.h"
#include "Game/p_switch.h"
#include "Game/p_tick.h"
#include "Game/sprinfo.h"
#include "PsyDoom/Config/Config.h"
#include "PsyDoom/DemoPlayer.h"
#include "PsyDoom/DemoRecorder.h"
#include "PsyDoom/Game.h"
#include "PsyDoom/GameConstants.h"
#include "PsyDoom/Input.h"
#include "PsyDoom/IntroLogos.h"
#include "PsyDoom/MapInfo/MapInfo.h"
#include "PsyDoom/Movie/MoviePlayer.h"
#include "PsyDoom/PlayerPrefs.h"
#include "PsyDoom/ProgArgs.h"
#include "PsyDoom/PsxPadButtons.h"
#include "PsyDoom/Utils.h"
#include "PsyDoom/Video.h"
#include "PsyQ/LIBGPU.h"
#include "Renderer/r_data.h"
#include "Renderer/r_main.h"
#include "UI/cr_main.h"
#include "UI/le_main.h"
#include "UI/m_main.h"
#include "UI/o_main.h"
#include "UI/st_main.h"
#include "UI/ti_main.h"

#if PSYDOOM_VULKAN_RENDERER
    #include "PsyDoom/Vulkan/VRenderer.h"
#endif

#include <chrono>
#include <cstdio>
#include <memory>

#if PSYDOOM_MODS
    // PsyDoom: how frequently (in seconds) to update the performance counters that track the average frame time
    static constexpr float PERF_COUNTER_FREQ = 0.25f;
#endif

// The current number of 1 vblank ticks
int32_t gTicCon;

// The number of elapsed vblanks for all players
int32_t gPlayersElapsedVBlanks[MAXPLAYERS];

// PsyDoom: networking - what amount of elapsed vblanks we told the other player we will simulate next
#if PSYDOOM_MODS
    int32_t gNextPlayerElapsedVBlanks;
#endif

// Pointer to a buffer holding the demo and the current pointer within the buffer for playback/recording
std::byte*  gpDemoBuffer;
std::byte*  gpDemo_p;

#if PSYDOOM_MODS
    // PsyDoom: info about the current classic demo being played (what game mode to use etc.)
    ClassicDemoDef gCurClassicDemo;
#endif

// Game start parameters
skill_t     gStartSkill         = sk_medium;
int32_t     gStartMapOrEpisode  = 1;
gametype_t  gStartGameType      = gt_single;

// Net games: set if a network game being started was aborted
bool gbDidAbortGame = false;

#if PSYDOOM_MODS
    bool        gbStartupWarpToMap = false;     // PsyDoom: warp straight to a map and bypass menus on starting a new game? (map development tool)
    double      gPrevFrameDuration;             // How long the previous frame took: used to try and provide more accurate interpolation
    float       gPerfAvgFps;                    // Performance counter: averaged FPS for the last few frames
    float       gPerfAvgUsec;                   // Performance counter: averaged microseconds duration for the last few frames
    bool        gbIsFirstTick;                  // Set to 'true' for the very first tick only, 'false' thereafter
    bool        gbKeepInputEvents;              // Ticker request: if true then don't consume input events after invoking the current ticker in 'MiniLoop'
    std::byte*  gpDemoBufferEnd;                // PsyDoom: save the end pointer for the buffer, so we know when to end the demo; do this instead of hardcoding the end
    bool        gbDoInPlaceLevelReload;         // PsyDoom developer feature: reload the map but preserve player position and orientation? Allows for fast preview of changes.
    fixed_t     gInPlaceReloadPlayerX;          // Where to position the player after doing the 'in place' level reload (x)
    fixed_t     gInPlaceReloadPlayerY;          // Where to position the player after doing the 'in place' level reload (y)
    fixed_t     gInPlaceReloadPlayerZ;          // Where to position the player after doing the 'in place' level reload (z)
    angle_t     gInPlaceReloadPlayerAng;        // Angle of the player when doing an 'in place' level releoad

    // When using PAL timings and NOT using demo timings this tells how many vblanks the current game/world tick will last for.
    // If 'true' then the current world tick will last for 4 vblanks, otherwise it will last for 2 vblanks.
    // 
    // For PAL timings (without demo timings) the world tick duration varies because a world tick only fires when a player tick fires, which is
    // every 2 vblanks. The world tick is INTENDED to trigger every 3 vblanks, but since it is tied to player ticks then the intervals between
    // world ticks must be a multiple of the player tick interval (2 vblanks). Depending on timing, this sometimes means that world ticks last
    // for 4 vblanks and sometimes just 2 vblanks. In this complex timing scenario world ticks should also normally switch between 4 and 2
    // vblanks duration on each alternate frame, yielding a running average of ~3 vblanks duration...
    //
    // This variable is basically used to try and smooth out interpolation for the PAL (non demo-timing) case as much as possible.
    // It's not possible to achieve totally smooth motion in this scenario because the interval between frames is constantly changing, which
    // makes the animation speed seem inconsistent. At least it's an improvement in the right direction however, and the best we can do for
    // this very complex scenario.
    //
    // Note also that we DON'T have to make this long vs short tick interpolation adjustment when we are using demo timings with PAL since
    // player ticks are perfectly synchronized (they fire at the same time) as world ticks in that situation.
    bool gbIsLongGameTick;
#endif

// Debug draw string position
static int32_t gDebugDrawStringXPos;
static int32_t gDebugDrawStringYPos;

#if PSYDOOM_MODS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom: play the intro movie and logos.
// These were originally done outside of 'PSXDOOM.EXE' in the main launcher executable.
// For PsyDoom however this logic needs to reside all within the same executable.
//------------------------------------------------------------------------------------------------------------------------------------------
static void D_PlayIntros() noexcept {
    // Show the Sony intro logo
    LogoPlayer::play(IntroLogos::getSonyLogo());

    if (Input::isQuitRequested())
        return;

    // Play the intro movies (just the Williams logo for 'Doom' and 'Final Doom')
    for (const String32& moviePath : Game::gConstants.introMovies) {
        // The list of movies is terminated by a blank path
        if (moviePath.length() <= 0)
            break;

        // Play the movie and quit afterwards if the game is shutting down:
        const float movieFps = (Game::gGameVariant == GameVariant::PAL) ? 25.0f : 30.0f;
        movie::MoviePlayer::play(moviePath.c_str().data(), movieFps);

        if (Input::isQuitRequested())
            return;
    }

    // Show the legal intro logos, if available for this game disc.
    // Note: if it is a demo version of 'Doom' and legal logos are not available then emulate the demo behavior and show the special demo-only 'legals' UI.
    IntroLogos::LogoList introLogos = IntroLogos::getLegalLogos();
    bool bDidShowLegals = false;
    
    for (const LogoPlayer::Logo& logo : introLogos.logos) {
        if (logo.pPixels && LogoPlayer::play(logo)) {
            bDidShowLegals = true;
        }

        if (Input::isQuitRequested())
            return;
    }

    if ((!bDidShowLegals) && Game::gbIsDemoVersion) {
        MiniLoop(START_Legals, STOP_Legals, TIC_Legals, DRAW_Legals);
    }
}
#endif  // #if PSYDOOM_MODS

//------------------------------------------------------------------------------------------------------------------------------------------
// Main DOOM entry point.
// Bootstraps the engine and platform specific code and runs the game loops.
//------------------------------------------------------------------------------------------------------------------------------------------
void D_DoomMain() noexcept {
    // PlayStation specific setup
    I_PSXInit();

    // Sound init:
    #if PSYDOOM_MODS
    {
        // PsyDoom: apply the sound and music volumes from the saved preferences file now, before we init sound
        PlayerPrefs::pushSoundAndMusicPrefs();

        // PsyDoom: allocate a buffer big enough to hold the WMD file (as it is on disk) temporarily.
        // The original PSX Doom used the 64 KiB static 'temp' buffer for this purpose; Final Doom did a temp 'Z_EndMalloc' of 122,880 bytes
        // because it's WMD file was much bigger. This method is more flexible and will allow for practically any sized WMD.
        const int32_t wmdFileSize = psxcd_get_file_size(CdFile::DOOMSND_WMD);
        std::unique_ptr<std::byte[]> wmdFileBuffer(new std::byte[wmdFileSize]);
        PsxSoundInit(doomToWessVol(gOptionsSndVol), doomToWessVol(gOptionsMusVol), wmdFileBuffer.get());
    }
    #else
        PsxSoundInit(doomToWessVol(gOptionsSndVol), doomToWessVol(gOptionsMusVol), gTmpBuffer);
    #endif

    // Initializing standard DOOM subsystems, zone memory management, WAD, platform stuff, renderer etc.
    Z_Init();
    I_Init();
    W_Init();
    R_Init();

    // PsyDoom: build the (now) dynamically generated lists of sprites, map objects, animated textures and switches for the game.
    // User mods can add new entries to any of these lists. Also initialize MAPINFO.
    #if PSYDOOM_MODS
        P_InitSprites();
        P_InitMobjInfo();
        P_InitAnimDefs();
        P_InitSwitchDefs();
        MapInfo::init();
    #endif

    ST_Init();

    #if PSYDOOM_MODS
        // PsyDoom: new cleanup logic before we exit
        const auto dmainCleanup = finally([]() noexcept {
            MapInfo::shutdown();
            W_Shutdown();
        });

        // PsyDoom: are we warping straight to a map and bypassing menus?
        if (ProgArgs::gWarpMap > 0) {
            gbStartupWarpToMap = true;
            gStartSkill = ProgArgs::gWarpSkill;
            gStartMapOrEpisode = ProgArgs::gWarpMap;
            gStartGameType = gt_single;
        }

        // PsyDoom: play intro movies and logos unless disabled.
        // Note: also skip them if we are playing a demo file or warping directly to a map.
        const bool bSkipIntros = (Config::gbSkipIntros || ProgArgs::gPlayDemoFilePath[0] || gbStartupWarpToMap);

        if (!bSkipIntros) {
            D_PlayIntros();
        }
    #endif

    // Clearing some global tick counters and inputs
    gPrevGameTic = 0;
    gGameTic = 0;
    gLastTgtGameTicCount = 0;
    gTicCon = 0;

    #if PSYDOOM_MODS
        D_UpdateIsLongGameTick();   // Needs to be called whenever we start a new game tick

        for (uint32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
            gTickInputs[playerIdx] = {};
            gOldTickInputs[playerIdx] = {};
        }

        gNextTickInputs = {};
        gTicButtons = 0;
        gOldTicButtons = 0;
    #else
        for (uint32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
            gTicButtons[playerIdx] = 0;
            gOldTicButtons[playerIdx] = 0;
        }
    #endif

    #if PSYDOOM_MODS
        // PsyDoom: put whatever password was saved into the game's password system.
        // This way it will be waiting for the player upon opening that menu:
        PlayerPrefs::pushLastPassword();

        // PsyDoom: play a single demo file and exit if commanded.
        // Also, if in headless mode then don't run the main game - only single demo playback is allowed.
        if (ProgArgs::gPlayDemoFilePath[0]) {
            RunDemoAtPath(ProgArgs::gPlayDemoFilePath);
            return;
        }

        if (ProgArgs::gbHeadlessMode)
            return;
    #endif

    // The main intro and demo scenes flow.
    // Continue looping until there is input and then execute the main menu until it times out.
    constexpr auto continueRunning = []() noexcept {
        // PsyDoom: the previously never-ending game loop can now be broken if the application is requesting to quit
        #if PSYDOOM_MODS
            return (!Input::isQuitRequested());
        #else
            return true;
        #endif
    };

    while (continueRunning()) {
        // PsyDoom: treat 'ga_quitapp' the same as 'ga_exit' here.
        // This makes us skip over the demo sequences and credits etc. if the app is quitting.
        constexpr auto didExit = [](const gameaction_t action) noexcept {
            #if PSYDOOM_MODS
                return ((action == ga_exit) || (action == ga_quitapp));
            #else
                return (action == ga_exit);
            #endif
        };

        if (!didExit(RunTitle())) {
            // PsyDoom: use a new, more flexible method of playing demos.
            // The constants for the game now define the list of demos to play.
            #if PSYDOOM_MODS
                bool bGotoTitle = false;    // Only go to the title if all demos were played without interruption and at least 1 demo was played

                for (uint32_t demoIdx = 0; demoIdx < C_ARRAY_SIZE(GameConstants::demos); ++demoIdx) {
                    // Grab the details for the current demo; if there are no more demos then playback stops:
                    gCurClassicDemo = Game::gConstants.demos[demoIdx];

                    if (gCurClassicDemo.filename.length() <= 0)
                        break;

                    // Run the demo itself
                    bGotoTitle = true;

                    if (didExit(RunDemo(gCurClassicDemo.filename))) {
                        bGotoTitle = false;
                        break;
                    }

                    // Show a credits screen after this demo?
                    if (gCurClassicDemo.bShowCreditsAfter) {
                        if (didExit(RunCredits())) {
                            bGotoTitle = false;
                            break;
                        }
                    }
                }

                // Re-run the title screen again?
                if (bGotoTitle)
                    continue;
            #else
                if (!didExit(RunDemo(CdFile::DEMO1_LMP))) {
                    if (!didExit(RunCredits())) {
                        if (!didExit(RunDemo(CdFile::DEMO2_LMP)))
                            continue;
                    }
                }
            #endif
        }

        while (continueRunning()) {
            // Go back to the title screen if timing out
            const gameaction_t result = RunMenu();

            if (result == ga_timeout)
                break;

            // PsyDoom: quit the application entirely if requested
            #if PSYDOOM_MODS
                if (result == ga_quitapp)
                    return;
            #endif
        }
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Runs a screen with scrolling legals text.
// This function is never called in the retail game, but was used for the PSX DOOM demo build.
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t RunLegals() noexcept {
    return MiniLoop(START_Legals, STOP_Legals, TIC_Legals, DRAW_Legals);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Runs the title screen
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t RunTitle() noexcept {
    // PsyDoom: if warping straight to a map then skip the title
    #if PSYDOOM_MODS
        if (gbStartupWarpToMap)
            return ga_exit;
    #endif

    return MiniLoop(START_Title, STOP_Title, TIC_Title, DRAW_Title);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Load and run the specified (built-in) demo file
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t RunDemo(const CdFileId file) noexcept {
    // PsyDoom: ensure this required graphic is loaded before starting the demo.
    // Also skip running the demo if the file does not exist.
    #if PSYDOOM_MODS
        if (!gTex_LOADING.bIsCached) {
            I_LoadAndCacheTexLump(gTex_LOADING, "LOADING", 0);
        }

        if (CdMapTbl_GetEntry(file).size <= 0)
            return ga_nothing;
    #endif

    // Open the demo file
    const uint32_t openFileIdx = OpenFile(file);

    // PsyDoom: determine the file size to read and only read the actual size of the demo rather than assuming it's 16 KiB.
    // Also allocate the demo buffer on the native host heap, so as to allow very large demos without affecting zone memory.
    #if PSYDOOM_MODS
        const int32_t demoFileSize = SeekAndTellFile(openFileIdx, 0, PsxCd_SeekMode::END);

        std::unique_ptr<std::byte[]> pDemoBuffer(new std::byte[demoFileSize]);
        gpDemoBuffer = pDemoBuffer.get();
        gpDemoBufferEnd = pDemoBuffer.get() + demoFileSize;

        SeekAndTellFile(openFileIdx, 0, PsxCd_SeekMode::SET);
        ReadFile(openFileIdx, gpDemoBuffer, demoFileSize);
    #else
        // Read the demo file contents (up to 16 KiB)
        constexpr uint32_t DEMO_BUFFER_SIZE = 16 * 1024;
        gpDemoBuffer = (std::byte*) Z_EndMalloc(*gpMainMemZone, DEMO_BUFFER_SIZE, PU_STATIC, nullptr);
        ReadFile(openFileIdx, gpDemoBuffer->get(), 16 * 1024);
    #endif

    CloseFile(openFileIdx);

    // Play the demo, free the demo buffer and return the exit action
    const gameaction_t exitAction = G_PlayDemoPtr();

    // PsyDoom: the demo buffer is no longer allocated through the zone memory system; std::unique_ptr will also cleanup after itself
    #if !PSYDOOM_MODS
        Z_Free2(*gpMainMemZone, gpDemoBuffer);
    #endif

    return exitAction;
}

#if PSYDOOM_MODS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom: load and run the specified demo file at the specified path on the host machine
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t RunDemoAtPath(const char* const filePath) noexcept {
    // Ensure this required graphic is loaded before starting the demo.
    // PsyDoom: the meaning of 'texPageId' has changed slightly, '0' is now the 1st page and 'bIsCached' is used check cache residency.
    if (!gTex_LOADING.bIsCached) {
        I_LoadAndCacheTexLump(gTex_LOADING, "LOADING", 0);
    }

    // Read the demo file into memory
    const FileData fileData = FileUtils::getContentsOfFile(filePath);

    if (!fileData.bytes) {
        FatalErrors::raiseF("Unable to read demo file '%s'! Is the file path valid?", filePath);
    }

    // Set the info for the current classic demo in case we are playing one of those.
    // Use the current game settings to determine the demo's game behavior and format.
    ClassicDemoDef& demoDef = gCurClassicDemo;
    demoDef = {};
    demoDef.bFinalDoomDemo = (Game::gGameType != GameType::Doom);
    demoDef.bPalDemo = (Game::gGameVariant == GameVariant::PAL);

    // Setup the demo buffers and play the demo file
    gpDemoBuffer = fileData.bytes.get();
    gpDemoBufferEnd = fileData.bytes.get() + fileData.size;

    const gameaction_t exitAction = G_PlayDemoPtr();

    // Cleanup after we are done and return the exit action
    gpDemoBuffer = nullptr;
    gpDemoBufferEnd = nullptr;

    return exitAction;
}
#endif  // #if PSYDOOM_MODS

//------------------------------------------------------------------------------------------------------------------------------------------
// Runs the credits screen
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t RunCredits() noexcept {
    return MiniLoop(START_Credits, STOP_Credits, TIC_Credits, DRAW_Credits);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Set the text position for the debug draw string
//------------------------------------------------------------------------------------------------------------------------------------------
void I_SetDebugDrawStringPos(const int32_t x, const int32_t y) noexcept {
    gDebugDrawStringXPos = x;
    gDebugDrawStringYPos = y;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draw the debug draw string.
// The string also scrolls down the screen with repeated calls.
//------------------------------------------------------------------------------------------------------------------------------------------
void I_DebugDrawString(const char* const fmtMsg, ...) noexcept {
    // Setup the drawing mode
    {
        // PsyDoom: explicitly clear the texture window here also to disable wrapping - don't rely on previous drawing code to do that
        // PsyDoom: use local instead of scratchpad draw primitives; compiler can optimize better, and removes reliance on global state
        #if PSYDOOM_MODS
            DR_MODE drawModePrim = {};
            const SRECT texWindow = { (int16_t) gTex_STATUS.texPageCoordX, (int16_t) gTex_STATUS.texPageCoordY, 256, 256 };
            LIBGPU_SetDrawMode(drawModePrim, false, false, gTex_STATUS.texPageId, &texWindow);
        #else
            DR_MODE& drawModePrim = *(DR_MODE*) LIBETC_getScratchAddr(128);
            LIBGPU_SetDrawMode(drawModePrim, false, false, gTex_STATUS.texPageId, nullptr);
        #endif
    }

    // Setting up some sprite primitive stuff for the 'draw string' call that follows.
    // PsyDoom: no longer need to do this, not using scratchpad globals and 'draw string' now requires explicit shading settings.
    #if !PSYDOOM_MODS
    {
        SPRT& spritePrim = *(SPRT*) LIBETC_getScratchAddr(128);

        LIBGPU_SetSprt(spritePrim);
        LIBGPU_SetSemiTrans(spritePrim, false);
        LIBGPU_SetShadeTex(spritePrim, false);
        LIBGPU_setRGB0(spritePrim, 128, 128, 128);
        spritePrim.clut = Game::getTexPalette_DebugFontSmall();
    }
    #endif  // #if !PSYDOOM_MODS

    // Format the message and print
    char msgBuffer[256];

    {
        va_list args;
        va_start(args, fmtMsg);

        // PsyDoom: Use 'vsnprint' as it's safer!
        #if PSYDOOM_MODS
            std::vsnprintf(msgBuffer, C_ARRAY_SIZE(msgBuffer), fmtMsg, args);
        #else
            D_vsprintf(msgBuffer, fmtMsg, args);
        #endif

        va_end(args);
    }

    // PsyDooom: have to explicitly specify sprite shading parameters now for 'draw string' rather than relying on global state
    #if PSYDOOM_MODS
        I_DrawStringSmall(
            gDebugDrawStringXPos,
            gDebugDrawStringYPos,
            msgBuffer,
            Game::getTexPalette_STATUS(),
            128,
            128,
            128,
            false,
            false
        );
    #else
        I_DrawStringSmall(gDebugDrawStringXPos, gDebugDrawStringYPos, msgBuffer);
    #endif

    // The message scrolls down the screen as it is drawn more
    gDebugDrawStringYPos += 8;
}

#if PSYDOOM_MODS
//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom: draws frame performance counters (average frame duration and FPS) at the top left of the screen if they are enabled
//------------------------------------------------------------------------------------------------------------------------------------------
void I_DrawEnabledPerfCounters() noexcept {
    // Are we showing performance counters?
    if (!Config::gbShowPerfCounters)
        return;

    // If using the Vulkan renderer, draw then as far as possible to the left, being widescreen aware:
    int32_t widescreenAdjust = 0;

    #if PSYDOOM_VULKAN_RENDERER
        if (Video::isUsingVulkanRenderPath() && Config::gbVulkanWidescreenEnabled) {
            // Compute the extra space/padding at the left and right sides of the screen (in PSX coords) due to widescreen.
            // This is the same calculation used by the Vulkan renderer in 'VDrawing::computeTransformMatrixForUI'.
            const float xPadding = (VRenderer::gPsxCoordsFbX / VRenderer::gPsxCoordsFbW) * (float) SCREEN_W;
            widescreenAdjust = (int32_t) -xPadding;
        }
    #endif

    // Need to setup the texture window beforehand for the draw string calls
    {
        DR_MODE drawModePrim = {};
        const SRECT texWindow = { (int16_t) gTex_STATUS.texPageCoordX, (int16_t) gTex_STATUS.texPageCoordY, 256, 256 };
        LIBGPU_SetDrawMode(drawModePrim, false, false, gTex_STATUS.texPageId, &texWindow);
        I_AddPrim(drawModePrim);
    }

    // Show average frame microseconds elapsed
    char msgBuffer[256];
    std::snprintf(msgBuffer, sizeof(msgBuffer), "USEC: %zu", (size_t)(gPerfAvgUsec + 0.5f));
    I_DrawStringSmall(2 + widescreenAdjust, 2, msgBuffer, Game::getTexPalette_STATUS(), 128, 255, 255, false, false);

    // Show average FPS counter
    std::snprintf(msgBuffer, sizeof(msgBuffer), "FPS:  %.1f", gPerfAvgFps);
    I_DrawStringSmall(2 + widescreenAdjust, 10, msgBuffer, Game::getTexPalette_STATUS(), 128, 255, 255, false, false);
}
#endif  // #if PSYDOOM_MODS

//------------------------------------------------------------------------------------------------------------------------------------------
// Set a region of memory to a specified byte value.
// Bulk writes in 32-byte chunks where possible.
//------------------------------------------------------------------------------------------------------------------------------------------
void D_memset(void* const pDst, const std::byte fillByte, const uint32_t count) noexcept {
    // Fill up until the next aligned 32-bit address
    uint32_t bytesLeft = count;
    std::byte* pDstByte = (std::byte*) pDst;

    while (((uintptr_t) pDstByte & 3) != 0) {
        if (bytesLeft == 0)
            return;

        *pDstByte = fillByte;
        ++pDstByte;
        --bytesLeft;
    }

    // Fill 32 bytes at a time (with 8 writes)
    {
        const uint32_t fb32 = (uint32_t) fillByte;
        const uint32_t fillWord = (fb32 << 24) | (fb32 << 16) | (fb32 << 8) | fb32;

        while (bytesLeft >= 32) {
            uint32_t* const pDstWords = (uint32_t*) pDstByte;

            pDstWords[0] = fillWord;
            pDstWords[1] = fillWord;
            pDstWords[2] = fillWord;
            pDstWords[3] = fillWord;
            pDstWords[4] = fillWord;
            pDstWords[5] = fillWord;
            pDstWords[6] = fillWord;
            pDstWords[7] = fillWord;

            pDstByte += 32;
            bytesLeft -= 32;
        }
    }

    // Fill the remaining bytes
    while (bytesLeft != 0) {
        *pDstByte = fillByte;
        bytesLeft--;
        pDstByte++;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copy a number of bytes from source to destination
//------------------------------------------------------------------------------------------------------------------------------------------
void D_memcpy(void* const pDst, const void* const pSrc, const uint32_t numBytes) noexcept {
    uint32_t bytesLeft = numBytes;
    std::byte* pDstByte = (std::byte*) pDst;
    const std::byte* pSrcByte = (const std::byte*) pSrc;

    while (bytesLeft != 0) {
        bytesLeft--;
        *pDstByte = *pSrcByte;
        ++pSrcByte;
        ++pDstByte;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copy up to 'maxChars' from 'src' to 'dst'
//------------------------------------------------------------------------------------------------------------------------------------------
void D_strncpy(char* dst, const char* src, uint32_t maxChars) noexcept {
    while (maxChars != 0) {
        const char c = *dst = *src;
        --maxChars;
        ++src;
        ++dst;

        if (c == 0)
            break;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Compare two strings, up to 'maxCount' characters.
// Return '0' if equal or '1' if not equal.
// Confusingly, unlike the equivalent standard C function, this comparison is *NOT* case insensitive.
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t D_strncasecmp(const char* str1, const char* str2, int32_t maxCount) noexcept {
    while (*str1 && *str2) {
        if (*str1 != *str2)
            return 1;

        ++str1;
        ++str2;
        --maxCount;

        // Bug fix: if the function is called with 'maxCount' as '0' for some reason then prevent a near infinite loop
        // due to wrapping around to '-1'. I don't think this happened in practice but just guard against it here anyway
        // in case future mods happen to trigger this issue...
        #if PSYDOOM_MODS
            if (maxCount <= 0)
                return 0;
        #else
            if (maxCount == 0)
                return 0;
        #endif
    }

    return (*str1 == *str2);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Makes the given ASCII string uppercase
//------------------------------------------------------------------------------------------------------------------------------------------
void D_strupr(char* str) noexcept {
    for (char c = *str; c != 0; c = *str) {
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }

        *str = c;
        ++str;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Runs the game loop for a menu screen or for the level gameplay.
// Calls startup/shutdown functions and drawer/ticker functions.
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t MiniLoop(
    void (*const pStart)(),
    void (*const pStop)(const gameaction_t exitAction),
    gameaction_t (*const pTicker)(),
    void (*const pDrawer)()
) noexcept {
    // Network initialization
    if (gNetGame != gt_single) {
        I_NetHandshake();
    }

    // Init timers and exit action
    gGameAction = ga_nothing;
    gPrevGameTic = 0;
    gGameTic = 0;
    gTicCon = 0;
    gLastTgtGameTicCount = 0;

    #if PSYDOOM_MODS
        gbIsFirstTick = true;
        D_UpdateIsLongGameTick();   // Needs to be called whenever we start a new game tick
        Input::consumeEvents();     // Clear any input events leftover
    #endif

    // Run startup logic for this game loop beginning
    pStart();

    // PsyDoom: sound update in case the start action played something
    #if PSYDOOM_MODS
        S_UpdateSounds();
    #endif

    // Update the video refresh timers.
    // PsyDoom: use 'I_GetTotalVBlanks' because it can adjust time in networked games.
    #if PSYDOOM_MODS
        gLastTotalVBlanks = I_GetTotalVBlanks();
    #else
        gLastTotalVBlanks = LIBETC_VSync(-1);
    #endif

    gElapsedVBlanks = 0;

    // PsyDoom: stuff relating to profiling the game loop and timing frame durations
    typedef std::chrono::high_resolution_clock frametimer_t;

    #if PSYDOOM_MODS
        frametimer_t::time_point frameStartTime = frametimer_t::now();      // When we started the current frame
        gPrevFrameDuration = 0.0;                                           // No previous frame duration (yet)

        frametimer_t::time_point profilerStartTime = frameStartTime;        // When we started profiling the current few frames
        uint32_t profilerNumFramesElapsed = 0;                              // How many frames have elapsed for the frame profiler
        gPerfAvgFps = 0;                                                    // Don't know this yet, frame profiler will tell us later!
        gPerfAvgUsec = 0;                                                   // Don't know this yet, frame profiler will tell us later!
    #endif

    // Continue running the game loop until something causes us to exit
    gameaction_t exitAction = ga_nothing;

    while (true) {
        // PsyDoom: initially assume no elasped vblanks for all players until found otherwise.
        // For net games we should get some elapsed vblanks from the other player in their packet, if it's time to read a new packet.
        // It will be time to read a new packet if we update inputs and timing.
        #if PSYDOOM_MODS
            for (int32_t i = 0; i < MAXPLAYERS; ++i) {
                gPlayersElapsedVBlanks[i] = 0;
            }
        #endif

        // Update timing and buttons.
        // PsyDoom: only do if enough time has elapsed or if it's the first frame, due to potentially uncapped framerate.
        gPlayersElapsedVBlanks[gCurPlayerIndex] = gElapsedVBlanks;

        #if PSYDOOM_MODS
            const bool bUpdateInputsAndTiming = ((gElapsedVBlanks > 0) || gbIsFirstTick);
        #else
            const bool bUpdateInputsAndTiming = true;
        #endif

        if (bUpdateInputsAndTiming) {
            // Read pad inputs and save as the current pad buttons (note: overwritten if a demo); also save old inputs for button just pressed detection.
            // PsyDoom: read tick inputs in addition to raw gamepad inputs, this is now the primary input source.
            #if PSYDOOM_MODS
                for (uint32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
                    gOldTickInputs[playerIdx] = gTickInputs[playerIdx];
                }

                gOldTicButtons = gTicButtons;

                // Note: ensure we have the latest input events prior to this with a call to 'Input::update'
                TickInputs& tickInputs = gTickInputs[gCurPlayerIndex];
                Input::update();
                P_GatherTickInputs(tickInputs);
                gTicButtons = I_ReadGamepad();
            #else
                for (uint32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
                    gOldTicButtons[playerIdx] = gTicButtons[playerIdx];
                }

                uint32_t padBtns = I_ReadGamepad();
                gTicButtons[gCurPlayerIndex] = padBtns;
            #endif

            if (gNetGame != gt_single) {
                // PsyDoom: check if any keys to exit demo playback are pressed.
                // Have to do it here before the network update, since that overwrites actual physical user inputs.
                #if PSYDOOM_MODS
                    const bool bExitDemoPlaybackKeysPressed = (tickInputs.fMenuOk() || tickInputs.fMenuBack() || tickInputs.fMenuStart());
                #endif

                // Updates for when we are in a networked game: abort from the game also if there is a problem
                const bool bNetError = I_NetUpdate();

                if (bNetError) {
                    // PsyDoom: if a network error occurs don't try to restart the level, the connection is most likely still gone.
                    // Exit to the main menu instead.
                    #if PSYDOOM_MODS
                        gGameAction = ga_exitdemo;
                        exitAction = ga_exitdemo;
                    #else
                        gGameAction = ga_warped;
                        exitAction = ga_warped;
                    #endif

                    break;
                }

                #if PSYDOOM_MODS
                    // PsyDoom: recording demo ticks for multiplayer mode
                    if (DemoRecorder::isRecording()) {
                        DemoRecorder::recordTick();
                    }

                    // PsyDoom: check if the demo is done due to the pause key being pressed.
                    // When playing back check for the exit demo keys or for when the end of the demo is reached.
                    const bool bIsAnyPlayerPausing = (gTickInputs[0].fTogglePause() || gTickInputs[1].fTogglePause());
                    const bool bDoingADemo = (gbDemoPlayback || gbNetIsGameBeingRecorded);
                    const bool bPausedDuringADemo = (bDoingADemo && bIsAnyPlayerPausing);
                    const bool bExitDemoPlayback = (gbDemoPlayback && bExitDemoPlaybackKeysPressed);
                    const bool bDemoPlaybackFinished = (gbDemoPlayback && DemoPlayer::hasReachedDemoEnd());

                    if (bPausedDuringADemo || bExitDemoPlayback || bDemoPlaybackFinished) {
                        // If pausing while recording then just end recording and allow gameplay to proceed instead of quitting the game
                        if (bPausedDuringADemo && gbNetIsGameBeingRecorded) {
                            if (DemoRecorder::isRecording()) {
                                DemoRecorder::end();
                                gStatusBar.message = "Recording ended";
                                gStatusBar.messageTicsLeft = 30;
                            }
                        }
                        else {
                            exitAction = ga_exitdemo;
                            gGameAction = ga_exitdemo;
                            break;
                        }
                    }
                #endif
            }
            else if (gbDemoRecording || gbDemoPlayback) {
                // Demo recording or playback.
                // Need to either read inputs from or save them to a buffer.
                if (gbDemoPlayback) {
                    // Demo playback: any button pressed on the gamepad will abort.
                    // PsyDoom: just use the menu action buttons to abort.
                    exitAction = ga_exit;

                    #if PSYDOOM_MODS
                        if (tickInputs.fMenuOk() || tickInputs.fMenuBack() || tickInputs.fMenuStart())
                            break;
                    #else
                        if (padBtns & PAD_ANY_BTNS)
                            break;
                    #endif

                    // Read inputs from the demo buffer and advance the demo.
                    // N.B: Demo inputs override everything else from here on in.
                    #if PSYDOOM_MODS
                        if (!DemoPlayer::readTickInputs())
                            break;
                    #else
                        padBtns = *gpDemo_p;
                        gTicButtons[gCurPlayerIndex] = padBtns;
                    #endif
                }
                else {
                    // Demo recording: record pad inputs to the buffer.
                    // PsyDoom: this logic is now handled by the demo recording module.
                    #if PSYDOOM_MODS
                        if (DemoRecorder::isRecording()) {
                            DemoRecorder::recordTick();
                        }
                    #else
                        *gpDemo_p = padBtns;
                    #endif
                }

                // Abort demo recording or playback?
                exitAction = ga_exitdemo;

                #if PSYDOOM_MODS
                    // PsyDoom: if pausing while recording then just end recording and allow gameplay to proceed instead of quitting the game
                    if (gTickInputs[gCurPlayerIndex].fTogglePause()) {
                        if (gbDemoRecording) {
                            DemoRecorder::end();
                            gbDemoRecording = false;
                            gStatusBar.message = "Recording ended";
                            gStatusBar.messageTicsLeft = 30;
                        } 
                        else {
                            gGameAction = ga_exitdemo;
                            break;
                        }
                    }
                #else
                    if (padBtns & PAD_START)
                        break;
                #endif

                #if PSYDOOM_MODS
                    // PsyDoom: don't assume the demo playback buffer is a fixed size, this allows us to work with demos of any size.
                    // Also note that the last tick of the demo does not get executed with this statement, which was the original behavior.
                    if (gbDemoPlayback && DemoPlayer::hasReachedDemoEnd())
                        break;
                #else
                    // Is the demo recording too big? Are we at the end of the largest possible demo size? If so then stop right now...
                    const int32_t demoTicksElapsed = (int32_t)(gpDemo_p - gpDemoBuffer);

                    if (demoTicksElapsed >= MAX_DEMO_TICKS)
                        break;
                #endif
            }

            // Advance the number of 1 vblank ticks passed.
            // N.B: the tick count used here is ALWAYS for player 1, this is how time is kept in sync for a network game.
            gTicCon += gPlayersElapsedVBlanks[0];

            // Advance to the next game tick if it is time; video refreshes at 60 Hz (NTSC) but the game ticks at 15 Hz (NTSC).
            // PsyDoom: some tweaks here also to make PAL mode gameplay behave the same as the original game.
            #if PSYDOOM_MODS
                const int32_t tgtGameTicCount = (Game::gSettings.bUsePalTimings) ? gTicCon / 3 : d_rshift<VBLANK_TO_TIC_SHIFT>(gTicCon);
            #else
                const int32_t tgtGameTicCount = d_rshift<VBLANK_TO_TIC_SHIFT>(gTicCon);
            #endif

            if (gLastTgtGameTicCount < tgtGameTicCount) {
                gLastTgtGameTicCount = tgtGameTicCount;
                gGameTic++;

                // PsyDoom: update the adjustments we make to interpolation for the PAL case (outside of demo timings)
                #if PSYDOOM_MODS
                    D_UpdateIsLongGameTick();
                #endif
            }
        }

        // Call the ticker function to do updates for the frame.
        // Note that I am calling this in all situations, even if the framerate is capped and if we haven't passed enough time for a game tick.
        // That allows for possible update logic which runs > 30 Hz in future, like framerate uncapped turning movement.
        exitAction = pTicker();

        if (exitAction != ga_nothing)
            break;

        // PsyDoom: allow renderer toggle and clear input events after the ticker has been called.
        // Unless the ticker has requested that we hold onto them.
        // Also check if the app wants to quit, because the window was closed.
        #if PSYDOOM_MODS
            if (!gbKeepInputEvents) {
                Utils::checkForRendererToggleInput();
                Input::consumeEvents();
            } else {
                gbKeepInputEvents = false;  // Temporary request only!
            }

            if (Input::isQuitRequested()) {
                exitAction = ga_quitapp;
                break;
            }
        #endif

        // Call the drawer function to do drawing for the frame
        pDrawer();

        // Do we need to update sound? (sound updates at 15 Hz)
        // PsyDoom: allow updates at any rate so sounds start as soon as possible.
        #if PSYDOOM_MODS
            S_UpdateSounds();
        #else
            if (gGameTic > gPrevGameTic) {
                S_UpdateSounds();
            }
        #endif

        gPrevGameTic = gGameTic;
        gbIsFirstTick = false;

        #if PSYDOOM_MODS
            // PsyDoom: wrap up timing this frame's duration
            const frametimer_t::time_point now = frametimer_t::now();
            gPrevFrameDuration = std::chrono::duration<double>(now - frameStartTime).count();
            frameStartTime = now;

            // PsyDoom: update frame time profiling if enough time has passed
            profilerNumFramesElapsed++;
            const float timeSincePerfUpdate = std::chrono::duration<float>(now - profilerStartTime).count();

            if (timeSincePerfUpdate >= PERF_COUNTER_FREQ) {
                // Compute and save the performance metrics
                const frametimer_t::duration elapsedTime = now - profilerStartTime;
                const std::chrono::microseconds elapsedTimeUsec = std::chrono::duration_cast<std::chrono::microseconds>(elapsedTime);

                const double avgUsec = (double) elapsedTimeUsec.count() / (double) profilerNumFramesElapsed;
                const double avgElapsedSec = avgUsec / 1000000.0;
                const double avgFps = (avgElapsedSec > 0.0) ? 1.0 / avgElapsedSec : 999999.0;

                gPerfAvgUsec = (float) avgUsec;
                gPerfAvgFps = (float) avgFps;

                // Begin a new profiling iteration
                profilerNumFramesElapsed = 0;
                profilerStartTime = now;
            }
        #endif
    }

    // PsyDoom: one last sound update before we exit
    #if PSYDOOM_MODS
        S_UpdateSounds();
    #endif

    // Run cleanup logic for this game loop ending
    pStop(exitAction);

    // PsyDoom: sound update in case the stop action played something
    #if PSYDOOM_MODS
        S_UpdateSounds();
    #endif

    // Current inputs become the old ones
    #if PSYDOOM_MODS
        for (uint32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
            gOldTickInputs[playerIdx] = gTickInputs[playerIdx];
        }

        gOldTicButtons = gTicButtons;
    #else
        for (uint32_t playerIdx = 0; playerIdx < MAXPLAYERS; ++playerIdx) {
            gOldTicButtons[playerIdx] = gTicButtons[playerIdx];
        }
    #endif

    // Return the exit game action
    return exitAction;
}

#if PSYDOOM_MODS
//------------------------------------------------------------------------------------------------------------------------------------------
// Tells if the duration of a game/world tick varies.
// See the documentation of 'gbIsLongGameTick' for more details.
//------------------------------------------------------------------------------------------------------------------------------------------
bool D_GameTickDurationVaries() noexcept {
    return (Game::gSettings.bUsePalTimings && (!Game::gSettings.bUseDemoTimings));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Updates whether the current game/world tick is a 'long' duration tick.
// See the documentation of 'gbIsLongGameTick' for more details.
//------------------------------------------------------------------------------------------------------------------------------------------
void D_UpdateIsLongGameTick() noexcept {
    if (D_GameTickDurationVaries()) {
        gbIsLongGameTick = (gTicCon % 3 == 0);
    } else {
        gbIsLongGameTick = false;
    }
}
#endif  // #if PSYDOOM_MODS
