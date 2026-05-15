// SpreadControlVC.asi - Weapon spread control for GTA Vice City
// Forces all peds (player + NPCs) through the RNG spread calculation
// and removes the skill-accumulator bug that zeros out spread on first shots

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define SPREADCONTROLVC_VERSION "1.0.0"

// --- Patch addresses (NPC path in FUN_005d1140) ---
const DWORD ADDR_PED_TYPE_CHECK    = 0x005D13BD; // JNZ -> skips NPCs (ped type != 3)
const DWORD ADDR_FLAG_CHECK        = 0x005D13DA; // JZ  -> skips when flag bit 0 not set
const DWORD ADDR_ACCURACY_READ     = 0x005D13E0; // MOV AL, [EBX+0x506]
const DWORD ADDR_LOCAL_PLAYER_CHECK= 0x005D1534; // JNZ -> skips non-local player
const DWORD ADDR_SKILL_MULT        = 0x005D1589; // FLD [2.5]; FMUL [skill]; FMUL [spread]

// --- Patch addresses (Player spread constants in FUN_005cbff0) ---
// These are .data section floats used as weapon-specific spread multipliers.
// The formula in FUN_005cbff0: angle_offset = rng_val * spread_constant
// where rng_val = (rand() & 0x7F) - 0x40  (range -64 to 63)
const DWORD ADDR_PLAYER_SPREAD_DEFAULT  = 0x0069D3A8; // used by most instant-hit weapon types
const DWORD ADDR_PLAYER_SPREAD_WEAPON_B = 0x0069D440; // used by weapon type 0x1B
const DWORD ADDR_PLAYER_SPREAD_WEAPON_A = 0x0069D444; // used by weapon types 0x1A, 0x20, 0x23

// Final instant-hit trace call in FUN_005d1140.
// This is a diagnostic/override hook that offsets the final target point after
// the game has finished all normal aim and spread calculations.
const DWORD ADDR_FINAL_TRACE_CALL = 0x005D2CAB;
const DWORD ADDR_PLAYER_TRACE_CALL = 0x005CC1F1;
const DWORD ADDR_FINAL_TRACE_FUNC = 0x005CEE60;

// Wanted-level crime accumulation routine. Returning before this function runs
// prevents crimes from adding wanted points/stars.
const DWORD ADDR_REGISTER_CRIME = 0x004D1610;
const DWORD ADDR_UPDATE_WANTED_LEVEL = 0x004D2110;

// Reserve/total ammo decrement in CWeapon::Fire. The nearby clip decrement
// remains intact so weapons still reload when the current magazine empties.
const DWORD ADDR_TOTAL_AMMO_DECREMENT = 0x005D4AF5;

// --- Patch data ---
BYTE g_NOP6[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
BYTE g_NOP3[3] = { 0x90, 0x90, 0x90 };

// Accuracy read patch: MOV AL, XX; NOP NOP NOP NOP
BYTE g_AccuracyPatch[6] = { 0xB0, 60, 0x90, 0x90, 0x90, 0x90 };
BYTE g_Return12Patch[6] = { 0xC2, 0x0C, 0x00, 0x90, 0x90, 0x90 };

// Global config values
float g_SpreadMultiplier = 1.0f;  // NPC spread intensity
float g_PlayerSpread     = 0.003f; // Player spread constant (~11 deg max at default)
float g_SpreadDegrees    = 2.0f;   // angular spread applied to final trace rays
float g_MinSpreadUnits   = 0.35f;  // minimum lateral spread for very short traces
float g_NoTargetDistance = 120.0f; // synthetic ray length when the game has no hit target
bool g_PoliceIgnore      = false;  // prevent crimes from increasing wanted stars
bool g_InfiniteAmmo      = true;   // keep reserve ammo from decreasing
DWORD g_RandomState      = 0;
int g_TraceLogLimit      = 40;
int g_TraceLogCount      = 0;
HMODULE g_Module = NULL;
char g_LogPath[MAX_PATH] = { 0 };

// Replacement for the skill-multiplication section (20 bytes):
// Original: FLD [2.5]; FMUL [skill]; FMUL [spread]; FSTP [spread]
// Replaces with: FLD [g_SpreadMultiplier]; FMUL [spread]; (NOPs); FSTP [spread]
BYTE g_SkillMultReplacement[20];

void BuildSkillMultReplacement() {
    // FLD dword ptr [g_SpreadMultiplier]
    g_SkillMultReplacement[0] = 0xD9;
    g_SkillMultReplacement[1] = 0x05;
    *(DWORD*)&g_SkillMultReplacement[2] = (DWORD)&g_SpreadMultiplier;

    // FMUL dword ptr [ESP + 0x0C]  (local_2d4 in Ghidra)
    g_SkillMultReplacement[6] = 0xD8;
    g_SkillMultReplacement[7] = 0x4C;
    g_SkillMultReplacement[8] = 0x24;
    g_SkillMultReplacement[9] = 0x0C;

    // 6 NOPs (padding)
    for (int i = 10; i < 16; i++)
        g_SkillMultReplacement[i] = 0x90;

    // FSTP dword ptr [ESP + 0x0C]
    g_SkillMultReplacement[16] = 0xD9;
    g_SkillMultReplacement[17] = 0x5C;
    g_SkillMultReplacement[18] = 0x24;
    g_SkillMultReplacement[19] = 0x0C;
}

void Log(const char* fmt, ...) {
    FILE* file = fopen(g_LogPath, "a");
    if (!file) {
        char tempPath[MAX_PATH] = { 0 };
        DWORD len = GetTempPathA(MAX_PATH, tempPath);
        if (len == 0 || len >= MAX_PATH)
            return;

        strncat(tempPath, "SpreadControlVC.log", MAX_PATH - strlen(tempPath) - 1);
        file = fopen(tempPath, "a");
        if (!file)
            return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

bool BytesMatch(DWORD addr, const BYTE* expected, size_t len) {
    return memcmp((const void*)addr, expected, len) == 0;
}

bool ApplyPatch(DWORD addr, const BYTE* expected, BYTE* data, size_t len, const char* name) {
    if (!BytesMatch(addr, expected, len)) {
        Log("SKIP %s at 0x%08X: original bytes did not match this executable.", name, addr);
        return false;
    }

    DWORD oldProtect = 0;
    VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((LPVOID)addr, data, len);
    VirtualProtect((LPVOID)addr, len, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, len);
    Log("Applied %s at 0x%08X.", name, addr);
    return true;
}

bool ApplyCallPatch(DWORD addr, const BYTE* expected, void* target, const char* name) {
    if (!BytesMatch(addr, expected, 5)) {
        Log("SKIP %s at 0x%08X: original call bytes did not match this executable.", name, addr);
        return false;
    }

    BYTE patch[5] = { 0xE8, 0, 0, 0, 0 };
    *(DWORD*)&patch[1] = (DWORD)target - (addr + 5);
    return ApplyPatch(addr, expected, patch, sizeof(patch), name);
}

bool ApplyJumpPatch6(DWORD addr, const BYTE* expected, void* target, const char* name) {
    if (!BytesMatch(addr, expected, 6)) {
        Log("SKIP %s at 0x%08X: original bytes did not match this executable.", name, addr);
        return false;
    }

    BYTE patch[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(DWORD*)&patch[1] = (DWORD)target - (addr + 5);
    return ApplyPatch(addr, expected, patch, sizeof(patch), name);
}

bool WriteFloat(DWORD addr, float expected, float val, const char* name) {
    if (*(float*)addr != expected) {
        Log("SKIP %s at 0x%08X: expected %.8f, found %.8f.", name, addr, expected, *(float*)addr);
        return false;
    }

    DWORD oldProtect = 0;
    VirtualProtect((LPVOID)addr, sizeof(float), PAGE_READWRITE, &oldProtect);
    *(float*)addr = val;
    VirtualProtect((LPVOID)addr, sizeof(float), oldProtect, &oldProtect);
    Log("Wrote %s at 0x%08X: %.8f -> %.8f.", name, addr, expected, val);
    return true;
}

DWORD NextRandom() {
    g_RandomState = g_RandomState * 1664525u + 1013904223u;
    return g_RandomState;
}

float RandomSignedUnit() {
    return ((float)(NextRandom() & 0xffff) / 32767.5f) - 1.0f;
}

float Dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool Normalize3(float* v) {
    float lengthSquared = Dot3(v, v);
    if (lengthSquared <= 0.0001f)
        return false;

    float invLength = 1.0f / (float)sqrt(lengthSquared);
    v[0] *= invLength;
    v[1] *= invLength;
    v[2] *= invLength;
    return true;
}

void Cross3(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool LooksLikeVector3(const float* v) {
    if (!v)
        return false;

    MEMORY_BASIC_INFORMATION mbi = { 0 };
    if (!VirtualQuery(v, &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD))
        return false;

    if ((SIZE_T)((const BYTE*)mbi.BaseAddress + mbi.RegionSize - (const BYTE*)v) < sizeof(float) * 3)
        return false;

    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

bool LooksLikeDirectionVector(const float* v) {
    if (!LooksLikeVector3(v))
        return false;

    float lenSq = Dot3(v, v);
    return lenSq > 0.25f && lenSq < 4.0f && fabs(v[0]) <= 2.0f && fabs(v[1]) <= 2.0f && fabs(v[2]) <= 2.0f;
}

void ApplyDirectionSpread(float* direction, DWORD callReturn) {
    if (!LooksLikeDirectionVector(direction) || g_SpreadDegrees <= 0.0f)
        return;

    float originalLength = (float)sqrt(Dot3(direction, direction));
    float dir[3] = { direction[0], direction[1], direction[2] };
    if (!Normalize3(dir))
        return;

    float up[3] = { 0.0f, 0.0f, 1.0f };
    if (fabs(Dot3(dir, up)) > 0.95f) {
        up[0] = 0.0f;
        up[1] = 1.0f;
        up[2] = 0.0f;
    }

    float right[3] = { 0.0f, 0.0f, 0.0f };
    Cross3(dir, up, right);
    if (!Normalize3(right))
        return;

    Cross3(right, dir, up);
    if (!Normalize3(up))
        return;

    g_RandomState ^= (DWORD)direction ^ callReturn;

    float maxOffset = (float)tan(g_SpreadDegrees * 0.01745329252f);
    float offsetX = RandomSignedUnit() * maxOffset;
    float offsetY = RandomSignedUnit() * maxOffset;

    float spreadDirection[3] = {
        dir[0] + right[0] * offsetX + up[0] * offsetY,
        dir[1] + right[1] * offsetX + up[1] * offsetY,
        dir[2] + right[2] * offsetX + up[2] * offsetY
    };
    if (!Normalize3(spreadDirection))
        return;

    direction[0] = spreadDirection[0] * originalLength;
    direction[1] = spreadDirection[1] * originalLength;
    direction[2] = spreadDirection[2] * originalLength;
}

void ApplyEndpointSpread(float* start, float* end, DWORD callReturn) {
    if (!LooksLikeVector3(start) || !LooksLikeVector3(end) || g_SpreadDegrees <= 0.0f)
        return;

    float direction[3] = {
        end[0] - start[0],
        end[1] - start[1],
        end[2] - start[2]
    };
    float distanceSquared = Dot3(direction, direction);
    if (distanceSquared <= 0.0001f || distanceSquared > 1000000.0f)
        return;

    float distance = (float)sqrt(distanceSquared);
    direction[0] /= distance;
    direction[1] /= distance;
    direction[2] /= distance;

    float up[3] = { 0.0f, 0.0f, 1.0f };
    if (fabs(Dot3(direction, up)) > 0.95f) {
        up[0] = 0.0f;
        up[1] = 1.0f;
        up[2] = 0.0f;
    }

    float right[3] = { 0.0f, 0.0f, 0.0f };
    Cross3(direction, up, right);
    if (!Normalize3(right))
        return;

    Cross3(right, direction, up);
    if (!Normalize3(up))
        return;

    g_RandomState ^= (DWORD)end ^ callReturn;

    float angularOffset = (float)tan(g_SpreadDegrees * 0.01745329252f);
    float minOffset = distance > 0.0f ? g_MinSpreadUnits / distance : 0.0f;
    float maxOffset = angularOffset > minOffset ? angularOffset : minOffset;
    if (maxOffset > 0.75f)
        maxOffset = 0.75f;

    float offsetX = RandomSignedUnit() * maxOffset;
    float offsetY = RandomSignedUnit() * maxOffset;

    float spreadDirection[3] = {
        direction[0] + right[0] * offsetX + up[0] * offsetY,
        direction[1] + right[1] * offsetX + up[1] * offsetY,
        direction[2] + right[2] * offsetX + up[2] * offsetY
    };
    if (!Normalize3(spreadDirection))
        return;

    end[0] = start[0] + spreadDirection[0] * distance;
    end[1] = start[1] + spreadDirection[1] * distance;
    end[2] = start[2] + spreadDirection[2] * distance;
}

bool IsZeroVector(const float* v) {
    return LooksLikeVector3(v) && fabs(v[0]) < 0.0001f && fabs(v[1]) < 0.0001f && fabs(v[2]) < 0.0001f;
}

bool BuildNoTargetEndpoint(float* start, float* end, DWORD* arg6Slot, DWORD* arg7Slot) {
    if (!LooksLikeVector3(start) || !LooksLikeVector3(end) || !IsZeroVector(end) || g_NoTargetDistance <= 1.0f)
        return false;

    float direction[3] = { 0.0f, 0.0f, 0.0f };
    if (arg6Slot && arg7Slot) {
        float dirX = *(float*)arg6Slot;
        float dirY = *(float*)arg7Slot;
        if (!isfinite(dirX) || !isfinite(dirY))
            return false;

        direction[0] = dirX;
        direction[1] = dirY;
        direction[2] = 0.0f;
    } else {
        return false;
    }

    if (!Normalize3(direction))
        return false;

    end[0] = start[0] + direction[0] * g_NoTargetDistance;
    end[1] = start[1] + direction[1] * g_NoTargetDistance;
    end[2] = start[2] + direction[2] * g_NoTargetDistance;
    return true;
}

extern "C" void __stdcall ApplyTraceSpread(DWORD callReturn, DWORD arg1, DWORD arg2, DWORD arg3, float* arg4, float* arg5, DWORD* arg6Slot, DWORD* arg7Slot) {
    if (g_SpreadDegrees <= 0.0f)
        return;

    float* arg3Vector = (float*)arg3;
    float oldArg3[3] = { 0.0f, 0.0f, 0.0f };
    float oldArg4[3] = { 0.0f, 0.0f, 0.0f };
    float oldArg5[3] = { 0.0f, 0.0f, 0.0f };
    bool arg3Valid = LooksLikeVector3(arg3Vector);
    bool arg4Valid = LooksLikeVector3(arg4);
    bool arg5Valid = LooksLikeVector3(arg5);

    if (arg3Valid) {
        oldArg3[0] = arg3Vector[0];
        oldArg3[1] = arg3Vector[1];
        oldArg3[2] = arg3Vector[2];
    }

    if (arg4Valid) {
        oldArg4[0] = arg4[0];
        oldArg4[1] = arg4[1];
        oldArg4[2] = arg4[2];
    }

    if (arg5Valid) {
        oldArg5[0] = arg5[0];
        oldArg5[1] = arg5[1];
        oldArg5[2] = arg5[2];
    }

    if (LooksLikeDirectionVector(arg4))
        ApplyDirectionSpread(arg4, callReturn);

    bool noTarget = (arg2 == 0) || (arg5Valid && IsZeroVector(arg5));
    bool synthesizedTarget = false;

    if (arg3Valid && arg4Valid && noTarget) {
        ApplyEndpointSpread(arg3Vector, arg4, callReturn);
    } else if (noTarget) {
        synthesizedTarget = BuildNoTargetEndpoint(arg4, arg5, arg6Slot, arg7Slot);
        if (arg4Valid && arg5Valid && synthesizedTarget && !LooksLikeDirectionVector(arg5))
            ApplyEndpointSpread(arg4, arg5, callReturn);
    } else if (arg4Valid && arg5Valid && !LooksLikeDirectionVector(arg5)) {
        ApplyEndpointSpread(arg4, arg5, callReturn);
    }

    if (g_TraceLogCount < g_TraceLogLimit) {
        ++g_TraceLogCount;
        Log("Trace %d site=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X arg3=(%.2f,%.2f,%.2f)->(%.2f,%.2f,%.2f) arg4=(%.2f,%.2f,%.2f)->(%.2f,%.2f,%.2f) arg5=(%.2f,%.2f,%.2f)->(%.2f,%.2f,%.2f) arg6=%.4f arg7=%.4f noTarget=%d synth=%d spreadDeg=%.2f.",
            g_TraceLogCount,
            callReturn,
            arg1, arg2, arg3,
            oldArg3[0], oldArg3[1], oldArg3[2],
            arg3Valid ? arg3Vector[0] : 0.0f, arg3Valid ? arg3Vector[1] : 0.0f, arg3Valid ? arg3Vector[2] : 0.0f,
            oldArg4[0], oldArg4[1], oldArg4[2],
            arg4Valid ? arg4[0] : 0.0f, arg4Valid ? arg4[1] : 0.0f, arg4Valid ? arg4[2] : 0.0f,
            oldArg5[0], oldArg5[1], oldArg5[2],
            arg5Valid ? arg5[0] : 0.0f, arg5Valid ? arg5[1] : 0.0f, arg5Valid ? arg5[2] : 0.0f,
            arg6Slot ? *(float*)arg6Slot : 0.0f,
            arg7Slot ? *(float*)arg7Slot : 0.0f,
            noTarget ? 1 : 0,
            synthesizedTarget ? 1 : 0,
            g_SpreadDegrees);
    }
}

extern "C" __declspec(naked) void FinalTraceWrapper() {
    __asm {
        pushad
        // Original stack after pushad:
        // [esp+32] = return address, [esp+36..60] = trace args 1..7.
        // Push right-to-left. After each push, the next original value is still
        // at [esp+60], so this remains stable without scratch storage.
        lea eax, [esp + 60]
        push eax
        lea eax, [esp + 60]
        push eax
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        push dword ptr [esp + 60]
        call ApplyTraceSpread
        popad
        jmp ADDR_FINAL_TRACE_FUNC
    }
}

extern "C" __declspec(naked) void NoWantedUpdateWrapper() {
    __asm {
        mov dword ptr [ecx], 0
        mov dword ptr [ecx + 0x20], 0
        mov byte ptr [ecx + 0x1A], 0
        mov byte ptr [ecx + 0x19], 0
        mov word ptr [ecx + 0x1C], 0
        ret
    }
}

void BuildPaths(char* iniPath, size_t iniPathLen) {
    GetModuleFileNameA(g_Module, iniPath, (DWORD)iniPathLen);
    char* ext = strrchr(iniPath, '.');
    if (ext)
        strcpy(ext, ".ini");
    else
        iniPath[0] = '\0';

    GetModuleFileNameA(g_Module, g_LogPath, MAX_PATH);
    ext = strrchr(g_LogPath, '.');
    if (ext)
        strcpy(ext, ".log");
    else
        g_LogPath[0] = '\0';
}

void LoadConfig() {
    char buf[32] = { 0 };
    char iniPath[MAX_PATH] = { 0 };
    BuildPaths(iniPath, sizeof(iniPath));
    if (!iniPath[0])
        return;

    // NPC accuracy value (0-100). Lower = more spread.
    GetPrivateProfileStringA("Config", "Accuracy", "60", buf, sizeof(buf), iniPath);
    int val = atoi(buf);
    if (val >= 0 && val <= 100) g_AccuracyPatch[1] = (BYTE)val;

    // NPC spread multiplier
    GetPrivateProfileStringA("Config", "SpreadMultiplier", "1.0", buf, sizeof(buf), iniPath);
    g_SpreadMultiplier = (float)atof(buf);
    if (g_SpreadMultiplier < 0.0f) g_SpreadMultiplier = 0.0f;

    // Player spread constant. Higher = more spray for the player.
    // The player's spread formula: angle_offset = (rand_float) * PlayerSpread
    // where rand_float ranges from -64 to +63 (unitless, scaled by PlayerSpread).
    // Suggested values:
    //   0.001 = ~3.7 deg max spread (very tight)
    //   0.003 = ~11 deg max spread (moderate, default)
    //   0.005 = ~18 deg max spread (noticeable spray)
    //   0.010 = ~37 deg max spread (heavy spray)
    //   0.020 = ~73 deg max spread (extreme)
    GetPrivateProfileStringA("Config", "PlayerSpread", "0.003", buf, sizeof(buf), iniPath);
    g_PlayerSpread = (float)atof(buf);
    if (g_PlayerSpread < 0.0f) g_PlayerSpread = 0.0f;

    GetPrivateProfileStringA("Config", "SpreadDegrees", "2.0", buf, sizeof(buf), iniPath);
    g_SpreadDegrees = (float)atof(buf);
    if (g_SpreadDegrees < 0.0f) g_SpreadDegrees = 0.0f;

    GetPrivateProfileStringA("Config", "MinSpreadUnits", "0.35", buf, sizeof(buf), iniPath);
    g_MinSpreadUnits = (float)atof(buf);
    if (g_MinSpreadUnits < 0.0f) g_MinSpreadUnits = 0.0f;

    GetPrivateProfileStringA("Config", "NoTargetDistance", "120.0", buf, sizeof(buf), iniPath);
    g_NoTargetDistance = (float)atof(buf);
    if (g_NoTargetDistance < 1.0f) g_NoTargetDistance = 1.0f;

    GetPrivateProfileStringA("Config", "PoliceIgnore", "0", buf, sizeof(buf), iniPath);
    g_PoliceIgnore = atoi(buf) != 0;

    GetPrivateProfileStringA("Config", "InfiniteAmmo", "1", buf, sizeof(buf), iniPath);
    g_InfiniteAmmo = atoi(buf) != 0;

    GetPrivateProfileStringA("Config", "TraceLogShots", "40", buf, sizeof(buf), iniPath);
    g_TraceLogLimit = atoi(buf);
    if (g_TraceLogLimit < 0) g_TraceLogLimit = 0;

    Log("Config: Accuracy=%u, SpreadMultiplier=%.4f, PlayerSpread=%.8f, SpreadDegrees=%.4f, MinSpreadUnits=%.4f, NoTargetDistance=%.2f, PoliceIgnore=%d, InfiniteAmmo=%d, TraceLogShots=%d.",
        (unsigned int)g_AccuracyPatch[1], g_SpreadMultiplier, g_PlayerSpread, g_SpreadDegrees, g_MinSpreadUnits, g_NoTargetDistance, g_PoliceIgnore ? 1 : 0, g_InfiniteAmmo ? 1 : 0, g_TraceLogLimit);
}

DWORD WINAPI InitThread(LPVOID) {
    g_RandomState = GetTickCount() ^ (DWORD)&g_RandomState;
    BuildSkillMultReplacement();
    LoadConfig();
    Log("SpreadControlVC %s initializing.", SPREADCONTROLVC_VERSION);

    // --- NPC patches (FUN_005d1140) ---

    // Patch 1: Ped type check at 0x005D13BD
    BYTE pedTypeCheck[6] = { 0x0F, 0x85, 0x10, 0x05, 0x00, 0x00 };
    ApplyPatch(ADDR_PED_TYPE_CHECK, pedTypeCheck, g_NOP6, 6, "ped type check");

    // Patch 2: Flag check at 0x005D13DA
    BYTE flagCheck[6] = { 0x0F, 0x84, 0x57, 0x04, 0x00, 0x00 };
    ApplyPatch(ADDR_FLAG_CHECK, flagCheck, g_NOP6, 6, "weapon flag check");

    // Patch 3: Accuracy read at 0x005D13E0
    BYTE accuracyRead[6] = { 0x8A, 0x83, 0x06, 0x05, 0x00, 0x00 };
    ApplyPatch(ADDR_ACCURACY_READ, accuracyRead, g_AccuracyPatch, 6, "NPC accuracy override");

    // Patch 4: Local player check at 0x005D1534
    BYTE localPlayerCheck[6] = { 0x0F, 0x85, 0x46, 0x01, 0x00, 0x00 };
    ApplyPatch(ADDR_LOCAL_PLAYER_CHECK, localPlayerCheck, g_NOP6, 6, "local player check");

    // Patch 5: Skill accumulator multiplication at 0x005D1589
    BYTE skillMult[20] = {
        0xD9, 0x05, 0x68, 0xD3, 0x69, 0x00,
        0xD8, 0x88, 0x18, 0x06, 0x00, 0x00,
        0xD8, 0x4C, 0x24, 0x0C,
        0xD9, 0x5C, 0x24, 0x0C
    };
    ApplyPatch(ADDR_SKILL_MULT, skillMult, g_SkillMultReplacement, 20, "skill spread multiplier");

    // --- Player spread patches (FUN_005cbff0) ---
    // The player's instant-hit firing function uses weapon-specific spread
    // constants from .data. The stock values are extremely tiny (~0.0002-0.0005)
    // which makes the player unnaturally accurate. We increase them here.

    WriteFloat(ADDR_PLAYER_SPREAD_DEFAULT, 0.0002f,  g_PlayerSpread, "default player/recoil spread");
    WriteFloat(ADDR_PLAYER_SPREAD_WEAPON_A, 0.0003f, g_PlayerSpread, "weapon 0x1A/0x20/0x23 spread");
    WriteFloat(ADDR_PLAYER_SPREAD_WEAPON_B, 0.00015f, g_PlayerSpread, "weapon 0x1B spread");

    BYTE finalTraceCall[5] = { 0xE8, 0xB0, 0xC1, 0xFF, 0xFF };
    ApplyCallPatch(ADDR_FINAL_TRACE_CALL, finalTraceCall, (void*)FinalTraceWrapper, "final trace target offset hook");

    BYTE playerTraceCall[5] = { 0xE8, 0x6A, 0x2C, 0x00, 0x00 };
    ApplyCallPatch(ADDR_PLAYER_TRACE_CALL, playerTraceCall, (void*)FinalTraceWrapper, "player trace target offset hook");

    if (g_PoliceIgnore) {
        BYTE registerCrimeStart[6] = { 0x53, 0x55, 0x83, 0xEC, 0x10, 0x66 };
        ApplyPatch(ADDR_REGISTER_CRIME, registerCrimeStart, g_Return12Patch, 6, "police ignore crime registration");

        BYTE updateWantedStart[6] = { 0x8B, 0x15, 0xDC, 0x10, 0x69, 0x00 };
        ApplyJumpPatch6(ADDR_UPDATE_WANTED_LEVEL, updateWantedStart, (void*)NoWantedUpdateWrapper, "police ignore wanted level clamp");
    } else {
        Log("Police ignore disabled.");
    }

    if (g_InfiniteAmmo) {
        BYTE totalAmmoDecrement[3] = { 0xFF, 0x4E, 0x0C };
        ApplyPatch(ADDR_TOTAL_AMMO_DECREMENT, totalAmmoDecrement, g_NOP3, 3, "infinite ammo reserve decrement");
    } else {
        Log("Infinite ammo disabled.");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_Module = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
