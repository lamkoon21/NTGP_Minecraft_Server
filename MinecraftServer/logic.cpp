// Logic 스레드와 게임 상태
//
// 게임 상태(플레이어 · 바뀐 블록 · 시드 · 틱 번호 · IP별 저장 위치)는 이 파일 안의 static이고 Logic 스레드만 읽고 쓴다.
// 만지는 스레드가 하나뿐이라 잠금이 없다.
// 다른 스레드와는 입력 큐(g_inQueue)와 송신 버퍼(QueuePacket)로만 주고받는다.
// ============================================================================

#include "server.h"
#include <chrono>
#include <map>
#include <string>
#include <tuple>


// ---------------------------------------------------------------------------
// 공유 데이터 : 틱 통계 (status 명령에서 읽음)
// ---------------------------------------------------------------------------
TickStats g_tickStats;


// ---------------------------------------------------------------------------
// 게임 상태 : Logic 스레드만 읽고 쓴다
// ---------------------------------------------------------------------------

// 슬롯 번호 = 플레이어 id
struct Player {
    bool        in_game = false;    // 게임 중 확인, HandleHello()에서 켜고 HandleLeave()에서 끈다 (세션의 PLAYING과 같이 움직임)
    bool        has_moved = false;  // CS_MOVE_PLAYER를 한 번이라도 받았거나 저장 위치에서 시작했는지 (SC_ADD_PLAYER의 has_moved)
    PlayerState state;              // 마지막으로 받은 위치, 시선, 든 블록, 이동 모드
};

static Player players[MAX_PLAYERS];

// 바뀐 블록 : 블록 월드 좌표 (x, y, z) -> 마지막으로 바뀐 BlockType. 좌표마다 마지막 변경 하나만
static std::map<std::tuple<int32_t, int32_t, int32_t>, uint8_t> worldDiff;

// 월드 시드 : main()이 정해서 LogicThread의 인자로 넘긴다. 입장할 때 SC_JOIN_OK로 보낸다
static int32_t seed = 0;

// 틱 번호 : 틱마다 +1. SC_SNAPSHOT에 넣는다
static uint32_t tickNumber = 0;

// 재접속 저장 위치 : 접속한 IP 주소 -> 나갈 때의 마지막 위치. HandleLeave()가 쓰고 HandleHello()가 찾는다
// 위치만 기억한다. 시선 · 든 블록 · 이동 모드는 다시 들어와도 처음 값으로 시작한다
struct SavedPosition {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};
static std::map<std::string, SavedPosition> savedPositions;


// ---------------------------------------------------------------------------
// QueueJoinData : 새 접속자의 송신 버퍼에 입장 데이터를 넣는다 (Logic 스레드)
// ---------------------------------------------------------------------------
// id : 입장하는 접속자의 슬롯 번호. HandleHello()가 CS_HELLO의 버전을 확인한 뒤 부른다.
// 넣는 순서
//   SC_JOIN_OK(id, 시드, 틱 속도, 초기 패킷 총개수, 재접속 저장 위치) 1개
//   -> 바뀐 칸마다 SC_BLOCK_CHANGE
//   -> 기존 플레이어마다 SC_ADD_PLAYER (마지막으로 받은 위치 · 시선 · 이동 모드 · 든 블록)
// 초기 패킷 총개수 = 바뀐 칸 수 + 기존 플레이어 수. 클라이언트는 이만큼 적용을 마친 뒤 게임을 시작한다 (로딩 진행률).
// 청크는 보내지 않는다. 클라이언트가 시드로 만든다.
// 한 틱 안에서 이어서 넣으므로 그 사이에 다른 변경이 끼어들지 않는다.
// 이 뒤로 이 접속자에게 가는 실시간 변경(블록 변경 · 입퇴장 · 스냅샷)은 같은 송신 버퍼를 거치므로 항상 초기 목록 뒤에 도착한다.
// 기존 플레이어를 셀 때 자기 자신(id)은 뺀다.
// v.3의 SendJoinData()에서, 직접 보내지 않고 송신 버퍼에 넣기만 하므로 Send를 Queue로 바꿨다.
// 저장 위치는 HandleHello()가 players[id]에 채워 둔 것을 담는다 (그때 has_moved가 켜져 있다).
//
// 바뀐 칸이 아주 많으면(1MB / 16바이트 = 약 6만 5천 칸) 입장 데이터만으로 QueuePacket()의 1MB 제한에 걸릴 수 있다.
void QueueJoinData(int id)
{
    // 1) 초기 패킷 총개수 = 바뀐 칸 수 + 기존 플레이어 수
    uint32_t existing = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i != id && players[i].in_game)
            ++existing;
    }

    // 2) SC_JOIN_OK
    SC_JOIN_OK_PACKET join;
    join.id = static_cast<uint8_t>(id);
    join.seed = seed;
    join.tick_rate = SERVER_TICK_RATE;
    join.initial_packet_count = static_cast<uint32_t>(worldDiff.size()) + existing;
    if (players[id].has_moved) {   // 재접속 : HandleHello()가 저장 위치를 넣어 두었다
        join.has_saved_pos = 1;
        join.saved_x = players[id].state.x;
        join.saved_y = players[id].state.y;
        join.saved_z = players[id].state.z;
    }
    QueuePacket(id, &join);

    // 3) 바뀐 칸마다 SC_BLOCK_CHANGE
    for (const auto& [pos, type] : worldDiff) {
        const auto& [x, y, z] = pos;

        SC_BLOCK_CHANGE_PACKET block;
        block.x = x;
        block.y = y;
        block.z = z;
        block.block_type = type;
        QueuePacket(id, &block);
    }

    // 4) 기존 플레이어마다 SC_ADD_PLAYER
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i == id || !players[i].in_game) // 자기 자신이거나 게임중이 아니라면 넘기기
            continue;

        SC_ADD_PLAYER_PACKET add;
        add.id = static_cast<uint8_t>(i);
        add.has_moved = players[i].has_moved ? 1 : 0;
        add.state = players[i].state;
        QueuePacket(id, &add);
    }
}


// ---------------------------------------------------------------------------
// LogicThread : 50ms마다 입력을 처리하고 결과를 송신 버퍼에 쓴다 (Logic 스레드, 1개)
// ---------------------------------------------------------------------------
// arg = 월드 시드 (main()이 intptr_t로 바꿔 넘긴다)
// 대기 가능 타이머를 50ms 주기(SERVER_TICK_MS)로 만들고 WaitForMultipleObjects()로 종료 이벤트와 타이머를 함께 기다린다.
// 틱마다 g_inQueueCS 안에서 입력 큐를 지역 벡터와 swap으로 통째로 바꾼 뒤,
// 잠금 밖에서 ProcessInput()을 받은 순서대로 부르고, CleanupClients()와 BroadcastSnapshot(++tickNumber)을 부른다.
// 틱 처리 시간을 재어 g_tickStats(tick · avgUs · maxUs)에 쓴다 (status 명령).
DWORD WINAPI LogicThread(LPVOID arg)
{
    // main()이 정한 시드를 게임 상태에 둔다. 이후로는 이 스레드만 읽는다 (QueueJoinData가 SC_JOIN_OK에 담음)
    seed = static_cast<int32_t>(reinterpret_cast<intptr_t>(arg));

    // 1) 틱 타이머 : 50ms 뒤에 처음 켜지고 이후 50ms마다 켜진다 (자동 리셋)
    LARGE_INTEGER due;
    due.QuadPart = -10000LL * SERVER_TICK_MS;   // 100ns 단위, 음수는 지금부터의 상대 시간
    const HANDLE hTimer = CreateWaitableTimer(nullptr, FALSE, nullptr);
    if (hTimer == nullptr || !SetWaitableTimer(hTimer, &due, SERVER_TICK_MS, nullptr, nullptr, FALSE)) {
        Log("틱 타이머를 만들지 못함, 오류 코드 %lu : Logic 스레드를 끝냄", GetLastError());
        if (hTimer != nullptr)
            CloseHandle(hTimer);
        return 1;
    }

    const HANDLE events[] = { g_hQuitEvent, hTimer };   // 둘 다 켜져 있으면 앞의 종료 이벤트를 먼저 본다
    std::vector<InputMsg> inputs;                       // 틱마다 g_inQueue와 바꿔 쓴다
    uint64_t totalUs = 0;                               // 평균을 내기 위한 처리 시간 합
    uint32_t maxUs = 0;

    while (true) {
        // 2) 기다리기 -> 종료 신호?
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0)
            break;
        if (result != WAIT_OBJECT_0 + 1) {
            Log("WaitForMultipleObjects() 실패, 오류 코드 %lu : Logic 스레드를 끝냄", GetLastError());
            break;
        }
        const auto start = std::chrono::steady_clock::now();

        // 3) 입력 큐 통째로 꺼내기 : 잠금 안에서는 swap 한 번만
        EnterCriticalSection(&g_inQueueCS);
        inputs.swap(g_inQueue);
        LeaveCriticalSection(&g_inQueueCS);

        // 4) 받은 순서대로 처리 : 게임 상태는 이 스레드만 바꾸므로 잠금 없이
        for (const InputMsg& msg : inputs)
            ProcessInput(msg);
        inputs.clear();

        // 5) 끝난 세션 회수 -> 스냅샷 예약
        CleanupClients();
        BroadcastSnapshot(++tickNumber);

        // 6) 틱 처리 시간 기록 (status 명령) : 평균은 서버를 켠 뒤 모든 틱의 평균
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const uint32_t us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        totalUs += us;
        if (us > maxUs)
            maxUs = us;
        g_tickStats.avgUs = static_cast<uint32_t>(totalUs / tickNumber);
        g_tickStats.maxUs = maxUs;
        g_tickStats.tick = tickNumber;
    }

    CloseHandle(hTimer);
    return 0;
}


// ---------------------------------------------------------------------------
// ProcessInput : 입력 한 건을 종류에 따라 Handle…()로 넘긴다
// ---------------------------------------------------------------------------
// g_sessionCS 안에서 그 슬롯의 세대 번호와 상태를 읽는다.
// 세대 번호가 다르거나 슬롯이 FREE면 이미 정리된 세션이 늦게 보낸 입력이므로 버린다.
// LEAVE -> HandleLeave(), PACKET -> 패킷 종류에 따라 HandleHello() · HandleMove() · HandleBlockChange()
// 패킷의 종류와 크기(header.size == sizeof(그 패킷))가 맞지 않아도 버린다.
// 세션 상태에 맞지 않는 패킷도 버린다. CS_HELLO는 JOINING, CS_MOVE_PLAYER · CS_BLOCK_CHANGE는 PLAYING일 때만 넘긴다.
void ProcessInput(const InputMsg& msg)
{
    if (msg.id < 0 || msg.id >= MAX_PLAYERS)
        return;

    // 1) 세대 번호 · 상태 읽기 : 잠금 안에서는 복사만
    EnterCriticalSection(&g_sessionCS);
    const uint32_t gen = g_sessions[msg.id].gen;
    const SessionState state = g_sessions[msg.id].state;
    LeaveCriticalSection(&g_sessionCS);

    // 이미 정리된 세션이 늦게 보낸 입력은 버린다
    if (msg.gen != gen || state == SessionState::FREE)
        return;

    // 2) 퇴장
    if (msg.kind == InputKind::LEAVE) {
        HandleLeave(msg.id);
        return;
    }

    // 3) 패킷 : 종류 · 크기 · 세션 상태가 모두 맞을 때만 넘긴다
    PacketHeader header;
    memcpy(&header, msg.packet, sizeof(header));
    switch (header.type) {
    case CS_HELLO:          // 입장 전(JOINING)에만
        if (header.size == sizeof(CS_HELLO_PACKET) && state == SessionState::JOINING)
            HandleHello(msg.id, msg.packet);
        break;
    case CS_MOVE_PLAYER:    // 입장 뒤(PLAYING)에만
        if (header.size == sizeof(CS_MOVE_PLAYER_PACKET) && state == SessionState::PLAYING)
            HandleMove(msg.id, msg.packet);
        break;
    case CS_BLOCK_CHANGE:   // 입장 뒤(PLAYING)에만
        if (header.size == sizeof(CS_BLOCK_CHANGE_PACKET) && state == SessionState::PLAYING)
            HandleBlockChange(msg.id, msg.packet);
        break;
    default:                // 모르는 종류는 버린다
        break;
    }
}


// ---------------------------------------------------------------------------
// HandleHello : CS_HELLO 처리
// ---------------------------------------------------------------------------
// 프로토콜 버전이 다르면 SC_JOIN_REJECT(JOIN_REJECT_VERSION)를 QueuePacket()으로 예약하고 RequestDisconnect().
// 같으면 그 세션의 IP(g_sessionCS 안에서 읽음)로 저장 위치를 찾아, 있으면 players[id]의 처음 상태로 쓴다 (has_moved = true).
// 그다음 QueueJoinData(id)로 입장 데이터를 예약하고 players[id].in_game을 켠 뒤,
// 다른 접속자에게 SC_ADD_PLAYER(저장 위치가 있으면 그 위치와 has_moved = 1)를 QueueToAll(…, id)로 예약하고
// g_sessionCS 안에서 세션을 PLAYING으로 바꾼다.
void HandleHello(int id, const char* packet)
{
    CS_HELLO_PACKET hello;
    memcpy(&hello, packet, sizeof(hello));

    // 1) 버전이 다르면 SC_JOIN_REJECT를 예약하고 끊는다
    //    QueuePacket()은 끊는 중인 세션에 붙이지 않으므로 RequestDisconnect()보다 먼저 넣는다
    if (hello.version != PROTOCOL_VERSION) {
        SC_JOIN_REJECT_PACKET reject;
        reject.reason = JOIN_REJECT_VERSION;
        QueuePacket(id, &reject);
        RequestDisconnect(id);
        Log("[입장 거절] id %d : 프로토콜 버전 %d (서버 %d)", id, hello.version, PROTOCOL_VERSION);
        return;
    }

    // 2) 그 세션의 IP로 저장 위치 찾기 : 잠금 안에서는 IP 복사만
    char ip[INET_ADDRSTRLEN];
    EnterCriticalSection(&g_sessionCS);
    memcpy(ip, g_sessions[id].ip, sizeof(ip));
    LeaveCriticalSection(&g_sessionCS);

    Player& player = players[id];
    player = Player{};   // 이 슬롯을 먼저 쓴 플레이어가 남긴 상태를 지운다
    const auto saved = savedPositions.find(ip);
    const bool hasSaved = (saved != savedPositions.end());
    if (hasSaved) {
        player.state.x = saved->second.x;
        player.state.y = saved->second.y;
        player.state.z = saved->second.z;
        player.has_moved = true;
    }

    // 3) 입장 데이터 예약 -> 게임 중으로 표시
    QueueJoinData(id);
    player.in_game = true;

    // 4) 다른 접속자에게 새 플레이어 알림
    SC_ADD_PLAYER_PACKET add;
    add.id = static_cast<uint8_t>(id);
    add.has_moved = player.has_moved ? 1 : 0;
    add.state = player.state;
    QueueToAll(&add, id);

    // 5) PLAYING으로 : 이제부터 스냅샷 · 블록 변경 · 입퇴장 알림을 받는다
    EnterCriticalSection(&g_sessionCS);
    g_sessions[id].state = SessionState::PLAYING;
    LeaveCriticalSection(&g_sessionCS);

    Log("[입장] id %d%s", id, hasSaved ? " : 저장 위치에서 시작" : "");
}


// ===========================================================================
// 아래는 아직 구현하지 않은 함수다. 빌드가 되도록 빈 본문만 두었다.
// 설명은 설계서의 서버 함수 표를 따른다. 구현할 때 (void) 줄과 TODO를 지운다.
// ===========================================================================

// ---------------------------------------------------------------------------
// HandleMove : CS_MOVE_PLAYER 처리
// ---------------------------------------------------------------------------
// 위치는 클라이언트가 계산한 값을 그대로 쓴다. 이동 · 충돌을 다시 판정하지 않는다.
// 값이 유효한 숫자인지(NaN · 무한대 아님), 월드 범위 안인지, pitch가 -89 ~ 89인지만 확인해 깨진 값을 거르고
// players[id].state에 저장, has_moved = true. 바로 중계하지 않는다.
// 저장한 상태는 틱마다 BroadcastSnapshot()이 보내고, 나중에 들어오는 사람의 입장 데이터(QueueJoinData)와 재접속 저장 위치에도 쓴다.
void HandleMove(int id, const char* packet)
{
    // TODO: 구현
    (void)id;
    (void)packet;
}

// ---------------------------------------------------------------------------
// HandleBlockChange : CS_BLOCK_CHANGE 처리
// ---------------------------------------------------------------------------
// 좌표가 월드 안(청크 -19 ~ 19, 높이 0 ~ 127)인지와 블록 종류가 맞는지 두 가지만 검사한다.
// 거리 · 빈도 · 다른 플레이어와 겹치는지는 검사하지 않는다.
// 통과하면 worldDiff에 기록하고 보낸 사람을 포함한 전원에게 SC_BLOCK_CHANGE를 예약한다 (QueueToAll(…, -1)).
// 거부하면 기록하지 않고 아무것도 보내지 않는다. 요청한 클라이언트는 미리 바꾸지 않았으므로 되돌릴 것이 없다.
void HandleBlockChange(int id, const char* packet)
{
    // TODO: 구현
    (void)id;
    (void)packet;
}

// ---------------------------------------------------------------------------
// HandleLeave : Recv 스레드가 넣은 퇴장 입력 처리
// ---------------------------------------------------------------------------
// players[id].in_game이면 마지막 위치를 그 IP의 저장 위치로 기록한 뒤 지우고(in_game = false),
// 남은 접속자에게 SC_REMOVE_PLAYER를 예약한다. 저장 위치는 메모리에만 있어 서버를 끄면 사라진다.
// 아직 입장 전이었다면 알리지 않는다.
// g_sessionCS 안에서 세션을 CLOSING으로 바꾼다. 소켓을 닫고 슬롯을 비우는 일은 CleanupClients()가 한다.
void HandleLeave(int id)
{
    // TODO: 구현
    (void)id;
}

// ---------------------------------------------------------------------------
// BroadcastSnapshot : 이번 틱의 플레이어 상태를 모두에게 예약한다 (매 틱)
// ---------------------------------------------------------------------------
// 게임 중인 플레이어마다, 자신을 뺀 다른 플레이어의 상태를
// SC_SNAPSHOT 하나(틱 번호 + 상태 묶음)로 만들어 QueuePacket()으로 예약한다.
// 아무도 움직이지 않아도 보내서, 클라이언트가 서버가 살아 있음을 알고 틱 번호로 보간할 수 있게 한다.
// SC_SNAPSHOT은 길이가 바뀌는 패킷이라 header.size = SNAPSHOT_BASE_SIZE + count * sizeof(SnapshotEntry)로 맞춰서 넣는다.
void BroadcastSnapshot(uint32_t tick)
{
    // TODO: 구현
    (void)tick;
}
