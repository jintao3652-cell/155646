/* MConnect — SimConnect ABI 声明 + 运行时动态装载。
 *
 * 为什么不用官方 SDK 的 SimConnect.h？
 *   1. 官方 SDK 需要单独安装（MSFS 开发者模式里下载），CI/新机器上没有；
 *   2. 2020 与 2024 的 SimConnect.h 内容不同（2024 多了 _EX1 系列、
 *      EnumerateSimObjectsAndLiveries、SIMCONNECT_RECV_ID 新枚举），
 *      编译期绑定任一头文件都会削掉另一侧的能力。
 *
 * 做法：本文件按官方头文件逐字节复刻**两侧共有的** ABI（结构布局、枚举值），
 * 并用 LoadLibrary + GetProcAddress 在运行时取函数。2024 专属能力
 * （_EX1 创建、涂装枚举）通过函数指针探测：取不到就退回 2020 基础版路径，
 * 取得到就用新路径。因此**同一份 exe 在 2020 和 2024 上都能工作**。
 *
 * 结构布局注意：SimConnect 的结构体全部 #pragma pack(1)（官方头文件如此），
 * 一旦这里改成默认对齐，dwData 之后的解析会整体错位。
 */
#ifndef MCONNECT_SC_H
#define MCONNECT_SC_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ---------------- 基础类型 ---------------- */

typedef DWORD SIMCONNECT_OBJECT_ID;
typedef DWORD SIMCONNECT_DATA_DEFINITION_ID;
typedef DWORD SIMCONNECT_DATA_REQUEST_ID;

static const DWORD SC_UNUSED = 0xFFFFFFFF;
static const DWORD SC_OBJECT_ID_USER = 0;
static const DWORD SC_OBJECT_ID_AI = 1;   /* 占位：AI 对象用实际 ID */

/* TransmitClientEvent 的 group priority / flags。
 * sc.h 不复刻官方 SimConnect.h，所以这里按官方 ABI 常量值自行定义；
 * 灯事件按 AI ObjectID 发送时必须带 GROUPID_IS_PRIORITY，否则会落到用户机。 */
static const DWORD SC_GROUP_PRIORITY_HIGHEST = 0x3FFFFFF8;
static const DWORD SC_EVENT_FLAG_GROUPID_IS_PRIORITY = 0x00000010;

/* ---------------- 接收消息 ID ---------------- */
/* 2020 与 2024 前缀部分完全一致；2024 在其后追加（见 kRecvIdEnumerateLivery）。
 * 未知 ID 一律忽略，保证新版模拟器多出来的消息不会打断解析。 */

enum SC_RECV_ID {
    SC_RECV_ID_NULL = 0,
    SC_RECV_ID_EXCEPTION = 1,
    SC_RECV_ID_OPEN = 2,
    SC_RECV_ID_QUIT = 3,
    SC_RECV_ID_EVENT = 4,
    SC_RECV_ID_EVENT_OBJECT_ADDREMOVE = 5,
    SC_RECV_ID_EVENT_FILENAME = 6,
    SC_RECV_ID_EVENT_FRAME = 7,
    SC_RECV_ID_SIMOBJECT_DATA = 8,
    SC_RECV_ID_SIMOBJECT_DATA_BYTYPE = 9,
    SC_RECV_ID_WEATHER_OBSERVATION = 10,
    SC_RECV_ID_CLOUD_STATE = 11,
    SC_RECV_ID_ASSIGNED_OBJECT_ID = 12,
    SC_RECV_ID_RESERVED_KEY = 13,
    SC_RECV_ID_CUSTOM_ACTION = 14,
    SC_RECV_ID_SYSTEM_STATE = 15,
    SC_RECV_ID_CLIENT_DATA = 16,
    SC_RECV_ID_EVENT_WEATHER_MODE = 17,
    SC_RECV_ID_AIRPORT_LIST = 18,
    SC_RECV_ID_VOR_LIST = 19,
    SC_RECV_ID_NDB_LIST = 20,
    SC_RECV_ID_WAYPOINT_LIST = 21,
    SC_RECV_ID_EVENT_MULTIPLAYER_SERVER_STARTED = 22,
    SC_RECV_ID_EVENT_MULTIPLAYER_CLIENT_STARTED = 23,
    SC_RECV_ID_EVENT_MULTIPLAYER_SESSION_ENDED = 24,
    SC_RECV_ID_EVENT_RACE_END = 25,
    SC_RECV_ID_EVENT_RACE_LAP = 26,
    /* --- 以下为 2020 SU 之后 / 2024 追加，编号按官方 SIMCONNECT_RECV_ID --- */
    SC_RECV_ID_PICK = 27,
    SC_RECV_ID_EVENT_EX1 = 28,
    SC_RECV_ID_FACILITY_DATA = 29,
    SC_RECV_ID_FACILITY_DATA_END = 30,
    SC_RECV_ID_FACILITY_MINIMAL_LIST = 31,
    SC_RECV_ID_JETWAY_DATA = 32,
    SC_RECV_ID_CONTROLLERS_LIST = 33,
    SC_RECV_ID_ACTION_CALLBACK = 34,
    SC_RECV_ID_ENUMERATE_INPUT_EVENTS = 35,
    SC_RECV_ID_GET_INPUT_EVENT = 36,
    SC_RECV_ID_SUBSCRIBE_INPUT_EVENT = 37,
    SC_RECV_ID_ENUMERATE_INPUT_EVENT_PARAMS = 38,
    SC_RECV_ID_ENUMERATE_SIMOBJECT_AND_LIVERY_LIST = 39,
    SC_RECV_ID_FLOW_EVENT = 40,
    SC_RECV_ID_CAMERA_DATA = 41,
    SC_RECV_ID_CAMERA_STATUS = 42,
    SC_RECV_ID_CAMERA_DEFINITION_LIST = 43,
    SC_RECV_ID_COMM_BUS = 44,
    SC_RECV_ID_CAMERA_WORLD_LOCKER = 45
};

/* ---------------- 数据类型 ---------------- */

enum SC_DATATYPE {
    SC_DATATYPE_INVALID = 0,
    SC_DATATYPE_INT32 = 1,
    SC_DATATYPE_INT64 = 2,
    SC_DATATYPE_FLOAT32 = 3,
    SC_DATATYPE_FLOAT64 = 4,
    SC_DATATYPE_STRING8 = 5,
    SC_DATATYPE_STRING32 = 6,
    SC_DATATYPE_STRING64 = 7,
    SC_DATATYPE_STRING128 = 8,
    SC_DATATYPE_STRING256 = 9,
    SC_DATATYPE_STRING260 = 10,
    SC_DATATYPE_STRINGV = 11,
    SC_DATATYPE_INITPOSITION = 12,
    SC_DATATYPE_MARKERSTATE = 13,
    SC_DATATYPE_WAYPOINT = 14,
    SC_DATATYPE_LATLONALT = 15,
    SC_DATATYPE_XYZ = 16
};

/* ---------------- 请求周期 / 标志 ---------------- */

enum SC_PERIOD {
    SC_PERIOD_NEVER = 0,
    SC_PERIOD_ONCE = 1,
    SC_PERIOD_VISUAL_FRAME = 2,
    SC_PERIOD_SIM_FRAME = 3,
    SC_PERIOD_SECOND = 4
};

enum SC_DATA_REQUEST_FLAG {
    SC_DATA_REQUEST_FLAG_DEFAULT = 0,
    SC_DATA_REQUEST_FLAG_CHANGED = 1,
    SC_DATA_REQUEST_FLAG_TAGGED = 2
};

enum SC_DATA_SET_FLAG {
    SC_DATA_SET_FLAG_DEFAULT = 0,
    SC_DATA_SET_FLAG_TAGGED = 1
};

enum SC_SIMOBJECT_TYPE {
    SC_SIMOBJECT_TYPE_USER = 0,
    SC_SIMOBJECT_TYPE_ALL = 1,
    SC_SIMOBJECT_TYPE_AIRCRAFT = 2,
    SC_SIMOBJECT_TYPE_HELICOPTER = 3,
    SC_SIMOBJECT_TYPE_BOAT = 4,
    SC_SIMOBJECT_TYPE_GROUND = 5
};

/* ---------------- 结构体（全部 pack(1)，与官方头文件一致） ---------------- */

#pragma pack(push, 1)

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
} SC_RECV;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwException;
    DWORD dwSendID;
    DWORD dwIndex;
} SC_RECV_EXCEPTION;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    char  szApplicationName[256];
    DWORD dwApplicationVersionMajor;
    DWORD dwApplicationVersionMinor;
    DWORD dwApplicationBuildMajor;
    DWORD dwApplicationBuildMinor;
    DWORD dwSimConnectVersionMajor;
    DWORD dwSimConnectVersionMinor;
    DWORD dwSimConnectBuildMajor;
    DWORD dwSimConnectBuildMinor;
    DWORD dwReserved1;
    DWORD dwReserved2;
} SC_RECV_OPEN;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwObjectID;
    DWORD dwDefineID;
    DWORD dwFlags;
    DWORD dwentrynumber;
    DWORD dwoutof;
    DWORD dwDefineCount;
    DWORD dwData;      /* 数据起始：后跟 dwDefineCount 个数据项 */
} SC_RECV_SIMOBJECT_DATA;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwObjectID;
} SC_RECV_ASSIGNED_OBJECT_ID;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwInteger;
    float fFloat;
    char  szString[260];
} SC_RECV_SYSTEM_STATE;

/* 列表模板：设施列表与 2024 的 simobject/livery 枚举都以此开头 */
typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwArraySize;
    DWORD dwEntryNumber;
    DWORD dwOutOf;
} SC_RECV_LIST_TEMPLATE;

/* 2024 专属：一项 = 一个 (机型容器标题, 涂装名) 组合 */
#define SC_STR_256 256
typedef struct {
    char AircraftTitle[SC_STR_256];
    char LiveryName[SC_STR_256];
} SC_ENUM_SIMOBJECT_LIVERY;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwArraySize;
    DWORD dwEntryNumber;
    DWORD dwOutOf;
    SC_ENUM_SIMOBJECT_LIVERY rgData[1];
} SC_RECV_ENUM_SIMOBJECT_LIVERY_LIST;

typedef struct {
    DWORD  dwSize;
    DWORD  dwVersion;
    DWORD  dwID;
    DWORD  dwRequestID;
    DWORD  dwEventID;
    DWORD  dwData;
} SC_RECV_EVENT;

/* ---- Input Events 结构 ----
 * SIMCONNECT_INPUT_EVENT_DESCRIPTOR：一个输入事件的名字/hash/类型/节点
 * SIMCONNECT_RECV_ENUMERATE_INPUT_EVENTS：分页的 descriptor 数组
 *
 * 注意 Name 是 STRING(64)、NodeNames 是 STRING(1024)，
 * 布局必须与官方 pack(1) 一致，否则读出来的名字全是乱的。
 * 另有 SIMCONNECT_RECV_GET_INPUT_EVENT 用于取单个事件的值。 */
typedef struct {
    char  Name[64];
    DWORD Hash;
    DWORD Type;          /* SIMCONNECT_DATATYPE */
    char  NodeNames[1024];
} SC_INPUT_EVENT_DESCRIPTOR;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwArraySize;
    DWORD dwEntryNumber;
    DWORD dwOutOf;
    SC_INPUT_EVENT_DESCRIPTOR rgData[1];
} SC_RECV_ENUMERATE_INPUT_EVENTS;

typedef struct {
    DWORD dwSize;
    DWORD dwVersion;
    DWORD dwID;
    DWORD dwRequestID;
    DWORD dwHash;
    DWORD dwType;
    BYTE  Value[1];      /* 依 dwType 解释（FLOAT32 / STRING128…） */
} SC_RECV_GET_INPUT_EVENT;

typedef struct {
    double Latitude;
    double Longitude;
    double Altitude;    /* feet */
    double Pitch;       /* degrees */
    double Bank;        /* degrees */
    double Heading;     /* degrees, true */
    DWORD  OnGround;    /* 1 = 置于地面 */
    DWORD  Airspeed;    /* knots */
} SC_DATA_INITPOSITION;

#pragma pack(pop)

/* ---------------- 函数指针类型 ---------------- */

typedef void (CALLBACK* SC_DISPATCH_PROC)(SC_RECV* pData, DWORD cbData, void* pContext);

typedef HRESULT (WINAPI* PFN_SC_Open)(HANDLE* phSimConnect, LPCSTR szName, HWND hWnd,
                                      DWORD UserEventWin32, HANDLE hEventHandle,
                                      DWORD ConfigIndex);
typedef HRESULT (WINAPI* PFN_SC_Close)(HANDLE hSimConnect);
typedef HRESULT (WINAPI* PFN_SC_CallDispatch)(HANDLE hSimConnect, SC_DISPATCH_PROC pfcnDispatch,
                                              void* pContext);
typedef HRESULT (WINAPI* PFN_SC_GetNextDispatch)(HANDLE hSimConnect, SC_RECV** ppData,
                                                 DWORD* pcbData);
typedef HRESULT (WINAPI* PFN_SC_AddToDataDefinition)(HANDLE hSimConnect,
                                                     SIMCONNECT_DATA_DEFINITION_ID DefineID,
                                                     const char* DatumName, const char* UnitsName,
                                                     DWORD DatumType, float fEpsilon, DWORD DatumID);
typedef HRESULT (WINAPI* PFN_SC_ClearDataDefinition)(HANDLE hSimConnect,
                                                     SIMCONNECT_DATA_DEFINITION_ID DefineID);
typedef HRESULT (WINAPI* PFN_SC_RequestDataOnSimObject)(HANDLE hSimConnect,
                                                        SIMCONNECT_DATA_REQUEST_ID RequestID,
                                                        SIMCONNECT_DATA_DEFINITION_ID DefineID,
                                                        SIMCONNECT_OBJECT_ID ObjectID,
                                                        DWORD Period, DWORD Flags,
                                                        DWORD origin, DWORD interval, DWORD limit);
typedef HRESULT (WINAPI* PFN_SC_SetDataOnSimObject)(HANDLE hSimConnect, DWORD Flags,
                                                    SIMCONNECT_OBJECT_ID ObjectID,
                                                    SIMCONNECT_DATA_DEFINITION_ID DefineID,
                                                    DWORD ArrayCount, DWORD cbUnitSize,
                                                    void* pDataSet);
typedef HRESULT (WINAPI* PFN_SC_RequestSystemState)(HANDLE hSimConnect,
                                                    SIMCONNECT_DATA_REQUEST_ID RequestID,
                                                    const char* szState);
typedef HRESULT (WINAPI* PFN_SC_SubscribeToSystemEvent)(HANDLE hSimConnect, DWORD EventID,
                                                        const char* SystemEventName);
typedef HRESULT (WINAPI* PFN_SC_MapClientEventToSimEvent)(HANDLE hSimConnect, DWORD EventID,
                                                          const char* EventName);
typedef HRESULT (WINAPI* PFN_SC_TransmitClientEvent)(HANDLE hSimConnect,
                                                     SIMCONNECT_OBJECT_ID ObjectID,
                                                     DWORD EventID, DWORD dwData,
                                                     DWORD GroupID, DWORD Flags);
typedef HRESULT (WINAPI* PFN_SC_AICreateNonATCAircraft)(HANDLE hSimConnect,
                                                        const char* szContainerTitle,
                                                        const char* szTailNumber,
                                                        SC_DATA_INITPOSITION InitPos,
                                                        SIMCONNECT_DATA_REQUEST_ID RequestID);
typedef HRESULT (WINAPI* PFN_SC_AIRemoveObject)(HANDLE hSimConnect,
                                                SIMCONNECT_OBJECT_ID ObjectID, DWORD RequestID);
typedef HRESULT (WINAPI* PFN_SC_AIReleaseControl)(HANDLE hSimConnect,
                                                  SIMCONNECT_OBJECT_ID ObjectID, DWORD RequestID);

/* 2024 SU1+ 新增（取不到则为 NULL，走 2020 路径）：
 *   AICreateNonATCAircraft_EX1 —— 多了 szLivery，支持模块化（modular）机型
 *   EnumerateSimObjectsAndLiveries —— 枚举容器标题 + 涂装名 */
typedef HRESULT (WINAPI* PFN_SC_AICreateNonATCAircraft_EX1)(HANDLE hSimConnect,
                                                            const char* szContainerTitle,
                                                            const char* szLivery,
                                                            const char* szTailNumber,
                                                            SC_DATA_INITPOSITION InitPos,
                                                            SIMCONNECT_DATA_REQUEST_ID RequestID);
typedef HRESULT (WINAPI* PFN_SC_EnumerateSimObjectsAndLiveries)(HANDLE hSimConnect,
                                                                SIMCONNECT_DATA_REQUEST_ID RequestID,
                                                                DWORD Type);

/* ---- Input Events（2020 SU 起即有；2024 更完整）----
 *
 * 这套 API 解决"标准 SimConnect 事件改不动深度机模"的问题：
 * Fenix / PMDG / Inibuilds 等把座舱逻辑做成自定义 InputEvent，只能用
 * SimConnect_SetInputEvent 按 **hash** 触发；而标准事件（如 COM_RADIO_SET_HZ）
 * 它们往往不响应。
 *
 * 与 WASimCommander（XVoice 用的方案）的关系：
 *   WASimCommander 走"模拟器内 WASM 模块"，能力更强（LVar 读写、RPN 执行），
 *   但要用户额外安装 WASM 包；Input Events 是**纯 SimConnect** 就能用的子集，
 *   覆盖"按住/松开按钮、拨动开关"这类我们真正需要的写入，无需额外安装。
 *
 * 哈希：官方文档说是 CRC 型；实测为 **FNV-1a 32 位**（对 ASCII 事件名），
 * 但不同版本有差异，因此一律用 EnumerateInputEvents 返回的 Hash，不自己算。 */
typedef HRESULT (WINAPI* PFN_SC_EnumerateInputEvents)(HANDLE hSimConnect,
                                                      SIMCONNECT_DATA_REQUEST_ID RequestID);
typedef HRESULT (WINAPI* PFN_SC_EnumerateInputEventParams)(HANDLE hSimConnect,
                                                           DWORD Hash);
typedef HRESULT (WINAPI* PFN_SC_SetInputEvent)(HANDLE hSimConnect, DWORD Hash,
                                               DWORD cbUnitSize, PVOID Value);

/* ---- ClientData（共享内存区）----
 *
 * 用于和**模拟器内 WASM 模块**通信（MobiFlight 就是这条路）：
 *   MapClientDataNameToID  把字符串名字绑到一个 ClientData ID
 *   CreateClientData       在模拟器侧分配内存
 *   AddToClientDataDefinition  定义该区域里的字段布局
 *   SetClientData          往区域里写（发给 WASM 的命令）
 *   RequestClientData      订阅区域变化（收 WASM 的返回）
 *
 * 与 Input Events 的分工：Input Events 只能按名字触发机模预定义的按钮；
 * ClientData + WASM 能执行**任意 RPN 表达式**（LVar 读写、多参数 K 事件、
 * `(>B:...)` B 事件），是深度机模写入/读取的最终方案。 */
typedef HRESULT (WINAPI* PFN_SC_MapClientDataNameToID)(HANDLE hSimConnect,
                                                       const char* szClientDataName,
                                                       DWORD ClientDataID);
typedef HRESULT (WINAPI* PFN_SC_CreateClientData)(HANDLE hSimConnect,
                                                  DWORD ClientDataID,
                                                  DWORD Size,
                                                  DWORD Flags);
typedef HRESULT (WINAPI* PFN_SC_AddToClientDataDefinition)(HANDLE hSimConnect,
                                                           DWORD DefineID,
                                                           DWORD dwOffset,
                                                           DWORD dwSizeOrType,
                                                           float fEpsilon,
                                                           DWORD DatumID);
typedef HRESULT (WINAPI* PFN_SC_SetClientData)(HANDLE hSimConnect,
                                               DWORD ClientDataID,
                                               DWORD DefineID,
                                               DWORD Flags,
                                               DWORD dwReserved,
                                               DWORD cbUnitSize,
                                               PVOID pData);
typedef HRESULT (WINAPI* PFN_SC_RequestClientData)(HANDLE hSimConnect,
                                                   DWORD ClientDataID,
                                                   DWORD RequestID,
                                                   DWORD DefineID,
                                                   DWORD Period,
                                                   DWORD Flags,
                                                   DWORD dwReserved,
                                                   DWORD cbUnitSize);

/* ---------------- API 表 ---------------- */

typedef struct {
    HMODULE dll;

    PFN_SC_Open                        Open;
    PFN_SC_Close                       Close;
    PFN_SC_CallDispatch                CallDispatch;
    PFN_SC_GetNextDispatch             GetNextDispatch;
    PFN_SC_AddToDataDefinition         AddToDataDefinition;
    PFN_SC_ClearDataDefinition         ClearDataDefinition;
    PFN_SC_RequestDataOnSimObject      RequestDataOnSimObject;
    PFN_SC_SetDataOnSimObject          SetDataOnSimObject;
    PFN_SC_RequestSystemState          RequestSystemState;
    PFN_SC_SubscribeToSystemEvent      SubscribeToSystemEvent;
    PFN_SC_MapClientEventToSimEvent    MapClientEventToSimEvent;
    PFN_SC_TransmitClientEvent         TransmitClientEvent;
    PFN_SC_AICreateNonATCAircraft      AICreateNonATCAircraft;
    PFN_SC_AIRemoveObject              AIRemoveObject;
    PFN_SC_AIReleaseControl            AIReleaseControl;

    /* 2024 专属（可为 NULL） */
    PFN_SC_AICreateNonATCAircraft_EX1      AICreateNonATCAircraft_EX1;
    PFN_SC_EnumerateSimObjectsAndLiveries  EnumerateSimObjectsAndLiveries;

    /* Input Events（2020 SU 起；可为 NULL） */
    PFN_SC_EnumerateInputEvents      EnumerateInputEvents;
    PFN_SC_EnumerateInputEventParams EnumerateInputEventParams;
    PFN_SC_SetInputEvent             SetInputEvent;

    /* ClientData（2020 起即有；配合外部 WASM 模块使用，可为 NULL） */
    PFN_SC_MapClientDataNameToID        MapClientDataNameToID;
    PFN_SC_CreateClientData             CreateClientData;
    PFN_SC_AddToClientDataDefinition    AddToClientDataDefinition;
    PFN_SC_SetClientData                SetClientData;
    PFN_SC_RequestClientData            RequestClientData;

    /* 观测到的模拟器侧版本（来自 SimConnect_Open 的 SIMCONNECT_RECV_OPEN）。
     * 注意这是**服务端**版本，与本机装的是哪份 DLL 不一定一致。 */
    int  sim_version_major;   /* 2020 / 2024（按构建号推断），0 = 未知 */
    int  build_major;
    int  build_minor;
    int  supports_modular;    /* 1 = 具备 _EX1 与涂装枚举（2024 SU1+） */
} ScApi;

extern ScApi g_sc;

/* 当前 SimConnect 会话句柄（sc_open 成功后非 NULL）。 */
extern HANDLE g_sc_handle;

/* 装载 SimConnect.dll 并解析函数。返回 0 成功，-1 失败（err 里给出原因）。
 *
 * 查找顺序（模拟器自带的 DLL 才是正确版本，先找它）：
 *   1. 环境变量 MCONNECT_SIMCONNECT（显式指定，调试用）
 *   2. **从模拟器进程的已加载模块表里取路径**（最可靠，见 sc.c 说明）
 *   3. 注册表 HKLM\SOFTWARE\Microsoft\Flight Simulator\10.0 / SimConnect
 *      记载的安装路径
 *   4. 系统目录（MSFS 安装时会往 System32 注册一份）
 *   5. MS Store 包目录（按注册表版本号拼全路径，绕过 WindowsApps 不可枚举）
 *   6. exe 同目录（便于随包分发）
 *   7. 遍历各驱动器常见安装布局
 *   8. 直接 LoadLibraryA("SimConnect.dll") 交给系统搜索路径
 *
 * 第 2 步借鉴 swift：模拟器在运行时，SimConnect.dll 必然已加载进它的
 * 进程空间，直接问系统要它的路径 —— 不用猜、不怕版本错配、
 * 也不受 WindowsApps 目录权限限制。 */
int  sc_load(char* err, int errcap);

/* 建立与模拟器的连接。返回 0 成功。会顺带填充版本信息与 supports_modular。
 * on_recv 为派发回调（sc_open 内部会立刻调用它以完成版本探测）。 */
int  sc_open(SC_DISPATCH_PROC on_recv, void* ctx);

/* 收到 SIMCONNECT_RECV_OPEN 时调用：填充版本与能力信息。 */
void sc_on_open_info(const SC_RECV_OPEN* o);

/* 关闭连接并卸载 DLL。 */
void sc_close(void);

/* 模型名（"2020"/"2024"/"unknown"），用于日志与状态上报。 */
const char* sc_sim_name(void);

#endif /* MCONNECT_SC_H */