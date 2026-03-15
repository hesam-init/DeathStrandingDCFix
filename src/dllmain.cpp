#include "stdafx.h"
#include "helper.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <inipp/inipp.h>
#include <safetyhook.hpp>

#define spdlog_confparse(var) spdlog::info("Config Parse: {}: {}", #var, var)

HMODULE exeModule = GetModuleHandle(NULL);
HMODULE thisModule;

// Fix details
std::string sFixName = "DeathStrandingDCFix";
std::string sFixVersion = "0.0.1";
std::filesystem::path FixPath;

// Ini
inipp::Ini<char> ini;
std::string sConfigFile = sFixName + ".ini";

// Logger
std::shared_ptr<spdlog::logger> logger;
std::string sLogFile = sFixName + ".log";
std::filesystem::path ExePath;
std::string sExeName;

// Aspect ratio / FOV / HUD
std::pair DesktopDimensions = { 0,0 };
const float fPi = 3.1415926535f;
const float fNativeAspect = (16.00f / 9.00f);


float fAspectRatio;
float fAspectMultiplier;
float fHUDWidth;
float fHUDWidthOffset;
float fHUDHeight;
float fHUDHeightOffset;

// Ini variables
bool bIntroSkip;
bool bCustomRes;
int iCustomResX;
int iCustomResY;
bool customCapEnabled;
int customCap;
bool bFixMovies;
bool bFixFOV;
bool bFixHUD;
float fGameplayFOVMulti;
float fLODDistance;
bool bCenterHUD;
bool bEntityUpdateRate;

// Variables
int iCurrentResX;
int iCurrentResY;
bool bIntroSkipped = false;

void CalculateAspectRatio(bool bLog)
{
    if (iCurrentResX <= 0 || iCurrentResY <= 0)
        return;

    // Calculate aspect ratio
    fAspectRatio = (float)iCurrentResX / (float)iCurrentResY;
    fAspectMultiplier = fAspectRatio / fNativeAspect;

    // HUD
    fHUDWidth = (float)iCurrentResY * fNativeAspect;
    fHUDHeight = (float)iCurrentResY;
    fHUDWidthOffset = (float)(iCurrentResX - fHUDWidth) / 2.00f;
    fHUDHeightOffset = 0.00f;
    if (fAspectRatio < fNativeAspect) {
        fHUDWidth = (float)iCurrentResX;
        fHUDHeight = (float)iCurrentResX / fNativeAspect;
        fHUDWidthOffset = 0.00f;
        fHUDHeightOffset = (float)(iCurrentResY - fHUDHeight) / 2.00f;
    }

    // Log details about current resolution
    if (bLog) {
        spdlog::info("----------");
        spdlog::info("Current Resolution: Resolution: {:d}x{:d}", iCurrentResX, iCurrentResY);
        spdlog::info("Current Resolution: fAspectRatio: {}", fAspectRatio);
        spdlog::info("Current Resolution: fAspectMultiplier: {}", fAspectMultiplier);
        spdlog::info("Current Resolution: fHUDWidth: {}", fHUDWidth);
        spdlog::info("Current Resolution: fHUDHeight: {}", fHUDHeight);
        spdlog::info("Current Resolution: fHUDWidthOffset: {}", fHUDWidthOffset);
        spdlog::info("Current Resolution: fHUDHeightOffset: {}", fHUDHeightOffset);
        spdlog::info("----------");
    }
}

void Logging()
{
    // Get path to DLL
    WCHAR dllPath[_MAX_PATH] = {0};
    GetModuleFileNameW(thisModule, dllPath, MAX_PATH);
    FixPath = dllPath;
    FixPath = FixPath.remove_filename();

    // Get game name and exe path
    WCHAR gameExePath[_MAX_PATH] = {0};
    GetModuleFileNameW(exeModule, gameExePath, MAX_PATH);
    ExePath = gameExePath;
    sExeName = ExePath.filename().string();
    ExePath = ExePath.remove_filename();

    // Spdlog initialisation
    try
    {
        // Truncate existing log file
        std::ofstream file(ExePath / sLogFile, std::ios::trunc);
        if (file.is_open()) file.close();

        // Create single log file that's size-limited to 10MB
        logger = std::make_shared<spdlog::logger>(sFixName, std::make_shared<spdlog::sinks::rotating_file_sink_st>(ExePath.string() + sLogFile, 10 * 1024 * 1024, 1));
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::debug);

        #ifdef _DEBUG
        spdlog::set_level(spdlog::level::debug);
        #endif

        spdlog::info("----------");
        spdlog::info("{:s} v{:s} loaded.", sFixName, sFixVersion);
        spdlog::info("----------");
        spdlog::info("Log file: {}", FixPath.string() + sLogFile);
        spdlog::info("----------");
        spdlog::info("Module Name: {:s}", sExeName);
        spdlog::info("Module Path: {}", ExePath.string());
        spdlog::info("Module Address: 0x{:x}", (uintptr_t)exeModule);
        spdlog::info("Module Timestamp: {:d}", Memory::ModuleTimestamp(exeModule));
        spdlog::info("----------");
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        AllocConsole();
        FILE *dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        std::cout << "Log initialisation failed: " << ex.what() << std::endl;
        FreeLibraryAndExitThread(thisModule, 1);
    }
}

void Configuration()
{
    // Inipp initialisation
    std::ifstream iniFile(FixPath / sConfigFile);
    if (!iniFile)
    {
        AllocConsole();
        FILE *dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        std::cout << "" << sFixName.c_str() << " v" << sFixVersion.c_str() << " loaded." << std::endl;
        std::cout << "ERROR: Could not locate config file." << std::endl;
        std::cout << "ERROR: Make sure " << sConfigFile.c_str() << " is located in " << FixPath.string().c_str() << std::endl;
        spdlog::error("ERROR: Could not locate config file {}", sConfigFile);
        spdlog::shutdown();
        FreeLibraryAndExitThread(thisModule, 1);
    }
    else
    {
        spdlog::info("Config file: {}", FixPath.string() + sConfigFile);
        ini.parse(iniFile);
    }

    // Parse config
    ini.strip_trailing_comments();
    spdlog::info("----------");

    // Load settings from ini
    inipp::get_value(ini.sections["Intro Skip"], "Enabled", bIntroSkip);
    inipp::get_value(ini.sections["Custom FPS Limit"], "Enabled", customCapEnabled);
    inipp::get_value(ini.sections["Custom FPS Limit"], "Limit", customCap);
    inipp::get_value(ini.sections["Custom Resolution"], "Enabled", bCustomRes);
    inipp::get_value(ini.sections["Custom Resolution"], "Width", iCustomResX);
    inipp::get_value(ini.sections["Custom Resolution"], "Height", iCustomResY);
    inipp::get_value(ini.sections["Force Entity Updates"], "Enabled", bEntityUpdateRate);
    inipp::get_value(ini.sections["Gameplay FOV"], "Multiplier", fGameplayFOVMulti);
    inipp::get_value(ini.sections["LOD"], "Distance", fLODDistance);
    inipp::get_value(ini.sections["Center HUD"], "Enabled", bCenterHUD);
    inipp::get_value(ini.sections["Fix Movies"], "Enabled", bFixMovies);
    inipp::get_value(ini.sections["Fix FOV"], "Enabled", bFixFOV);
    inipp::get_value(ini.sections["Fix HUD"], "Enabled", bFixHUD);

    // Clamp settings
    fGameplayFOVMulti = std::clamp(fGameplayFOVMulti, 0.10f, 2.00f);
    fLODDistance = std::clamp(fLODDistance, 0.00f, 5.00f);

    // Log ini parse
    spdlog_confparse(bIntroSkip);
    spdlog_confparse(bCustomRes);
    spdlog_confparse(iCustomResX);
    spdlog_confparse(iCustomResY);
    spdlog_confparse(bEntityUpdateRate);
    spdlog_confparse(fGameplayFOVMulti);
    spdlog_confparse(fLODDistance);
    spdlog_confparse(bCenterHUD);
    spdlog_confparse(bFixMovies);
    spdlog_confparse(bFixFOV);
    spdlog_confparse(bFixHUD);

    spdlog::info("----------");
}

void Resolution()
{
    // Current resolution
    std::uint8_t* CurrentResolutionScanResult = Memory::PatternScan(exeModule, "89 ?? ?? 44 89 ?? ?? 44 ?? ?? ?? 33 ?? 49 ?? ?? ?? 4D ?? ?? ?? 8B ??");
    if (CurrentResolutionScanResult) {
        spdlog::info("Current Resolution: Address is {:s}+{:x}", sExeName.c_str(), CurrentResolutionScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
        static SafetyHookMid CurrentResolutionMidHook{};
        CurrentResolutionMidHook = safetyhook::create_mid(CurrentResolutionScanResult,
            [](SafetyHookContext& ctx) {
                int iResX = (int)ctx.rdx;
                int iResY = (int)ctx.r8;

                if (iResX != iCurrentResX || iResY != iCurrentResY)
                {
                    iCurrentResX = iResX;
                    iCurrentResY = iResY;
                    CalculateAspectRatio(true);
                }
            });
    }
    else {
        spdlog::error("Current Resolution: Pattern scan failed.");
    }

    if (bCustomRes)
    {
        // Grab desktop resolution
        DesktopDimensions = Util::GetPhysicalDesktopDimensions();

        // Use desktop resolution if custom resolution is set automatic/invalid
        if (iCustomResX <= 0 || iCustomResY <= 0) {
            iCustomResX = DesktopDimensions.first;
            iCustomResY = DesktopDimensions.second;
        }

        iCurrentResX = iCustomResX;
        iCurrentResY = iCustomResY;
        CalculateAspectRatio(true);

        // Graphics window setting check
        std::uint8_t* GraphicsConfigCheckScanResult = Memory::PatternScan(exeModule, "E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8D ?? ?? ?? ?? ?? 44 39 ?? ?? ?? ?? ?? 0F 87 ?? ?? ?? ?? 8B ?? ?? ?? ?? ??");
        if (GraphicsConfigCheckScanResult) {
            spdlog::info("Custom Resolution: Config Check: Address is {:s}+{:x}", sExeName.c_str(), GraphicsConfigCheckScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            Memory::PatchBytes(GraphicsConfigCheckScanResult + 0x7, "\x90\x90\x90\x90\x90\x90", 6);
        }
        else {
            spdlog::error("Custom Resolution: Config Check: Pattern scan failed.");
        }

        // Menu resolution check
        std::uint8_t* MenuResCheckScanResult = Memory::PatternScan(exeModule, "C5 F2 ?? ?? 45 8B ?? ?? 45 85 ?? 7E ?? C5 ?? ?? ?? 85 ?? 74 ??");
        if (MenuResCheckScanResult) {
            spdlog::info("Custom Resolution: Menu Check: Address is {:s}+{:x}", sExeName.c_str(), MenuResCheckScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid MenuResCheckMidHook{};
            MenuResCheckMidHook = safetyhook::create_mid(MenuResCheckScanResult + 0x4,
                [](SafetyHookContext& ctx) {
                    // If current aspect ratio > 2.33~, then current aspect ratio = 2.33~ so that it is valid to display in the menu
                    ctx.xmm2.f32[0] = std::min(ctx.xmm2.f32[0], ctx.xmm7.f32[0]);
                });
        }
        else {
            spdlog::error("Custom Resolution: Menu Check: Pattern scan failed.");
        }

        // Remove restriction on available windowed resolutions
        std::uint8_t* WindowedResRestrictionScanResult = Memory::PatternScan(exeModule, "49 ?? ?? 01 75 ?? 8B ?? ?? 41 ?? ?? 2B ?? ?? 41 ?? ?? 3B ?? 0F 8D ?? ?? ?? ??");
        if (WindowedResRestrictionScanResult) {
            spdlog::info("Custom Resolution: Windowed Resolution Restriction: Address is {:s}+{:x}", sExeName.c_str(), WindowedResRestrictionScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            Memory::PatchBytes(WindowedResRestrictionScanResult + 0x4, "\xEB", 1);
        }
        else {
            spdlog::error("Custom Resolution: Windowed Resolution Restriction: Pattern scan failed.");
        }

        // Custom resolution
        std::uint8_t* CustomResolutionScanResult = Memory::PatternScan(exeModule, "44 89 ?? ?? 44 89 ?? ?? 8B ?? ?? 8D ?? ?? 89 ?? ?? 49 ?? ?? 49 83 ?? ??");
        if (CustomResolutionScanResult) {
            spdlog::info("Custom Resolution: Address is {:s}+{:x}", sExeName.c_str(), CustomResolutionScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid CustomResolutionMidHook{};
            CustomResolutionMidHook = safetyhook::create_mid(CustomResolutionScanResult,
                [](SafetyHookContext& ctx) {
                    if (!ctx.rbx) return;

                    // idx 5, 4, 3, 2, 1
                    // 7680x4320, 3840x2160, 2560x1440, 1920x1080, 1600x900, 1280x720
                    // 10080x4320, 5040x2160, 3360x1440, 2520x1080, 2100x900, 1680x720

                    // ctx.r15 = 16:9/21:9 (0/1)
                    // ctx.r13 = Windowed/Fullscreen (0/1)
                    // ctx.rdi = Resolution index

                    int iSupportedResIndex = *reinterpret_cast<int*>(ctx.rbx + 0x4);

                    // Replace highest supported resolution for both lists with custom resolution
                    if (ctx.rdi == iSupportedResIndex) {
                        ctx.r11 = iCustomResX;
                        ctx.r14 = iCustomResY;
                    }
                });
        }
        else {
            spdlog::error("Custom Resolution: Pattern scan failed.");
        }
    }
}

void IntroSkip()
{
    if (bIntroSkip)
    {
        // Intro skip
        std::uint8_t* IntroSkipScanResult = Memory::PatternScan(exeModule,"44 ?? ?? ?? 48 8B ?? 88 ?? ?? ?? ?? ?? 41 ?? ?? 03 74 ?? 41 ?? ?? 01");
        if (IntroSkipScanResult) {
            spdlog::info("Intro Skip: Address is {:s}+{:x}", sExeName.c_str(), IntroSkipScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid IntroSkipMidHook{};
            IntroSkipMidHook = safetyhook::create_mid(IntroSkipScanResult,
                [](SafetyHookContext& ctx) {
                    if (ctx.rcx && !bIntroSkipped) {
                        // Skip to decima logo
                        if ((ctx.rdx & 0xFF) == 0x01) // if SIELogo
                            ctx.rdx = (ctx.rdx & ~0xFF) | 0x06; // DecimaLogo

                        // Set show time
                        *reinterpret_cast<float*>(ctx.rcx + 0xE8) = 0.01f; // SIELogoShowTimer
                        *reinterpret_cast<float*>(ctx.rcx + 0xEC) = 0.01f; // KJPLogoShowTimer
                        *reinterpret_cast<float*>(ctx.rcx + 0xF0) = 0.01f; // DecimaLogoShowTimer
                        *reinterpret_cast<float*>(ctx.rcx + 0xF4) = 0.01f; // _505GamesLogoShowTimer

                        bIntroSkipped = true;
                        spdlog::info("Intro Skip: Skipped intro logos.");
                    }
                });
        }
        else {
            spdlog::error("Intro Skip: Pattern scan failed.");
        }
    }
}

void FOV()
{
    if (fGameplayFOVMulti != 1.00f)
    {
        // FOV: Gameplay
        std::uint8_t* GameplayFOVScanResult = Memory::PatternScan(exeModule, "48 8D ?? ?? ?? ?? ?? C5 7A ?? ?? ?? 48 8B ?? ?? ?? ?? ?? C5 FA ?? ?? ?? ?? ?? ?? C5 FA ?? ?? ?? ?? ?? ??");
        if (GameplayFOVScanResult) {
            spdlog::info("FOV: Gameplay: Address is {:s}+{:x}", sExeName.c_str(), GameplayFOVScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid GameplayFOVMidHook{};
            GameplayFOVMidHook = safetyhook::create_mid(GameplayFOVScanResult,
                [](SafetyHookContext& ctx) {
                    ctx.xmm11.f32[0] *= fGameplayFOVMulti;
                });
        }
        else {
            spdlog::error("FOV: Gameplay: Pattern scan failed.");
        }
    }

    if (bFixFOV && fAspectRatio < fNativeAspect)
    {
        // FOV: Global
        std::uint8_t* GlobalFOVScanResult = Memory::PatternScan(exeModule, "C5 F8 ?? ?? C5 F2 ?? ?? E8 ?? ?? ?? ?? C5 CA ?? ?? ?? ?? ?? ?? C5 F2 ?? ?? C5 FB ?? ?? E8 ?? ?? ?? ??");
        if (GlobalFOVScanResult) {
            spdlog::info("FOV: Global: Address is {:s}+{:x}", sExeName.c_str(), GlobalFOVScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid GlobalFOVMidHook{};
            GlobalFOVMidHook = safetyhook::create_mid(GlobalFOVScanResult,
                [](SafetyHookContext& ctx) {
                    if (fAspectRatio < fNativeAspect)
                        ctx.xmm0.f32[0] = fNativeAspect;
                });
        }
        else {
            spdlog::error("FOV: Global: Pattern scan failed.");
        }

        // FOV: Cuffs
        std::uint8_t* CuffsFOVScanResult = Memory::PatternScan(exeModule, "C5 F8 ?? ?? ?? ?? C5 F8 ?? ?? E8 ?? ?? ?? ?? C5 FA ?? ?? ?? ?? ?? ?? C5 EA ?? ?? C5 FA ?? ?? E8 ?? ?? ?? ??");
        if (CuffsFOVScanResult) {
            spdlog::info("FOV: Cuffs: Address is {:s}+{:x}", sExeName.c_str(), CuffsFOVScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid CuffsFOVMidHook{};
            CuffsFOVMidHook = safetyhook::create_mid(CuffsFOVScanResult,
                [](SafetyHookContext& ctx) {
                    if (fAspectRatio < fNativeAspect)
                        ctx.xmm1.f32[0] = fAspectRatio;
                });
        }
        else {
            spdlog::error("FOV: Cuffs: Pattern scan failed.");
        }
    }
}

void HUD()
{
    if (bFixHUD && fAspectRatio < fNativeAspect)
    {
        // TODO: Fix mouse at <16:9

        // HUD: Menus
        std::uint8_t* HUDMenusScanResult = Memory::PatternScan(exeModule, "C5 F8 ?? E8 ?? ?? ?? ?? 48 8B ?? ?? 48 8D ?? ?? ?? ?? ?? ?? 44 89 ?? ?? ?? ?? ?? ?? BE ?? ?? ?? ??");
        if (HUDMenusScanResult) {
            spdlog::info("HUD: Menus: Address is {:s}+{:x}", sExeName.c_str(), HUDMenusScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid HUDMenusMidHook{};
            HUDMenusMidHook = safetyhook::create_mid(HUDMenusScanResult,
                [](SafetyHookContext& ctx) {
                    if (fAspectRatio < fNativeAspect) {
                        float fHeightOffset = (ctx.xmm3.f32[0] - ctx.xmm5.f32[0]) / 2.00f;
                        float fWidthOffset = (ctx.xmm2.f32[0] - 1920.00f) / 2.00f;

                        *reinterpret_cast<float*>(ctx.rsp + 0x20) = fHeightOffset;
                        ctx.xmm1.f32[0] = fWidthOffset;
                        ctx.xmm2.f32[0] = 1920.00f;
                        ctx.xmm3.f32[0] = ctx.xmm5.f32[0] + fHeightOffset;
                    }
                });
        }
        else {
            spdlog::error("HUD: Menus: Pattern scan failed.");
        }
    }

    if (bCenterHUD)
    {
        // HUD: Ultrawide
        std::uint8_t* HUDUltrawideScanResult = Memory::PatternScan(exeModule, "C5 ?? ?? ?? C5 ?? ?? ?? ?? C4 ?? ?? ?? ?? ?? ?? C5 ?? ?? ?? ?? C5 ?? ?? ?? ?? 48 8D ?? ?? E8 ?? ?? ?? ??");
        if (HUDUltrawideScanResult) {
            spdlog::info("HUD: Ultrawide: Address is {:s}+{:x}", sExeName.c_str(), HUDUltrawideScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid HUDUltrawideMidHook{};
            HUDUltrawideMidHook = safetyhook::create_mid(HUDUltrawideScanResult,
                [](SafetyHookContext& ctx) {
                    //std::string sHUDObjName = *reinterpret_cast<char**>(ctx.rbx + 0x20);
                    if (fAspectRatio > fNativeAspect)
                        ctx.xmm6.f32[0] = 0.00f;
                });
        }
        else {
            spdlog::error("HUD: Ultrawide: Pattern scan failed.");
        }
    }
}

void Movies()
{
    if (bFixMovies)
    {
        // Movies
        std::uint8_t* MoviesScanResult = Memory::PatternScan(exeModule, "C5 FA ?? ?? ?? ?? E8 ?? ?? ?? ?? EB ?? C5 ?? ?? ?? ?? ?? ?? ?? 8B ?? ?? ?? 2B ?? ?? ??");
        if (MoviesScanResult) {
            spdlog::info("Movies: Address is {:s}+{:x}", sExeName.c_str(), MoviesScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid MoviesMidHook{};
            MoviesMidHook = safetyhook::create_mid(MoviesScanResult,
                [](SafetyHookContext& ctx) {
                    if (fAspectRatio > fNativeAspect) {
                        ctx.xmm1.f32[0] = fHUDWidthOffset;      // Left
                        ctx.xmm2.f32[0] = fHUDHeightOffset;     // Top
                        ctx.xmm3.f32[0] = fHUDWidth;            // Right
                        ctx.xmm0.f32[0] = fHUDHeight;           // Bottom
                    }
                });
        }
        else {
            spdlog::error("Movies: Pattern scan failed.");
        }
    }
}

void Graphics()
{
    if (bEntityUpdateRate)
    {
        // Fix camera stutter
        std::uint8_t* EntityUpdateRateScanResult = Memory::PatternScan(exeModule, "85 C0 74 ?? 41 ?? ?? 0F ?? ?? 8B ?? 2B ?? 41 ?? ?? ??");
        if (EntityUpdateRateScanResult) {
            spdlog::info("Entity Update Rate: Address is {:s}+{:x}", sExeName.c_str(), EntityUpdateRateScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            Memory::PatchBytes(EntityUpdateRateScanResult + 0x2, "\xEB", 1);
            spdlog::info("Entity Update Rate: Patched instruction.");
        }
        else {
            spdlog::error("Entity Update Rate: Pattern scan failed.");
        }
    }
}

// Changes what the settings menu sets the var to (unfinished)
// void FixFpsCap() {
//     std::uint8_t* scan = Memory::PatternScan(
//         exeModule,
//         "3B C1 0F 4F C1 89 05 ?? ?? ?? ?? 8B 4B 40"
//     );
//     if (scan) {
//         spdlog::info("FPS Cap Write: {:s}+{:x}", sExeName.c_str(),
//                      scan - reinterpret_cast<std::uint8_t*>(exeModule));
//
//         // Replace mov [mem],eax with mov eax,<cap>
//         uint8_t patch[6] =
//             {
//                 0xB8,                     // mov eax,imm32
//                 (uint8_t)(customCap),
//                 (uint8_t)(customCap >> 8),
//                 (uint8_t)(customCap >> 16),
//                 (uint8_t)(customCap >> 24),
//                 0x90                      // nop
//             };
//
//         Memory::PatchBytes(
//             scan + 5,
//             reinterpret_cast<const char*>(patch),
//             sizeof(patch)
//         );
//
//         spdlog::info("FPS Cap patched to {}", customCap);
//     }
//     else {
//         spdlog::error("FPS Cap pattern scan failed.");
//     }
// }

// changes the code that reads the cap
void FixFpsCap() {
    std::uint8_t* scan = Memory::PatternScan(
        exeModule,
        "8B 1D ? ? ? ? 44 0F B6 05 ? ? ? ? 8B 88 ?? ?? ?? ?? c7 80 ?? ?? ?? ?? ?? ?? ?? ?? 48 8b 15"
    );

    if (scan) {
        spdlog::info("FPS limit read at {:s}+{:x}",
                     sExeName.c_str(),
                     scan - reinterpret_cast<std::uint8_t*>(exeModule));

        uint8_t patch[6] =
            {
                0xBB,
                (uint8_t)(customCap),
                (uint8_t)(customCap >> 8),
                (uint8_t)(customCap >> 16),
                (uint8_t)(customCap >> 24),
                0x90
            };

        Memory::PatchBytes(scan, reinterpret_cast<const char*>(patch), sizeof(patch));

        spdlog::info("FPS limit overridden to {}", customCap);
    }
    else {
        spdlog::error("FPS limit pattern scan failed.");
    }
}

std::mutex mainThreadFinishedMutex;
std::condition_variable mainThreadFinishedVar;
bool mainThreadFinished = false;

DWORD __stdcall Main(void*)
{
    Logging();
    Configuration();
    Resolution();
    IntroSkip();
    FOV();
    HUD();
    Movies();
    Graphics();
    if (customCapEnabled) {
        FixFpsCap();
    }

    {
        std::lock_guard lock(mainThreadFinishedMutex);
        mainThreadFinished = true;
        mainThreadFinishedVar.notify_all();
    }

    return true;
}

std::mutex memsetHookMutex;
bool memsetHookCalled = false;
void* (__cdecl* memset_Fn)(void* Dst, int Val, size_t Size);
void* __cdecl memset_Hook(void* Dst, int Val, size_t Size)
{
    std::lock_guard lock(memsetHookMutex);
    if (!memsetHookCalled) {
        memsetHookCalled = true;
        Memory::HookIAT(exeModule, "VCRUNTIME140.dll", memset_Hook, memset_Fn);

        if (!mainThreadFinished) {
            std::unique_lock finishedLock(mainThreadFinishedMutex);
            mainThreadFinishedVar.wait(finishedLock, [] { return mainThreadFinished; });
        }
    }

    return memset_Fn(Dst, Val, Size);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        thisModule = hModule;

        HMODULE vcruntime140 = GetModuleHandleA("VCRUNTIME140.dll");
        if (vcruntime140) {
            memset_Fn = decltype(memset_Fn)(GetProcAddress(vcruntime140, "memset"));
            Memory::HookIAT(exeModule, "VCRUNTIME140.dll", memset_Fn, memset_Hook);
        }

        HANDLE mainHandle = CreateThread(NULL, 0, Main, 0, NULL, 0);
        if (mainHandle)
        {
            SetThreadPriority(mainHandle, THREAD_PRIORITY_HIGHEST);
            CloseHandle(mainHandle);
        }
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
