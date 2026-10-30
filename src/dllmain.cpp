#include "stdafx.h"
#include "helper.hpp"
#include "overlay.hpp"
#include "proxy/winhttp.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <inipp/inipp.h>
#include <safetyhook.hpp>
#include <imgui.h>

#include <format>

#define spdlog_confparse(var) spdlog::info("Config Parse: {}: {}", #var, var)

HMODULE exeModule = GetModuleHandle(NULL);
HMODULE thisModule;

// Fix details
std::string sFixName = "DeathStrandingDCFix";
std::string sFixVersion = FIX_VERSION; // set by CMake from the git tag (cmake/FixVersion.cmake)
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
bool bFixMovies;
bool bFixFOV;
bool bFixHUD;
float fGameplayFOVMulti;
float fLODDistance;
bool bCenterHUD;
bool bEntityUpdateRate;
bool bOverlay = true;
bool bOverlayTransparency = false;
bool bOverlayInputPassthrough = false;
int iIniCustomResX; // Custom resolution as written in the ini (iCustomResX/Y get resolved to the desktop resolution)
int iIniCustomResY;

// Variables
int iCurrentResX;
int iCurrentResY;
bool bIntroSkipped = false;
std::uint8_t* EntityUpdateRateAddress = nullptr;
std::uint8_t EntityUpdateRateOriginalByte = 0;

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
        if (HMODULE realWinHttp = Proxy::GetRealModule()) {
            WCHAR realWinHttpPath[_MAX_PATH] = {0};
            GetModuleFileNameW(realWinHttp, realWinHttpPath, MAX_PATH);
            spdlog::info("Proxy: Forwarding winhttp.dll to {}", std::filesystem::path(realWinHttpPath).string());
        }
        else {
            spdlog::error("Proxy: Failed to load the system winhttp.dll, winhttp calls will fail.");
        }
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
    inipp::get_value(ini.sections["Overlay"], "Enabled", bOverlay);
    inipp::get_value(ini.sections["Overlay"], "Transparency", bOverlayTransparency);
    inipp::get_value(ini.sections["Overlay"], "InputPassthrough", bOverlayInputPassthrough);

    // Clamp settings
    fGameplayFOVMulti = std::clamp(fGameplayFOVMulti, 0.10f, 2.00f);
    fLODDistance = std::clamp(fLODDistance, 0.00f, 5.00f);
    iIniCustomResX = iCustomResX;
    iIniCustomResY = iCustomResY;

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
    spdlog_confparse(bOverlay);
    spdlog_confparse(bOverlayTransparency);
    spdlog_confparse(bOverlayInputPassthrough);

    spdlog::info("----------");
}

bool SaveConfig()
{
    auto BoolStr = [](bool b) { return std::string(b ? "true" : "false"); };
    struct IniValue { std::string section; std::string key; std::string value; bool bWritten = false; };
    std::vector<IniValue> values = {
        { "Intro Skip", "Enabled", BoolStr(bIntroSkip) },
        { "Custom Resolution", "Enabled", BoolStr(bCustomRes) },
        { "Custom Resolution", "Width", std::to_string(iIniCustomResX) },
        { "Custom Resolution", "Height", std::to_string(iIniCustomResY) },
        { "Force Entity Updates", "Enabled", BoolStr(bEntityUpdateRate) },
        { "Gameplay FOV", "Multiplier", std::format("{:.2f}", fGameplayFOVMulti) },
        { "Center HUD", "Enabled", BoolStr(bCenterHUD) },
        { "Fix Movies", "Enabled", BoolStr(bFixMovies) },
        { "Fix FOV", "Enabled", BoolStr(bFixFOV) },
        { "Fix HUD", "Enabled", BoolStr(bFixHUD) },
        { "Overlay", "Enabled", BoolStr(bOverlay) },
        { "Overlay", "Transparency", BoolStr(bOverlayTransparency) },
        { "Overlay", "InputPassthrough", BoolStr(bOverlayInputPassthrough) },
    };

    auto Trim = [](const std::string& str) {
        size_t start = str.find_first_not_of(" \t\r");
        size_t end = str.find_last_not_of(" \t\r");
        return start == std::string::npos ? std::string() : str.substr(start, end - start + 1);
    };

    // Read the existing ini so comments and layout are kept, only values are replaced
    std::vector<std::string> lines;
    {
        std::ifstream iniFile(FixPath / sConfigFile, std::ios::binary);
        std::string line;
        while (std::getline(iniFile, line))
            lines.push_back(line);
    }

    std::string section;
    for (std::string& line : lines) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
            continue;
        if (trimmed[0] == '[') {
            section = Trim(trimmed.substr(1, trimmed.find(']') - 1));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Trim(line.substr(0, eq));
        for (IniValue& value : values) {
            if (value.section == section && value.key == key) {
                bool bCR = line.ends_with('\r');
                line = line.substr(0, eq + 1) + " " + value.value + (bCR ? "\r" : "");
                value.bWritten = true;
            }
        }
    }

    // Append anything missing from the file (e.g. sections added in newer versions)
    std::string lastAppendedSection;
    for (const IniValue& value : values) {
        if (value.bWritten)
            continue;
        if (value.section != lastAppendedSection) {
            lines.push_back("");
            lines.push_back("[" + value.section + "]");
            lastAppendedSection = value.section;
        }
        lines.push_back(value.key + " = " + value.value);
    }

    std::ofstream iniFile(FixPath / sConfigFile, std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines)
        iniFile << line << "\n";
    iniFile.close();

    if (!iniFile) {
        spdlog::error("Config Save: Failed to write {}", FixPath.string() + sConfigFile);
        return false;
    }
    spdlog::info("Config Save: Saved settings to {}", FixPath.string() + sConfigFile);
    return true;
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
    // Hooks are always installed and check their settings at runtime so they can be changed from the overlay.
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

    {
        // FOV: Global
        std::uint8_t* GlobalFOVScanResult = Memory::PatternScan(exeModule, "C5 F8 ?? ?? C5 F2 ?? ?? E8 ?? ?? ?? ?? C5 CA ?? ?? ?? ?? ?? ?? C5 F2 ?? ?? C5 FB ?? ?? E8 ?? ?? ?? ??");
        if (GlobalFOVScanResult) {
            spdlog::info("FOV: Global: Address is {:s}+{:x}", sExeName.c_str(), GlobalFOVScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid GlobalFOVMidHook{};
            GlobalFOVMidHook = safetyhook::create_mid(GlobalFOVScanResult,
                [](SafetyHookContext& ctx) {
                    if (bFixFOV && fAspectRatio < fNativeAspect)
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
                    if (bFixFOV && fAspectRatio < fNativeAspect)
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
    {
        // TODO: Fix mouse at <16:9

        // HUD: Menus
        std::uint8_t* HUDMenusScanResult = Memory::PatternScan(exeModule, "C5 F8 ?? E8 ?? ?? ?? ?? 48 8B ?? ?? 48 8D ?? ?? ?? ?? ?? ?? 44 89 ?? ?? ?? ?? ?? ?? BE ?? ?? ?? ??");
        if (HUDMenusScanResult) {
            spdlog::info("HUD: Menus: Address is {:s}+{:x}", sExeName.c_str(), HUDMenusScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid HUDMenusMidHook{};
            HUDMenusMidHook = safetyhook::create_mid(HUDMenusScanResult,
                [](SafetyHookContext& ctx) {
                    if (bFixHUD && fAspectRatio < fNativeAspect) {
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

    {
        // HUD: Ultrawide
        std::uint8_t* HUDUltrawideScanResult = Memory::PatternScan(exeModule, "C5 ?? ?? ?? C5 ?? ?? ?? ?? C4 ?? ?? ?? ?? ?? ?? C5 ?? ?? ?? ?? C5 ?? ?? ?? ?? 48 8D ?? ?? E8 ?? ?? ?? ??");
        if (HUDUltrawideScanResult) {
            spdlog::info("HUD: Ultrawide: Address is {:s}+{:x}", sExeName.c_str(), HUDUltrawideScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid HUDUltrawideMidHook{};
            HUDUltrawideMidHook = safetyhook::create_mid(HUDUltrawideScanResult,
                [](SafetyHookContext& ctx) {
                    //std::string sHUDObjName = *reinterpret_cast<char**>(ctx.rbx + 0x20);
                    if (bCenterHUD && fAspectRatio > fNativeAspect)
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
    {
        // Movies
        std::uint8_t* MoviesScanResult = Memory::PatternScan(exeModule, "C5 FA ?? ?? ?? ?? E8 ?? ?? ?? ?? EB ?? C5 ?? ?? ?? ?? ?? ?? ?? 8B ?? ?? ?? 2B ?? ?? ??");
        if (MoviesScanResult) {
            spdlog::info("Movies: Address is {:s}+{:x}", sExeName.c_str(), MoviesScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
            static SafetyHookMid MoviesMidHook{};
            MoviesMidHook = safetyhook::create_mid(MoviesScanResult,
                [](SafetyHookContext& ctx) {
                    if (bFixMovies && fAspectRatio > fNativeAspect) {
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

void ApplyEntityUpdateRate()
{
    if (!EntityUpdateRateAddress)
        return;

    // jz -> jmp to force updates, or restore the original jz
    Memory::PatchBytes(EntityUpdateRateAddress, bEntityUpdateRate ? "\xEB" : reinterpret_cast<const char*>(&EntityUpdateRateOriginalByte), 1);
    spdlog::info("Entity Update Rate: {} instruction.", bEntityUpdateRate ? "Patched" : "Restored");
}

void Graphics()
{
    // Fix camera stutter
    std::uint8_t* EntityUpdateRateScanResult = Memory::PatternScan(exeModule, "85 C0 74 ?? 41 ?? ?? 0F ?? ?? 8B ?? 2B ?? 41 ?? ?? ??");
    if (EntityUpdateRateScanResult) {
        spdlog::info("Entity Update Rate: Address is {:s}+{:x}", sExeName.c_str(), EntityUpdateRateScanResult - reinterpret_cast<std::uint8_t*>(exeModule));
        EntityUpdateRateAddress = EntityUpdateRateScanResult + 0x2;
        EntityUpdateRateOriginalByte = *EntityUpdateRateAddress;
        if (bEntityUpdateRate)
            ApplyEntityUpdateRate();
    }
    else {
        spdlog::error("Entity Update Rate: Pattern scan failed.");
    }
}

// Menu layout follows REFramework's main window (REFramework::draw_ui)
void DrawMenu()
{
    static std::string sSaveStatus;
    static bool bSaveSucceeded = false;
    static double fSaveStatusTime = 0.0;

    auto HelpMarker = [](const char* text) {
        ImGui::SameLine();
        ImGui::Text("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", text);
    };
    auto RestartNote = []() {
        ImGui::SameLine();
        ImGui::TextDisabled("(restart required)");
    };

    // Transparency: faded when the menu isn't focused (REFramework's values)
    ImGuiStyle& style = ImGui::GetStyle();
    if (bOverlayTransparency) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
            style.Alpha = 1.00f;
        else if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
            style.Alpha = 0.90f;
        else
            style.Alpha = 0.80f;
    }
    else {
        style.Alpha = 1.00f;
    }

    const float fScale = style.FontScaleDpi;
    ImGui::SetNextWindowPos(ImVec2(50.00f, 50.00f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300.00f * fScale, 500.00f * fScale), ImGuiCond_FirstUseEver);

    static const std::string sTitle = sFixName + " v" + sFixVersion;
    bool bOpen = true;
    ImGui::Begin(sTitle.c_str(), &bOpen);

    ImGui::Text("Menu Key: Home");
    ImGui::Checkbox("Transparency", &bOverlayTransparency);
    HelpMarker("Makes the UI transparent when not focused.");
    if (ImGui::Checkbox("Input Passthrough", &bOverlayInputPassthrough))
        Overlay::SetInputPassthrough(bOverlayInputPassthrough);
    HelpMarker("Allows mouse and keyboard inputs to register to the game while the UI is open.");

    if (ImGui::Button("Save Settings")) {
        bSaveSucceeded = SaveConfig();
        sSaveStatus = bSaveSucceeded ? "Saved to " + sConfigFile : "Failed to save, see " + sLogFile;
        fSaveStatusTime = ImGui::GetTime();
    }
    if (!sSaveStatus.empty() && ImGui::GetTime() - fSaveStatusTime < 2.0) {
        ImGui::SameLine();
        ImGui::TextColored(bSaveSucceeded ? ImVec4(0.40f, 0.90f, 0.40f, 1.00f) : ImVec4(1.00f, 0.40f, 0.40f, 1.00f), "%s", sSaveStatus.c_str());
    }

    if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TreePush("General");
        ImGui::SliderFloat("Gameplay FOV", &fGameplayFOVMulti, 0.10f, 2.00f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        HelpMarker("Multiplier for the gameplay FOV. Ctrl+Click to type a value.");
        if (ImGui::Checkbox("Force Entity Updates", &bEntityUpdateRate))
            ApplyEntityUpdateRate();
        HelpMarker("Forces certain game entities to update every frame. This can smooth out perceived camera stutter.");
        ImGui::Checkbox("Intro Skip", &bIntroSkip);
        HelpMarker("Skips company logos on game startup.");
        RestartNote();
        ImGui::Checkbox("Custom Resolution", &bCustomRes);
        HelpMarker("Replaces the highest resolution option for 16:9/21:9 in-game with your custom resolution. Leave at 0 to use your desktop resolution.");
        RestartNote();
        ImGui::BeginDisabled(!bCustomRes);
        if (ImGui::InputInt("Width", &iIniCustomResX, 0))
            iIniCustomResX = std::max(iIniCustomResX, 0);
        if (ImGui::InputInt("Height", &iIniCustomResY, 0))
            iIniCustomResY = std::max(iIniCustomResY, 0);
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    if (ImGui::CollapsingHeader("Ultrawide/Narrower", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TreePush("Ultrawide/Narrower");
        ImGui::Checkbox("Center HUD", &bCenterHUD);
        HelpMarker("Adjusts the gameplay HUD to 16:9. (Ultrawide-only)");
        ImGui::Checkbox("Fix Movies", &bFixMovies);
        HelpMarker("Fixes cropped movies when using an ultrawide resolution.");
        ImGui::Checkbox("Fix FOV", &bFixFOV);
        HelpMarker("Fixes cropped FOV when using a narrower than 16:9 resolution.");
        ImGui::Checkbox("Fix HUD", &bFixHUD);
        HelpMarker("Fixes cropped HUD when using a narrower than 16:9 resolution.");
        ImGui::TreePop();
    }

    if (ImGui::CollapsingHeader("About")) {
        ImGui::TreePush("About");
        ImGui::Text("%s v%s", sFixName.c_str(), sFixVersion.c_str());
        ImGui::Text("Menu style and D3D12 hooking based on REFramework by praydog.");
        ImGui::Text("https://github.com/praydog/REFramework");
        if (ImGui::TreeNode("Licenses")) {
            ImGui::BulletText("REFramework - MIT");
            ImGui::BulletText("Dear ImGui - MIT");
            ImGui::BulletText("safetyhook - BSL-1.0");
            ImGui::BulletText("spdlog - MIT");
            ImGui::BulletText("inipp - MIT");
            ImGui::BulletText("Roboto font - Apache-2.0");
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    ImGui::End();

    // Close button (X) on the title bar
    if (!bOpen)
        Overlay::Close();
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

    if (bOverlay) {
        Overlay::SetInputPassthrough(bOverlayInputPassthrough);
        Overlay::Init(DrawMenu);
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
        Memory::HookIAT(exeModule, "VCRUNTIME140.dll", (void*)memset_Hook, (void*)memset_Fn);

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
        Proxy::Init(hModule);

        HMODULE vcruntime140 = GetModuleHandleA("VCRUNTIME140.dll");
        if (vcruntime140) {
            memset_Fn = decltype(memset_Fn)(GetProcAddress(vcruntime140, "memset"));
            Memory::HookIAT(exeModule, "VCRUNTIME140.dll", (void*)memset_Fn, (void*)memset_Hook);
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