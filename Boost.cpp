// SADX Boost Mod (Sonic Generations style) v0.4 — SADX Mod Loader, C++ DLL (x86)
// v0.2: блок спиндэша/шара на кнопке буста, камера с самокалибровкой, меньше заноса вправо.
// v0.3: бег по воде (вода временно становится твёрдой), синие полосы скорости по краям экрана.
// v0.4: камера снова применяется в OnFrame, запасной бег по воде (прижимаем Соника к воде), журнал boost_log.txt.

#define NOMINMAX
#include "SADXModLoader.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>
#include <string>
#include <cstdio>
#include <cstdarg>

// ---------- Настройки ----------
static const int   BOOST_KEY          = 'A';       // клавиша клавиатуры
static const int   BOOST_PAD_BUTTON   = Buttons_Y; // кнопка геймпада
// Кнопки, которые блокируются, пока зажата кнопка буста (спиндэш / шар)
static const int   BLOCKED_BUTTONS    = Buttons_X | Buttons_B | Buttons_Y;

static const float METER_MAX          = 100.0f;
static const float METER_PER_RING     = 4.0f;
static const float DRAIN_PER_FRAME    = 0.45f;
static const float BOOST_SPEED        = 9.0f;
static const float BOOST_ACCEL        = 0.35f;
static const float LATERAL_DAMP       = 0.80f;     // гасит боковой снос (Speed.z)
static const bool  WATER_RUN          = true;      // бег по воде во время буста

// Камера
static const float CAM_DIST           = 26.0f;     // дистанция позади спины
static const float CAM_HEIGHT         = 10.0f;     // высота камеры над Соником
static const float LOOK_HEIGHT        = 6.0f;      // точка, на которую смотрит камера
static const float CAM_POS_LERP       = 0.20f;
static const float CAM_ROT_LERP       = 0.35f;
static const float HEADING_LERP       = 0.12f;

// ---------- Состояние ----------
static float  g_meter     = 0.0f;
static bool   g_boosting  = false;
static bool   g_keyHeld   = false;   // кнопка буста нажата (читается до блокировки)
static short  g_lastRings = 0;
static bool   g_physSaved = false;
static PhysicsData g_origPhys;
static bool   g_hadMagnet = false;

// направление движения (для камеры)
static float  g_hx = 0.0f, g_hz = 1.0f;
static NJS_VECTOR g_prevPos;
static bool   g_hasPrev = false;

// калибровка соглашения о углах камеры: rot = sign * angle + offset
static float g_yc[2] = { 0, 0 }, g_ys[2] = { 0, 0 };
static float g_pc[2] = { 0, 0 }, g_ps[2] = { 0, 0 };

static const float PI_F = 3.14159265f;
static inline float BamsToRad(int a) { return (float)(short)a * PI_F / 32768.0f; }
static inline int   RadToBams(float r) { return (int)(r * 32768.0f / PI_F); }

// ---------- Журнал для отладки ----------
static std::string g_logPath;
static int g_logTick = 0;

static void Log(const char* fmt, ...)
{
    if (g_logPath.empty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath.c_str(), "a") != 0 || !f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static bool IsSonicInGame(EntityData1* data)
{
    return GameState == 15 && data && data->CharID == Characters_Sonic && !MetalSonicFlag;
}

static bool RawBoostKey()
{
    if (GetAsyncKeyState(BOOST_KEY) & 0x8000) return true;
    if (ControllerPointers[0] && (ControllerPointers[0]->HeldButtons & BOOST_PAD_BUTTON)) return true;
    return false;
}

// Блокируем спиндэш/шар, пока зажата кнопка буста
static void MaskInput()
{
    if (!g_keyHeld) return;
    if (!IsSonicInGame(EntityData1Ptrs[0])) return;
    ControllerData* c = ControllerPointers[0];
    if (!c) return;
    c->HeldButtons    &= ~BLOCKED_BUTTONS;
    c->PressedButtons &= ~BLOCKED_BUTTONS;
}

// ---------- Камера ----------
static void Accumulate(float* c, float* s, int rot, int ang)
{
    for (int i = 0; i < 2; i++)
    {
        float sign = (i == 0) ? 1.0f : -1.0f;
        float d = BamsToRad(rot) - sign * BamsToRad(ang);
        c[i] = c[i] * 0.97f + cosf(d) * 0.03f;
        s[i] = s[i] * 0.97f + sinf(d) * 0.03f;
    }
}

static void Resolve(const float* c, const float* s, float& sign, float& offset)
{
    float r0 = sqrtf(c[0] * c[0] + s[0] * s[0]);
    float r1 = sqrtf(c[1] * c[1] + s[1] * s[1]);
    int i = (r1 > r0 + 0.05f) ? 1 : 0;
    sign = (i == 0) ? 1.0f : -1.0f;
    float r = (i == 0) ? r0 : r1;
    offset = (r > 0.5f) ? atan2f(s[i], c[i]) : 0.0f;
}

// Пока камерой управляет игра, подсматриваем, как она поворачивается
static void CalibrateCamera(EntityData1* p)
{
    if (!Camera_Data1) return;
    float dx = p->Position.x - Camera_Data1->Position.x;
    float dy = (p->Position.y + LOOK_HEIGHT) - Camera_Data1->Position.y;
    float dz = p->Position.z - Camera_Data1->Position.z;
    float h = sqrtf(dx * dx + dz * dz);
    if (h < 15.0f) return;
    Accumulate(g_yc, g_ys, Camera_Data1->Rotation.y, RadToBams(atan2f(dx, dz)));
    Accumulate(g_pc, g_ps, Camera_Data1->Rotation.x, RadToBams(atan2f(-dy, h)));
}

static void ApplyCamera(EntityData1* p)
{
    if (!Camera_Data1) return;

    float ysign, yoff, psign, poff;
    Resolve(g_yc, g_ys, ysign, yoff);
    Resolve(g_pc, g_ps, psign, poff);

    // позиция: строго позади по направлению движения, выше
    float tx = p->Position.x - g_hx * CAM_DIST;
    float ty = p->Position.y + CAM_HEIGHT;
    float tz = p->Position.z - g_hz * CAM_DIST;
    Camera_Data1->Position.x += (tx - Camera_Data1->Position.x) * CAM_POS_LERP;
    Camera_Data1->Position.y += (ty - Camera_Data1->Position.y) * CAM_POS_LERP;
    Camera_Data1->Position.z += (tz - Camera_Data1->Position.z) * CAM_POS_LERP;

    // поворот: смотрим на Соника
    float lx = p->Position.x - Camera_Data1->Position.x;
    float ly = (p->Position.y + LOOK_HEIGHT) - Camera_Data1->Position.y;
    float lz = p->Position.z - Camera_Data1->Position.z;
    float h = sqrtf(lx * lx + lz * lz);
    if (h < 0.01f) return;

    int wantYaw   = RadToBams(ysign * atan2f(lx, lz) + yoff);
    int wantPitch = RadToBams(psign * atan2f(-ly, h) + poff);

    int dyaw   = (int)(short)(wantYaw - Camera_Data1->Rotation.y);
    int dpitch = (int)(short)(wantPitch - Camera_Data1->Rotation.x);
    Camera_Data1->Rotation.y += (int)(dyaw * CAM_ROT_LERP);
    Camera_Data1->Rotation.x += (int)(dpitch * CAM_ROT_LERP);
}

static void UpdateHeading(EntityData1* p)
{
    if (g_hasPrev)
    {
        float vx = p->Position.x - g_prevPos.x;
        float vz = p->Position.z - g_prevPos.z;
        float len = sqrtf(vx * vx + vz * vz);
        if (len > 0.5f && len < 60.0f)
        {
            vx /= len; vz /= len;
            g_hx += (vx - g_hx) * HEADING_LERP;
            g_hz += (vz - g_hz) * HEADING_LERP;
            float n = sqrtf(g_hx * g_hx + g_hz * g_hz);
            if (n > 0.001f) { g_hx /= n; g_hz /= n; }
        }
    }
    g_prevPos = p->Position;
    g_hasPrev = true;
}

// ---------- Бег по воде ----------
// Вода в уровне — это COL с флагом 0x2 (несолидный). Пока идёт буст, делаем такие COL твёрдыми,
// после буста возвращаем исходные флаги.
static LandTable* g_waterTable = nullptr;
static std::vector<std::pair<int, int> > g_waterCols; // (индекс, исходные флаги)

// Формат LandTable в памяти SADX (x86): +0 short COLCount, +0x0C указатель на список COL;
// каждый COL занимает 0x24 байта, флаги поверхности лежат по смещению 0x20.
// Работаем по смещениям, чтобы не зависеть от названий полей в заголовках.
static inline int LandCOLCount(LandTable* lt)
{
    return (int)*(short*)lt;
}

static inline int* LandCOLFlags(LandTable* lt, int i)
{
    unsigned char* base = (unsigned char*)lt;
    unsigned char* list = *(unsigned char**)(base + 0x0C);
    if (!list) return nullptr;
    return (int*)(list + i * 0x24 + 0x20);
}

static void EnableWaterRun()
{
    if (g_waterTable) return;
    LandTable* lt = CurrentLandTable;
    if (!lt) return;

    int count = LandCOLCount(lt);
    if (count <= 0 || count > 20000) return;

    for (int i = 0; i < count; i++)
    {
        int* fp = LandCOLFlags(lt, i);
        if (!fp) break;
        int flags = *fp;
        if (flags & 0x2)
        {
            g_waterCols.push_back(std::make_pair(i, flags));
            *fp = (flags & ~0x2) | 0x1;
        }
    }
    g_waterTable = lt;
    Log("WaterRun enabled: COLCount=%d waterCOLs=%d", count, (int)g_waterCols.size());
}

static void DisableWaterRun()
{
    if (!g_waterTable) return;
    if (g_waterTable == CurrentLandTable)
    {
        for (size_t k = 0; k < g_waterCols.size(); k++)
        {
            int* fp = LandCOLFlags(g_waterTable, g_waterCols[k].first);
            if (fp) *fp = g_waterCols[k].second;
        }
    }
    g_waterCols.clear();
    g_waterTable = nullptr;
}

// Запасной способ: ищем высоту поверхности воды под Соником и прижимаем его к ней
static bool FindWaterSurface(EntityData1* p, float& outY)
{
    if (!g_waterTable) return false;
    unsigned char* list = *(unsigned char**)((unsigned char*)g_waterTable + 0x0C);
    if (!list) return false;

    bool found = false;
    float best = -1.0e9f;
    for (size_t k = 0; k < g_waterCols.size(); k++)
    {
        unsigned char* col = list + g_waterCols[k].first * 0x24;
        float* c = (float*)col;                 // Center xyz, радиус в c[3]
        float dx = p->Position.x - c[0];
        float dz = p->Position.z - c[2];
        float r = c[3];
        if (dx * dx + dz * dz > r * r) continue;

        NJS_OBJECT* obj = *(NJS_OBJECT**)(col + 0x18);
        if (!obj) continue;
        float y = obj->pos[1];
        if (y <= p->Position.y + 12.0f && y > p->Position.y - 80.0f && y > best)
        {
            best = y;
            found = true;
        }
    }
    outY = best;
    return found;
}

// ---------- Буст ----------
static void StopBoost(EntityData1* data, CharObj2* co2)
{
    if (!g_boosting) return;
    g_boosting = false;
    Log("StopBoost");

    if (g_physSaved) co2->PhysicsData = g_origPhys;
    g_physSaved = false;

    if (!g_hadMagnet) co2->Powerups &= ~Powerups_MagneticBarrier;
    data->Status &= ~Status_Attack;
    DisableWaterRun();
}

static void StartBoost(EntityData1* data, CharObj2* co2)
{
    g_boosting = true;
    Log("StartBoost");

    g_origPhys = co2->PhysicsData;
    g_physSaved = true;

    co2->PhysicsData.GroundAccel *= 1.8f;
    co2->PhysicsData.AirAccel    *= 2.0f;
    co2->PhysicsData.GroundDecel *= 1.5f;
    co2->PhysicsData.MaxAccel     = std::max(co2->PhysicsData.MaxAccel, BOOST_SPEED + 2.0f);

    g_hadMagnet = (co2->Powerups & Powerups_MagneticBarrier) != 0;

    if (WATER_RUN) EnableWaterRun();

    // стартовое направление камеры = то, куда сейчас смотрит игровая камера
    if (Camera_Data1)
    {
        float vx = data->Position.x - Camera_Data1->Position.x;
        float vz = data->Position.z - Camera_Data1->Position.z;
        float n = sqrtf(vx * vx + vz * vz);
        if (n > 0.01f) { g_hx = vx / n; g_hz = vz / n; }
    }
}

static unsigned int g_rng = 12345u;
static float Rand01()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)((g_rng >> 8) & 0xFFFF) / 65535.0f;
}

static void DrawHUD()
{
    float sw = (float)HorizontalResolution;
    float sh = (float)VerticalResolution;

    float w = sw * 0.22f, h = sh * 0.025f;
    float x2 = sw - sw * 0.04f, y2 = sh - sh * 0.06f;
    float x1 = x2 - w,          y1 = y2 - h;

    DrawRect_Queue(x1 - 3, y1 - 3, x2 + 3, y2 + 3, 62000.0f, 0xFF101830, QueuedModelFlagsB_EnableZWrite);
    float fill = x1 + w * (g_meter / METER_MAX);
    int col = g_boosting ? 0xFF40D0FF : 0xFF1E78FF;
    if (g_meter > 0.0f)
        DrawRect_Queue(x1, y1, fill, y2, 62001.0f, col, QueuedModelFlagsB_EnableZWrite);

    if (g_boosting)
    {
        float edge = sw * 0.12f;
        DrawRect_Queue(0, 0, edge, sh, 61000.0f, 0x502060FF, QueuedModelFlagsB_EnableZWrite);
        DrawRect_Queue(sw - edge, 0, sw, sh, 61000.0f, 0x502060FF, QueuedModelFlagsB_EnableZWrite);

        // синие полосы скорости по краям
        for (int i = 0; i < 10; i++)
        {
            float th = sh * 0.004f;
            float yl = Rand01() * sh;
            float ll = sw * (0.05f + Rand01() * 0.12f);
            DrawRect_Queue(0, yl, ll, yl + th, 61500.0f, 0x7060D0FF, QueuedModelFlagsB_EnableZWrite);
            float yr = Rand01() * sh;
            float lr = sw * (0.05f + Rand01() * 0.12f);
            DrawRect_Queue(sw - lr, yr, sw, yr + th, 61500.0f, 0x7060D0FF, QueuedModelFlagsB_EnableZWrite);
        }
    }
}

extern "C"
{
    __declspec(dllexport) ModInfo SADXModInfo = { ModLoaderVer };

    __declspec(dllexport) void __cdecl Init(const char* path, const HelperFunctions& helperFunctions)
    {
        g_logPath = std::string(path) + "\\boost_log.txt";
        FILE* f = nullptr;
        if (fopen_s(&f, g_logPath.c_str(), "w") == 0 && f) fclose(f);   // очистить старый журнал
        Log("Sonic Boost v0.4 loaded");
    }

    // читаем кнопку буста ДО того, как вырежем её действия из ввода
    __declspec(dllexport) void __cdecl OnInput()
    {
        g_keyHeld = RawBoostKey();
        MaskInput();
    }

    __declspec(dllexport) void __cdecl OnControl()
    {
        MaskInput();
    }

    __declspec(dllexport) void __cdecl OnFrame()
    {
        if (GameState != 15) return;

        EntityData1* data = EntityData1Ptrs[0];
        CharObj2*    co2  = CharObj2Ptrs[0];
        if (!data || !co2) return;

        if (!IsSonicInGame(data))
        {
            if (g_boosting) StopBoost(data, co2);
            DisableWaterRun();
            return;
        }

        UpdateHeading(data);

        if (Rings > g_lastRings)
            g_meter = std::min(METER_MAX, g_meter + (Rings - g_lastRings) * METER_PER_RING);
        g_lastRings = Rings;

        bool down = g_keyHeld || RawBoostKey();
        bool want = down && g_meter > 0.0f;

        if (want && !g_boosting) StartBoost(data, co2);
        if (!want && g_boosting)  StopBoost(data, co2);

        if (g_boosting)
        {
            g_meter = std::max(0.0f, g_meter - DRAIN_PER_FRAME);

            if (co2->Speed.x < BOOST_SPEED) co2->Speed.x += BOOST_ACCEL;
            co2->Speed.z *= LATERAL_DAMP;               // меньше заноса вбок

            data->Status &= ~Status_Ball;               // не сворачиваться в шар
            data->Status |= Status_Attack;              // сносит врагов
            co2->Powerups |= Powerups_MagneticBarrier;  // магнит колец

            // бег по воде: прижимаем Соника к поверхности воды
            float wy = 0.0f;
            bool onWater = WATER_RUN && FindWaterSurface(data, wy) && data->Position.y < wy + 7.0f;
            if (onWater)
            {
                data->Position.y = wy + 5.0f;
                if (co2->Speed.y < 0.0f) co2->Speed.y = 0.0f;
                data->Status |= Status_Ground;
            }

            // камера (применяем и здесь, и перед отрисовкой)
            ApplyCamera(data);

            if (++g_logTick % 30 == 1 && Camera_Data1)
            {
                Log("BOOST meter=%.1f spd=%.2f pos=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f) camRot=(%d,%d) waterCols=%d onWater=%d wy=%.1f",
                    g_meter, co2->Speed.x, data->Position.x, data->Position.y, data->Position.z,
                    Camera_Data1->Position.x, Camera_Data1->Position.y, Camera_Data1->Position.z,
                    (int)Camera_Data1->Rotation.y, (int)Camera_Data1->Rotation.x,
                    (int)g_waterCols.size(), onWater ? 1 : 0, wy);
            }
        }
        else
        {
            CalibrateCamera(data);
        }

        DrawHUD();
    }

    // Камеру двигаем прямо перед отрисовкой, чтобы игра не перезаписала её
    __declspec(dllexport) void __cdecl OnRenderSceneStart()
    {
        if (GameState != 15) return;
        EntityData1* data = EntityData1Ptrs[0];
        if (!IsSonicInGame(data)) return;

        if (g_boosting) ApplyCamera(data);
        else            CalibrateCamera(data);
    }
}
