// ============================================================================
// protocol.h : 서버와 클라이언트가 함께 쓰는 패킷 약속
// ============================================================================
// 서버(server.h)와 클라이언트(net_client.cpp)가 똑같이 include. 고칠 때는 두 저장소의 파일을 같이 고친다
//
// 모든 패킷은 [헤더 3바이트][내용] 꼴 
// 헤더 - 패킷 전체 크기 + 종류
// 두 패킷이 recv() 한 번에 오거나, 나뉘어 올 수 있다.
// 그래서 받는 쪽은 헤더의 size를 보고 패킷 하나만큼 잘라 낸다 (ExtractPacket).
//
// 패킷 10가지
//   CS_ : 클라이언트 -> 서버
//     CS_HELLO          프로토콜 버전 전달
//     CS_MOVE_PLAYER    위치, 시선, 든 블록, 이동 모드
//     CS_BLOCK_CHANGE   블록 부수기/놓기 요청
//     CS_PING           연결 유지 알림
//
//   SC_ : 서버 -> 클라이언트
//     SC_JOIN_OK        내 ID, 월드 시드, 틱 속도, 초기 패킷 총개수, 재접속 저장 위치
//     SC_JOIN_REJECT    접속 거절 사유
//     SC_ADD_PLAYER     플레이어 추가
//     SC_REMOVE_PLAYER  플레이어 퇴장
//     SC_SNAPSHOT       서버 틱 번호와 다른 플레이어들의 상태
//     SC_BLOCK_CHANGE   블록 변경 결과
//
// 게임의 흐름
//   입장 : 접속 후 클라이언트가 CS_HELLO 전송
//          서버가 SC_JOIN_OK 또는 SC_JOIN_REJECT 응답
//          승인 시 초기 SC_BLOCK_CHANGE와 SC_ADD_PLAYER가 뒤따르고, 다른 클라이언트에게는 SC_ADD_PLAYER
//          클라이언트는 초기 패킷을 모두 적용한 뒤 게임 시작
//
//   이동 : 클라이언트는 20Hz 간격으로 상태가 바뀌면 CS_MOVE_PLAYER 등록
//          서버는 20 TPS로 처리하고 SC_SNAPSHOT으로 상태 전달
//
//   블록 : CS_BLOCK_CHANGE 요청을 서버가 검증
//          승인한 변경은 요청자를 포함한 전원에게 SC_BLOCK_CHANGE 전달
//
//   유지 : 클라이언트 SendThread가 1초마다 CS_PING 전송
//   퇴장 : 서버가 남은 클라이언트에게 SC_REMOVE_PLAYER 전달
//
// 바이트 순서 : 서버와 클라이언트가 모두 Windows x86/x64(리틀 엔디언)
//               sockaddr_in에 넣는 포트 번호만 htons()로 바꾼다.
// ============================================================================

// include guard
#ifndef PROTOCOL_H // PROTOCOL_H가 정의되어 있지 않을 때만
#define PROTOCOL_H // PROTOCOL_H를 정의

#include <winsock2.h>  
#include <cstdint> 
#include <cstring>

#pragma comment(lib, "ws2_32.lib")  
// 윈속 라이브러리를 자동 링크

// ---------------------------------------------------------------------------
// 공통 상수
// ---------------------------------------------------------------------------
constexpr uint16_t SERVER_PORT = 9000;  // 서버가 listen하는 포트
constexpr int MAX_PLAYERS = 3;          // 최대 플레이어 정원. 서버의 세션 표 g_sessions[MAX_PLAYERS], 플레이어 id는 0 ~ 2
constexpr int MAX_PACKET_SIZE = 64;     // 패킷 하나의 최대 바이트 수. 가장 큰 SC_SNAPSHOT(54바이트)보다 넉넉하게 잡았다.
                                        // ExtractPacket이 꺼낸 패킷을 담는 배열의 크기이고,
                                        // 헤더의 size가 이보다 크면 깨진 데이터로 본다.

// 공통 상수 추가 - 11/20 
constexpr uint16_t PROTOCOL_VERSION = 3; // 통신 규약 버전 (서로 같은 패킷 형식을 사용하는 지 확인 용)
                                         // 패킷 형식을 바꾸면 올린다. 11/27 : 3 (SC_JOIN_OK에 재접속 저장 위치 추가)
constexpr uint8_t SERVER_TICK_RATE = 20;  // 서버의 초당 갱신 횟수 (초당 20번 처리하는 기본 값)
constexpr int SERVER_TICK_MS = 1000 / SERVER_TICK_RATE; // 50ms (서버 타이머 실행 값)

// ---------------------------------------------------------------------------
// 패킷 종류 번호 : 헤더의 type에 들어간다
// ---------------------------------------------------------------------------
// enum - c 스타일 열거형
// 0으로 채워진 메모리를 패킷으로 착각하지 않게 하기 위해 0은 비워 두었다. 
enum PacketType : uint8_t {
    CS_MOVE_PLAYER   = 1,
    CS_BLOCK_CHANGE  = 2,

    SC_JOIN_OK       = 3,
    SC_ADD_PLAYER    = 4,
    SC_REMOVE_PLAYER = 5,
    // 6은 v.3의 SC_MOVE_PLAYER 자리. SC_SNAPSHOT으로 바꿔서 비워 둔다
    SC_BLOCK_CHANGE = 7,

    // v2 추가 (버전 확인, 거절 사유 전달, 틱 단위 상태 전달, 연결 감시)
    CS_HELLO = 8,
    SC_JOIN_REJECT = 9,
    SC_SNAPSHOT = 10,
    CS_PING = 11,

};

// 11/20 추가 - 거절 사유 정의
enum JoinRejectReason : uint8_t {
    JOIN_REJECT_FULL = 1,    // 서버 정원 초과
    JOIN_REJECT_VERSION = 2, // 프로토콜 버전 불일치
};

// ---------------------------------------------------------------------------
// 패킷 구조체
// ---------------------------------------------------------------------------
// #pragma pack(push, 1) ~ #pragma pack(pop)
// 이 범위 내의 컴파일러 최적화 방식중 패딩을 제거(멤버 사이에 빈 바이트 넣기)
//   컴파일러는 보통 float를 4의 배수 위치에 두려고 빈 바이트를 끼워 넣는다.(4단위로 읽기 때문)

// + 현재 아이템 UI(어떤 블록을 들지 결정)과 블럭 타입 변수의 순서가 약간 다르다...
// 내부적으로 꼬이진 않아서 바꿀 이유는 없지만, 만약 바꾼다면 이 헤더파일도 같이 수정해야한다.

#pragma pack(push, 1)

// 모든 패킷의 맨 앞 3바이트
struct PacketHeader {
    uint16_t size;  // 헤더를 포함한 패킷 전체 바이트 수 (= sizeof(그 패킷 구조체)) 
                    //가변 길이 패킷은 sizeof와 다를 수 있음
    uint8_t  type;  // PacketType 값
};

// 플레이어 한 명의 움직임 상태 (22바이트)
// CS_MOVE_PLAYER, SC_ADD_PLAYER, SC_SNAPSHOT에 똑같이 들어가므로, 서버는 받은 state를 통째로 저장했다가 그대로 보낸다.
struct PlayerState {
    float x = 0.0f, y = 0.0f, z = 0.0f;  // Character::Position (발 위치, 월드 좌표)
    float yaw = 0.0f;                    // camera.Yaw   (도 단위)
    float pitch = 0.0f;                  // camera.Pitch (도 단위, -89 ~ 89)
    uint8_t block_kind = 0;              // player->block_kind : 손에 든 블록의 칸 번호 0 ~ 8
                                         // BlockType 값이 아니라 Character::blocks[] 배열의 인덱스다.
    // 11/20 추가
    uint8_t flags = 0;      // 이동 모드: 0=걷기, 1=달리기, 2=웅크리기
};

// ----- 클라이언트 -> 서버 -----
// CS_MOVE_PLAYER (25바이트) : 클라이언트가 50ms마다 보낸다 (상태가 같으면 생략)
// 보낸 사람 id는 넣지 않는다. 서버는 어느 소켓(RecvThread)으로 왔는지 알 수 있음
// 서버 : 값을 확인해 저장하고(HandleMove), 틱마다 SC_SNAPSHOT으로 다른 접속자에게 보낸다
struct CS_MOVE_PLAYER_PACKET {
    PacketHeader header = { sizeof(CS_MOVE_PLAYER_PACKET), CS_MOVE_PLAYER };
    PlayerState state;
};

// CS_BLOCK_CHANGE (16바이트) : 블록을 부수거나 놓을 때 보내는 요청. 클라이언트는 SC_BLOCK_CHANGE를 받은 뒤에 바꾼다.
// 서버 : 좌표와 종류를 검사하고 worldDiff에 기록한 뒤(HandleBlockChange), 보낸 사람을 포함한 전원에게 SC_BLOCK_CHANGE를 보낸다.
struct CS_BLOCK_CHANGE_PACKET {
    PacketHeader header = { sizeof(CS_BLOCK_CHANGE_PACKET), CS_BLOCK_CHANGE };
    int32_t x = 0;                // 블록 한 칸의 월드 좌표 = 청크 번호 * 16 + 청크 안 칸 번호 (z도 같다)
    int32_t y = 0;                // 청크 -19 ~ 19이므로 x, z는 -304 ~ 319, 높이 y는 0 ~ 127
    int32_t z = 0;
    uint8_t block_type = 0;       // 바뀐 뒤의 BlockType 값 (blocktype.h). 부순 것은 AIR(0)
                                  // 놓은 것은 blocks[block_kind]다. 칸 번호 block_kind를 그대로 넣으면 안 된다
                                  // (칸 순서와 BlockType 순서가 다름).
};


// ----- 서버 -> 클라이언트 -----
// 
// SC_JOIN_OK (26바이트) : CS_HELLO의 버전이 맞으면 서버가 맨 먼저 보낸다 (QueueJoinData의 첫 패킷).
// 클라이언트 ConnectThread()는 최대 10초 동안 이 패킷(또는 SC_JOIN_REJECT)만 받고 header의 type과 size를 확인한다.
// 뒤따라오는 SC_BLOCK_CHANGE, SC_ADD_PLAYER는 소켓에 남아 있다가 RecvThread가 받는다.
struct SC_JOIN_OK_PACKET {
    PacketHeader header = { sizeof(SC_JOIN_OK_PACKET), SC_JOIN_OK };
    uint8_t id = 0;               // 서버가 정해 준 내 id (0 ~ MAX_PLAYERS - 1)
    int32_t seed = 0;             // 월드 시드. CreateWorld(seed) -> new World(seed)

    // 11/20 추가
    uint8_t tick_rate = SERVER_TICK_RATE; // 서버의 초당 갱신 횟수
    uint32_t initial_packet_count = 0;   // 뒤따르는 초기 패킷 총개수

    // 11/27 추가 - 재접속 저장 위치 : 같은 IP로 나갔던 적이 있으면 그 위치에서 시작한다
    uint8_t has_saved_pos = 0;    // 1 : 내 캐릭터를 saved_x, y, z에서 시작, 0 : 기본 시작 위치 (아래 값 무시)
    float saved_x = 0.0f, saved_y = 0.0f, saved_z = 0.0f;  // 나갈 때의 발 위치 (월드 좌표). 시선 · 든 블록은 처음 값으로 시작
};

// SC_ADD_PLAYER (27바이트) : 원격 플레이어를 만들라는 패킷. 두 경우에 보낸다.
//   1) 새 접속자에게 : 기존 플레이어마다 1개 (QueueJoinData). 서버가 마지막으로 받은 상태를 담는다.
//   2) 기존 접속자에게 : 방금 들어온 플레이어 1개 (HandleHello). 저장 위치에서 시작하면 has_moved = 1, 아니면 0
// 클라이언트 : 원격 Character를 만들고, has_moved가 1이면 ApplyRemoteState()로 state를 적용한다.
//              0이면 생성자가 정한 시작 지점에 그대로 둔다.
struct SC_ADD_PLAYER_PACKET {
    PacketHeader header = { sizeof(SC_ADD_PLAYER_PACKET), SC_ADD_PLAYER };
    uint8_t id = 0;
    uint8_t has_moved = 0;        // 1 : CS_MOVE_PLAYER를 한 번이라도 보냈거나 저장 위치에서 시작했다 -> state가 유효하다
                                  // 0 : 아직 보낸 적이 없다 -> state를 무시한다
    PlayerState state;
};

// 11/2O 추가
// 스냅샷에 들어가는 플레이어 한 명의 정보 (23바이트)
struct SnapshotEntry {
    uint8_t id = 0;     // 플레이어 ID
    PlayerState state;
};

// 헤더 3 + 서버 틱 4 + 인원수 1 = 8바이트
constexpr uint16_t SNAPSHOT_BASE_SIZE =
sizeof(PacketHeader) + sizeof(uint32_t) + sizeof(uint8_t);

// SC_SNAPSHOT: 서버가 한 틱의 다른 플레이어 상태를 묶어 보낸다 (BroadcastSnapshot).
// 길이가 바뀌는 패킷이다. header.size = SNAPSHOT_BASE_SIZE + count * sizeof(SnapshotEntry)
struct SC_SNAPSHOT_PACKET {
    PacketHeader header = {
        SNAPSHOT_BASE_SIZE,
        SC_SNAPSHOT
    };

    uint32_t tick = 0; // 서버 틱 번호
    uint8_t count = 0; // 아래 배열에 담긴 유효한 플레이어 수

    // 받는 사람 자신을 제외하므로 최대 2명
    SnapshotEntry players[MAX_PLAYERS - 1]{};
};

// SC_REMOVE_PLAYER (4바이트) : 누가 나갔다. 남은 접속자에게 보낸다 (HandleLeave).
// 클라이언트 : 그 id의 원격 Character를 지운다.
struct SC_REMOVE_PLAYER_PACKET {
    PacketHeader header = { sizeof(SC_REMOVE_PLAYER_PACKET), SC_REMOVE_PLAYER };
    uint8_t id = 0;
};

// SC_BLOCK_CHANGE (16바이트) : 블록 한 칸이 바뀌었다. 두 경우에 보낸다.
//   1) 승인한 CS_BLOCK_CHANGE : 보낸 사람까지 전원에게 (HandleBlockChange). 그래서 모두가 서버가 처리한 순서대로 맞춰진다.
//   2) 새 접속자에게 : worldDiff의 바뀐 칸마다 1개 (QueueJoinData)
// 클라이언트 : world->SetBlock(x, y, z, block_type)
struct SC_BLOCK_CHANGE_PACKET {
    PacketHeader header = { sizeof(SC_BLOCK_CHANGE_PACKET), SC_BLOCK_CHANGE };
    int32_t x = 0;                // 뜻은 CS_BLOCK_CHANGE와 같다
    int32_t y = 0;
    int32_t z = 0;
    uint8_t block_type = 0;
};

// 11/20 추가 패킷
// CS_HELLO (5바이트): 접속 직후 사용하는 프로토콜 버전을 알린다.
struct CS_HELLO_PACKET {
    PacketHeader header = {
        sizeof(CS_HELLO_PACKET),
        CS_HELLO
    };

    uint16_t version = PROTOCOL_VERSION;
};

// SC_JOIN_REJECT (4바이트): 접속을 거절한 이유를 알린다.
struct SC_JOIN_REJECT_PACKET {
    PacketHeader header = {
        sizeof(SC_JOIN_REJECT_PACKET),
        SC_JOIN_REJECT
    };

    uint8_t reason = 0; // 송신 전에 JoinRejectReason 값으로 설정
};

// CS_PING (3바이트): 클라이언트가 연결되어 있음을 알린다.
// 클라이언트 SendThread가 1초마다 전송한다.
// 서버 RecvThread는 입력 큐에 넣지 않고 버린다 (무응답 연결은 SO_SNDTIMEO 5분으로 끊는다).
struct CS_PING_PACKET {
    PacketHeader header = {
        sizeof(CS_PING_PACKET),
        CS_PING
    };
};

#pragma pack(pop)
// 여기서부터 다시 패딩 적용



// 패킷 크기를 못 박아 둔다. 누가 필드를 바꾸거나 pack을 지우면 여기서 컴파일 오류가 나서 바로 알 수 있다.
// 새 패킷을 추가하면 여기에도 한 줄 추가한다.
// 11/20 바이트 변경
static_assert(sizeof(PacketHeader) == 3, "PacketHeader must be 3 bytes");
static_assert(sizeof(PlayerState) == 22, "PlayerState must be 22 bytes");
static_assert(sizeof(CS_MOVE_PLAYER_PACKET) == 25, "CS_MOVE_PLAYER_PACKET must be 25 bytes");
static_assert(sizeof(CS_BLOCK_CHANGE_PACKET) == 16, "CS_BLOCK_CHANGE_PACKET must be 16 bytes");
static_assert(sizeof(SC_JOIN_OK_PACKET) == 26, "SC_JOIN_OK_PACKET must be 26 bytes");
static_assert(sizeof(SC_ADD_PLAYER_PACKET) == 27, "SC_ADD_PLAYER_PACKET must be 27 bytes");
static_assert(sizeof(SC_REMOVE_PLAYER_PACKET) == 4, "SC_REMOVE_PLAYER_PACKET must be 4 bytes");
static_assert(sizeof(SC_BLOCK_CHANGE_PACKET) == 16, "SC_BLOCK_CHANGE_PACKET must be 16 bytes");
static_assert(sizeof(SC_ADD_PLAYER_PACKET) <= MAX_PACKET_SIZE, "every packet must fit in MAX_PACKET_SIZE");
// 11/20 추가
static_assert(sizeof(CS_HELLO_PACKET) == 5, "CS_HELLO_PACKET must be 5 bytes");
static_assert(sizeof(SC_JOIN_REJECT_PACKET) == 4, "SC_JOIN_REJECT_PACKET must be 4 bytes");
static_assert(sizeof(SnapshotEntry) == 23, "SnapshotEntry must be 23 bytes");
static_assert(SNAPSHOT_BASE_SIZE == 8, "Snapshot base size must be 8 bytes");
static_assert(sizeof(SC_SNAPSHOT_PACKET) == SNAPSHOT_BASE_SIZE + sizeof(SnapshotEntry) * (MAX_PLAYERS - 1),
"SC_SNAPSHOT_PACKET size mismatch");
static_assert(sizeof(SC_SNAPSHOT_PACKET) <= MAX_PACKET_SIZE, "SC_SNAPSHOT_PACKET exceeds MAX_PACKET_SIZE");
static_assert(sizeof(CS_PING_PACKET) == 3, "CS_PING_PACKET must be 3 bytes");


// ---------------------------------------------------------------------------
// ExtractPacket : recv()로 쌓아 둔 바이트에서 완성된 패킷 하나를 꺼낸다
// ---------------------------------------------------------------------------
// 설계안의 NeedMore · Ok · Corrupt와 같다
enum ExtractResult {
    EXTRACT_INCOMPLETE,  // 데이터 부족
    EXTRACT_COMPLETE,    // 패킷 하나 추출 완료
    EXTRACT_INVALID,     // 잘못된 패킷 크기
};

// buf : recv()로 받은 바이트를 이어 붙여 둔 버퍼. 앞에서부터 len바이트가 들어 있다.
// len : buf에 들어 있는 바이트 수. 패킷을 꺼내면 그만큼 줄어든다 (그래서 참조로 받는다).
// out : 꺼낸 패킷을 복사해 줄 곳. MAX_PACKET_SIZE바이트 이상이어야 한다.
// 반환 : EXTRACT_INCOMPLETE = 데이터 부족, 버퍼 유지
//        EXTRACT_COMPLETE = 패킷 추출 완료
//        EXTRACT_INVALID = 잘못된 크기, 버퍼 초기화
// EXTRACT_INVALID면 호출한 쪽에서 연결 종료
inline ExtractResult ExtractPacket(char* buf, int& len, char* out)
{
    // 1) 헤더 3바이트도 안 온 경우
    if (len < static_cast<int>(sizeof(PacketHeader)))
        return EXTRACT_INCOMPLETE;

    // 2) 헤더에서 이 패킷의 전체 크기를 읽는다
    //    char 배열을 구조체 포인터로 바꿔 읽는 대신 지역 변수로 복사해서 읽는다
    PacketHeader header;
    memcpy(&header, buf, sizeof(header));
    const int size = header.size;

    // 3) 잘못된 크기
    if (size < static_cast<int>(sizeof(PacketHeader)) || size > MAX_PACKET_SIZE) {
        len = 0;
        return EXTRACT_INVALID;
    }

    // 4) 패킷 끝까지 아직 안 온 경우
    if (len < size)
        return EXTRACT_INCOMPLETE;

    // 5) 완성된 패킷을 out에 복사하고, 뒤에 남은 바이트를 buf 맨 앞으로 당긴다.
    //    당길 때는 원본과 복사할 곳이 겹치므로 memcpy가 아니라 memmove를 쓴다.
    memcpy(out, buf, size);
    len -= size;
    memmove(buf, buf + size, len);
    return EXTRACT_COMPLETE;
}


// 버퍼 전체 전송 (설계안의 SendAll). 송신 버퍼에 이어 붙인 패킷들을 끝까지 보내고, 실패하면 false
inline bool SendBuffer(SOCKET sock, const void* buffer, int len)
{
    // 입력값 검사
    if (len < 0 || (buffer == nullptr && len > 0))
        return false;

    const char* data = static_cast<const char*>(buffer);
    int sent = 0;
    // 남은 데이터를 이어서 전송
    while (sent < len) {
        const int result = send(sock, data + sent, len - sent, 0);
        if (result <= 0)
            return false;
        sent += result;
    }
    return true;
}


// ---------------------------------------------------------------------------
// SendPacket : 패킷 하나를 통째로 보낸다
// ---------------------------------------------------------------------------
// sock   : 보낼 소켓
// packet : 보낼 패킷 구조체의 주소 (&p). 맨 앞 헤더의 size만큼 보낸다.
// 반환   : true = 다 보냈다, false = 실패했다 (연결이 끊겼거나 헤더가 잘못됐다)
//
// 사용 예 (클라이언트 SendMove)
//     CS_MOVE_PLAYER_PACKET p;                  // header는 이미 { 25, CS_MOVE_PLAYER }로 채워져 있다
//     p.state.x = player->Position.x;
//     ...
//     SendPacket(sock, &p);
//
// send()는 요청한 길이보다 적게 보내고 돌아올 수도 있어서, 다 보낼 때까지 반복
// 같은 소켓에 두 스레드가 동시에 보내면 바이트가 섞일 수 있다.
// 그래서 서버는 Send 스레드가 없는 소켓(RejectConnection)에만 쓰고, 세션의 패킷은 송신 버퍼를 거쳐 SendBuffer()로 보낸다
inline bool SendPacket(SOCKET sock, const void* packet)
{
    PacketHeader header;
    memcpy(&header, packet, sizeof(header));
    const int size = header.size;

    // 헤더가 잘못된 패킷을 보내면 받는 쪽의 데이터가 깨지므로 보내지 않는다
    if (size < static_cast<int>(sizeof(PacketHeader)) || size > MAX_PACKET_SIZE)
        return false;

    const char* data = static_cast<const char*>(packet);
    int sent = 0;
    while (sent < size) {
        const int result = send(sock, data + sent, size - sent, 0);
        if (result <= 0)              // SOCKET_ERROR(-1) : 연결이 끊겼거나 오류
            return false;
        sent += result;
    }
    return true;
}


#endif
