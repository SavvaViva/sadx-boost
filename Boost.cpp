// SADX Boost Mod (Sonic Generations style) — SADX Mod Loader, C++ DLL (x86)
// Сборка: Visual Studio, проект DLL Win32, подключить SADXModLoader.h (mod-loader-common / sadx-mod-loader).
// ВНИМАНИЕ: код не компилировался и не тестировался в игре. Имена полей/функций сверь с вашей версией заголовков.

#include "SADXModLoader.h"
#include <cmath>
#include <algorithm>

// ---------- Настройки (можно вынести в mod.ini) ----------
static const int   BOOST_KEY          = 'A';     // клавиша клавиатуры
static const int   BOOST_PAD_BUTTON   = Buttons_Y; // кнопка геймпада
static const float METER_MAX          = 100.0f;
static const float METER_PER_RING     = 4.0f;    // сколько буста даёт 1 кольцо
static const float DRAIN_PER_FRAME    = 0.45f;   // расход за кадр (~27/сек при 60 FPS)
static const float BOOST_SPEED        = 9.0f;    // целевая скорость буста
static const float BOOST_ACCEL        = 0.35f;   // разгон до целевой скорости
static const float CAM_DIST           = 38.0f;   // дистанция камеры позади спины
static const float CAM_HEIGHT         = 14.0f;   // высота над Соником
static const float CAM_LERP           = 0.18f;   // плавность камеры

// ---------- Состояние ----------
static float  g_meter     = 0.0f;
static bool   g_boosting  = false;
static short  g_lastRings = 0;
static bool   g_physSaved = false;
static PhysicsData g_origPhys;
static bool   g_hadMagnet = false;

static bool BoostPressed()
{
    if (GetAsyncKeyState(BOOST_KEY) & 0x8000) return true;
    if (ControllerPointers[0] && (ControllerPointers[0]->HeldButtons & BOOST_PAD_BUTTON)) return true;
    return false;
}

static void StopBoost(EntityData1* data, CharObj2* co2)
{
    if (!g_boosting) return;
    g_boosting = false;

    if (g_physSaved) co2->PhysicsData = g_origPhys;
    g_physSaved = false;

    if (!g_hadMagnet) co2->Powerups &= ~Powerups_MagneticBarrier; // убрать магнит колец
    data->Status &= ~Status_Attack;                                // убрать "атакующий" статус
}

static void StartBoost(EntityData1* data, CharObj2* co2)
{
    g_boosting = true;

    g_origPhys = co2->PhysicsData;
    g_physSaved = true;

    // Более управляемый Соник: быстрее разгон/торможение, лучше воздух
    co2->PhysicsData.GroundAccel *= 1.8f;
    co2->PhysicsData.AirAccel    *= 2.0f;
    co2->PhysicsData.GroundDecel *= 1.5f;
    co2->PhysicsData.MaxAccel     = std::max(co2->PhysicsData.MaxAccel, BOOST_SPEED + 2.0f);

    g_hadMagnet = (co2->Powerups & Powerups_MagneticBarrier) != 0;
}

static void UpdateCamera(EntityData1* player, float strength)
{
    if (!Camera_Data1) return;

    // направление взгляда Соника (если камера окажется спереди — поменяй знак у sx/sz)
    float yaw = (float)player->Rotation.y;
    float sx = njSin((Angle)yaw);
    float sz = njCos((Angle)yaw);

    NJS_VECTOR target;
    target.x = player->Position.x - sx * CAM_DIST;
    target.y = player->Position.y + CAM_HEIGHT;
    target.z = player->Position.z - sz * CAM_DIST;

    float k = CAM_LERP * strength;
    Camera_Data1->Position.x += (target.x - Camera_Data1->Position.x) * k;
    Camera_Data1->Position.y += (target.y - Camera_Data1->Position.y) * k;
    Camera_Data1->Position.z += (target.z - Camera_Data1->Position.z) * k;

    // поворот камеры по курсу Соника (смещение 0x4000 может потребовать подгонки)
    Angle desired = (Angle)(-player->Rotation.y - 0x4000);
    Angle diff = (Angle)(desired - Camera_Data1->Rotation.y);
    Camera_Data1->Rotation.y += (Angle)((float)diff * k);
}

static void DrawHUD()
{
    float sw = (float)HorizontalResolution;
    float sh = (float)VerticalResolution;

    // шкала справа снизу
    float w = sw * 0.22f, h = sh * 0.025f;
    float x2 = sw - sw * 0.04f, y2 = sh - sh * 0.06f;
    float x1 = x2 - w,          y1 = y2 - h;

    DrawRect_Queue(x1 - 3, y1 - 3, x2 + 3, y2 + 3, 62000.0f, 0xFF101830, QueuedModelFlagsB_EnableZWrite);
    float fill = x1 + w * (g_meter / METER_MAX);
    int col = g_boosting ? 0xFF40D0FF : 0xFF1E78FF;
    if (g_meter > 0.0f)
        DrawRect_Queue(x1, y1, fill, y2, 62001.0f, col, QueuedModelFlagsB_EnableZWrite);

    // "мыло" по бокам во время буста — полупрозрачные синие полосы
    if (g_boosting)
    {
        float edge = sw * 0.12f;
        DrawRect_Queue(0, 0, edge, sh, 61000.0f, 0x502060FF, QueuedModelFlagsB_EnableZWrite);
        DrawRect_Queue(sw - edge, 0, sw, sh, 61000.0f, 0x502060FF, QueuedModelFlagsB_EnableZWrite);
    }
}

extern "C"
{
    __declspec(dllexport) ModInfo SADXModInfo = { ModLoaderVer };

    __declspec(dllexport) void __cdecl Init(const char* path, const HelperFunctions& helperFunctions)
    {
    }

    __declspec(dllexport) void __cdecl OnFrame()
    {
        if (GameState != 15) return; // только в игровом процессе

        EntityData1* data = EntityData1Ptrs[0];
        CharObj2*    co2  = CharObj2Ptrs[0];
        if (!data || !co2) return;

        // Только Соник (не Metal Sonic)
        if (data->CharID != Characters_Sonic || MetalSonicFlag) { g_boosting = false; return; }

        // Копим буст от колец
        if (Rings > g_lastRings)
            g_meter = std::min(METER_MAX, g_meter + (Rings - g_lastRings) * METER_PER_RING);
        g_lastRings = Rings;

        bool want = BoostPressed() && g_meter > 0.0f;

        if (want && !g_boosting) StartBoost(data, co2);
        if (!want && g_boosting)  StopBoost(data, co2);

        if (g_boosting)
        {
            g_meter = std::max(0.0f, g_meter - DRAIN_PER_FRAME);

            // Разгон вперёд; препятствия остановят Соника обычной физикой
            if (co2->Speed.x < BOOST_SPEED) co2->Speed.x += BOOST_ACCEL;

            co2->Powerups |= Powerups_MagneticBarrier;  // кольца притягиваются
            data->Status  |= Status_Attack;             // сносит врагов

            UpdateCamera(data, 1.0f);
        }

        DrawHUD();
    }
}
