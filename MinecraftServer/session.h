// ============================================================================
// session.h : 세션 표 (Session · g_sessions)
// ============================================================================
// 세션 하나 = 접속 한 건의 수명. OpenSession()이 열고 CleanupClients()가 닫는다.
// 슬롯 번호 = 플레이어 id (0 ~ MAX_PLAYERS - 1). 한 슬롯을 서버가 끝날 때까지 여러 세션이 돌려 쓴다.
// 구현은 session.cpp. server.h가 이 파일을 include하므로 서버의 .cpp는 server.h만 include하면 된다.
//
// 상태 : 바꾸는 함수 (스레드)
//   FREE    -> JOINING  OpenSession()    (Accept)  접속을 받아 등록하고 Recv · Send를 만든다
//   JOINING -> FREE     OpenSession()    (Accept)  Recv · Send 스레드를 만들지 못했을 때만
//   JOINING -> PLAYING  HandleHello()    (Logic)   버전이 맞아 입장 데이터를 예약했다
//   JOINING -> CLOSING  HandleLeave()    (Logic)   입장 전에 끊겼다 (버전 거절 포함). 다른 플레이어에게 알리지 않는다
//   PLAYING -> CLOSING  HandleLeave()    (Logic)   게임 중에 끊겼다. 남은 플레이어에게 SC_REMOVE_PLAYER
//   CLOSING -> FREE     CleanupClients() (Logic)   Recv · Send가 모두 끝난 뒤 소켓 · 스레드 핸들을 닫는다
// 상태를 바꾸는 스레드는 Accept와 Logic 둘뿐이다. Recv · Send는 RequestDisconnect()로 closing만 켠다.
//
// 필드마다 지키는 방법
//   g_sessionCS  state · sock · ip · gen · hRecvThread · hSendThread   세션 표 전체에 1개
//   sendCS       sendBuf                                               세션마다 1개
//   잠금 없음    hSendEvent   main()이 만들고 끝날 때 닫는다. 그 사이에는 바뀌지 않는다
//                closing      std::atomic
//
// 잠금 규칙 (server.h 맨 위와 같다)
//   g_sessionCS와 sendCS는 겹쳐 잡지 않는다. 잠금 안에서 Log()를 부르지 않는다.
//   잠금 안에서는 값을 복사하거나 바꾸기만 하고, send() · recv() · select() · CreateThread() · 출력은 밖에서 한다.
// ============================================================================

#ifndef SESSION_H
#define SESSION_H

#include "protocol.h"       // MAX_PLAYERS, winsock2.h (SOCKET · HANDLE · CRITICAL_SECTION)
#include <ws2tcpip.h>       // INET_ADDRSTRLEN
#include <atomic>
#include <cstdint>
#include <vector>


// ---------------------------------------------------------------------------
// 세션 상태
// ---------------------------------------------------------------------------
// 순서를 바꾸면 PrintStatus()의 STATE_NAMES도 같이 바꾼다
enum class SessionState : uint8_t {
    FREE,       // 빈 슬롯. FindEmptySlot()이 찾는다
    JOINING,    // 접속은 받았고 CS_HELLO를 기다리는 중. 다른 플레이어는 아직 모른다
    PLAYING,    // 입장을 마쳤다. 스냅샷 · 블록 변경 · 입퇴장 알림을 받는다 (QueueToAll의 대상)
    CLOSING,    // 퇴장 처리를 마치고 Recv · Send가 끝나길 기다리는 중. FREE가 아니므로 새 접속에 주지 않는다
};


// ---------------------------------------------------------------------------
// 세션 : 세션 표의 한 칸
// ---------------------------------------------------------------------------
struct Session {
    // g_sessionCS 안에서 읽고 쓴다
    SessionState state = SessionState::FREE;
    SOCKET   sock = INVALID_SOCKET;         // OpenSession()이 넣는다. 닫는 곳은 CleanupClients() 한 곳뿐
    char     ip[INET_ADDRSTRLEN] = {};      // 상대 IPv4 주소 문자열 ("192.168.0.2"). OpenSession()이 넣는다
                                            // 재접속 위치를 찾는 열쇠 : HandleHello()가 찾고 HandleLeave()가 기록한다
                                            // OpenSession()이 매번 덮어쓰므로 FREE로 되돌릴 때 지우지 않는다
    uint32_t gen = 0;                       // 세대 번호. OpenSession()마다 +1. 이미 정리된 세션이 늦게 넣은 입력을 가려낸다
    HANDLE   hRecvThread = nullptr;         // 이 세션의 Recv · Send 스레드. OpenSession()이 넣고 CleanupClients()가 닫는다
    HANDLE   hSendThread = nullptr;

    // sendCS 안에서 읽고 쓴다
    CRITICAL_SECTION  sendCS;               // main()이 슬롯마다 한 번 만들고 서버가 끝날 때 지운다
    std::vector<char> sendBuf;              // Logic이 이어 붙이고(QueuePacket), Send가 통째로 꺼낸다(swap)

    // 잠금 없이 쓴다
    HANDLE hSendEvent = nullptr;            // 자동 리셋. "보낼 것이 생겼다". main()이 슬롯마다 만들고 끝날 때 닫는다
    std::atomic<bool> closing{ false };     // RequestDisconnect()가 켜고 OpenSession()이 끈다. Recv · Send가 보고 스스로 끝난다
};

extern Session          g_sessions[MAX_PLAYERS];   // 세션 표. 정의는 session.cpp
extern CRITICAL_SECTION g_sessionCS;               // 세션 표 전체에 1개. main()이 만들고 지운다


// ---------------------------------------------------------------------------
// 함수 선언 (session.cpp) : 실행되는 스레드, 잡는 임계 영역
// ---------------------------------------------------------------------------
int  FindEmptySlot();                                   // Accept 스레드, g_sessionCS 안에서 부른다
int  OpenSession(SOCKET sock, const char* ip);          // Accept 스레드, g_sessionCS · sendCS (겹치지 않게)
void QueuePacket(int id, const void* packet);           // Logic 스레드, sendCS
void QueueToAll(const void* packet, int except);        // Logic 스레드, g_sessionCS · sendCS (겹치지 않게). except = 뺄 id, -1이면 전원
void RequestDisconnect(int id);                         // Recv · Send · Logic · Accept 스레드. 잠금 없음
void CleanupClients();                                  // Logic 스레드, g_sessionCS · sendCS (겹치지 않게)


#endif
