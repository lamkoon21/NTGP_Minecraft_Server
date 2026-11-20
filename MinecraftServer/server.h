// ============================================================================
// server.h : 서버 파일들이 함께 쓰는 선언
// ============================================================================
// 파일
//   protocol.h   패킷 약속. 클라이언트 저장소와 같은 파일, 수정시 같이 고쳐야함
//   server.h     서버 파일들이 함께 쓰는 선언 (이 파일)
//   main.cpp     켜기 · 끄기, 콘솔 명령, 로그
//   net_io.cpp   Accept · Recv · Send 스레드
//   session.cpp  세션 표 · 송신 버퍼
//   logic.cpp    Logic 스레드 · 게임 상태
//
// 스레드 - 3 + 2 x 접속자 수 (정원 3명이면 9개)
//   main          콘솔 입력            Accept · Logic 생성. quit이면 종료 이벤트를 켜고 모두 기다린 뒤 정리
//   AcceptThread  select() 0.2초       새 접속을 빈 슬롯에 등록하고 Recv · Send 생성. 자리가 없으면 거절
//   RecvThread    select() 0.2초       세션마다 1개. 받은 패킷을 입력 큐에 넣기만
//   LogicThread   종료 이벤트 · 50ms   입력을 받은 순서대로 처리. 게임 상태를 바꾸는 유일한 스레드. 결과는 송신 버퍼에
//   SendThread    종료 · 송신 이벤트   세션마다 1개. 송신 버퍼를 통째로 꺼내 send()
//
// 임계 영역 - 안에서는 넣기 · 꺼내기 · 값 복사 · 상태 바꾸기만 한다.
//             send() · recv() · select() · 출력은 모두 밖에서 하고, 잠금을 겹쳐 잡지 않는다.
//   g_inQueueCS   입력 큐 g_inQueue      Recv x 3 -> Logic           push_back 한 번 / swap 한 번
//   sendCS        세션마다 송신 버퍼     Logic -> 그 세션의 Send     이어 붙이기 / swap 한 번
//   g_sessionCS   세션 표 g_sessions     Accept(등록) <-> Logic(상태 변경 · 회수)
//   g_logCS       콘솔 출력              모든 스레드                 한 줄 출력
//   없음          게임 상태              Logic만 (logic.cpp 안의 static)
//
// 소켓은 그 세션의 Recv · Send가 모두 끝난 뒤 CleanupClients() 한 곳에서만 닫는다.
// 예외는 세션이 되기 전이거나 다른 스레드가 없을 때뿐 - RejectConnection(), OpenSession() 실패, main()의 마지막 정리
// ============================================================================

#ifndef SERVER_H
#define SERVER_H

#include "protocol.h"
#include <ws2tcpip.h>   // INET_ADDRSTRLEN
#include <atomic>
#include <cstdint>
#include <vector>


// ---------------------------------------------------------------------------
// 서버 상수
// ---------------------------------------------------------------------------
// 틱 속도는 protocol.h의 SERVER_TICK_RATE(초당 20틱) · SERVER_TICK_MS(50ms)를 쓴다. SC_JOIN_OK로 클라이언트에 알린다.
constexpr int    SELECT_TIMEOUT_MS = 200;           // Accept · Recv의 select() 한 번 대기. 이 간격으로 종료 신호를 본다
constexpr DWORD  SEND_TIMEOUT_MS = 5 * 60 * 1000;   // SO_SNDTIMEO 5분. send()가 이보다 오래 끝나지 않으면 실패로 돌아온다
                                                    // 무응답 연결을 끊는 기준은 이것 하나다 (5초 무응답 검사는 없앰)
constexpr size_t MAX_SEND_BUFFER = 1024 * 1024;     // 송신 대기가 1MB를 넘으면 받지 못하는 클라이언트로 보고 끊는다


// ---------------------------------------------------------------------------
// 세션 표 : 슬롯 번호 = 플레이어 id
// ---------------------------------------------------------------------------
// FREE -> JOINING (OpenSession, 접속 받음) -> PLAYING (HandleHello, 입장 데이터 예약)
//      -> CLOSING (HandleLeave) -> FREE (CleanupClients, Recv · Send가 모두 끝난 뒤)
enum class SessionState : uint8_t { FREE, JOINING, PLAYING, CLOSING };

struct Session {
    // g_sessionCS 안에서 읽고 쓴다
    SessionState state = SessionState::FREE;
    SOCKET   sock = INVALID_SOCKET;
    char     ip[INET_ADDRSTRLEN] = {};      // 상대 IPv4 주소 문자열. 재접속 위치를 찾는 열쇠 (HandleHello가 찾고 HandleLeave가 기록)
    uint32_t gen = 0;                       // 세대 번호. OpenSession()마다 +1. 이미 정리된 세션이 늦게 넣은 입력을 가려낸다
    HANDLE   hRecvThread = nullptr;
    HANDLE   hSendThread = nullptr;

    // sendCS 안에서 읽고 쓴다
    CRITICAL_SECTION  sendCS;
    std::vector<char> sendBuf;              // Logic이 이어 붙이고(QueuePacket), Send가 통째로 꺼낸다

    // 잠금 없이 쓴다
    HANDLE hSendEvent = nullptr;            // 자동 리셋. "보낼 것이 생겼다". main()이 만들고 끝날 때 닫는다
    std::atomic<bool> closing{ false };     // RequestDisconnect()가 켠다. Recv · Send가 보고 스스로 끝난다
};

extern Session          g_sessions[MAX_PLAYERS];
extern CRITICAL_SECTION g_sessionCS;


// ---------------------------------------------------------------------------
// 입력 큐 : Recv -> Logic
// ---------------------------------------------------------------------------
enum class InputKind : uint8_t { PACKET, LEAVE };

struct InputMsg {
    int       id = 0;                       // 슬롯 번호
    uint32_t  gen = 0;                      // 넣을 때의 세대 번호. 지금 슬롯의 것과 다르면 Logic이 버린다
    InputKind kind = InputKind::PACKET;     // PACKET = 받은 패킷, LEAVE = 연결이 끝남
    char      packet[MAX_PACKET_SIZE] = {}; // kind가 PACKET일 때만. 헤더부터 시작하는 패킷 하나
};

extern std::vector<InputMsg> g_inQueue;
extern CRITICAL_SECTION      g_inQueueCS;


// ---------------------------------------------------------------------------
// 그 밖의 공유 데이터
// ---------------------------------------------------------------------------
extern HANDLE           g_hQuitEvent;   // 수동 리셋. main()이 켜면 모든 스레드가 자기 흐름의 "종료 신호?"에서 끝난다
extern CRITICAL_SECTION g_logCS;        // 콘솔 출력

// 틱 통계 : LogicThread가 쓰고 PrintStatus()가 읽는다. atomic이라 잠금이 없다
struct TickStats {
    std::atomic<uint32_t> tick{ 0 };    // 마지막으로 처리한 틱 번호
    std::atomic<uint32_t> avgUs{ 0 };   // 틱 처리 평균 시간 (마이크로초)
    std::atomic<uint32_t> maxUs{ 0 };   // 틱 처리 최대 시간 (마이크로초)
};
extern TickStats g_tickStats;


// ---------------------------------------------------------------------------
// 함수 선언 : 실행되는 스레드, 잡는 임계 영역
// ---------------------------------------------------------------------------

// main.cpp
void Log(const char* fmt, ...);                         // 모든 스레드. printf 형식 한 줄 출력, g_logCS
void PrintStatus();                                     // main 스레드. status 명령

// net_io.cpp
DWORD WINAPI AcceptThread(LPVOID arg);                  // Accept 스레드. arg = 리슨 소켓
void RejectConnection(SOCKET sock, uint8_t reason);     // Accept 스레드. reason = JoinRejectReason
DWORD WINAPI RecvThread(LPVOID arg);                    // Recv 스레드. arg = 슬롯 id
void PushInput(int id, uint32_t gen, InputKind kind, const char* packet);   // Recv 스레드, g_inQueueCS
DWORD WINAPI SendThread(LPVOID arg);                    // Send 스레드. arg = 슬롯 id

// session.cpp
int  FindEmptySlot();                                   // Accept 스레드, g_sessionCS 안에서 부른다
int  OpenSession(SOCKET sock, const char* ip);          // Accept 스레드, g_sessionCS · sendCS (겹치지 않게)
void QueuePacket(int id, const void* packet);           // Logic 스레드, sendCS
void QueueToAll(const void* packet, int except);        // Logic 스레드. except = 뺄 id, -1이면 전원
void RequestDisconnect(int id);                         // Recv · Send · Logic · Accept 스레드. 잠금 없음
void CleanupClients();                                  // Logic 스레드, g_sessionCS

// logic.cpp
DWORD WINAPI LogicThread(LPVOID arg);                   // Logic 스레드. arg = 월드 시드
void ProcessInput(const InputMsg& msg);                 // 이하 모두 Logic 스레드
void HandleHello(int id, const char* packet);
void QueueJoinData(int id);
void HandleMove(int id, const char* packet);
void HandleBlockChange(int id, const char* packet);
void HandleLeave(int id);
void BroadcastSnapshot(uint32_t tick);


#endif
