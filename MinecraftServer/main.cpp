// ============================================================================
// main.cpp : 마인크래프트 멀티플레이 서버 (콘솔 프로그램)
// ============================================================================
// 게임 규칙 없이 위치와 블록 변경만 중계한다.
//
// 스레드
//   메인 스레드        main()         : 접속을 받고 입장 처리
//   클라이언트 스레드  ClientThread() : 접속마다 1개, 받은 패킷을 바로 중계
//
// 여러 스레드가 함께 쓰는 전역에는 g_ 접두사를 붙이고, 모두 g_cs 안에서만 다룬다.
// send()도 모두 g_cs 안에서 해서, 두 스레드가 같은 소켓에 동시에 보내지 않게 한다.
//
// protocol.h는 클라이언트 저장소(Minecraft/header/protocol/protocol.h)와 같은 파일이다.
// 패킷을 바꿀 때는 두 저장소의 protocol.h를 함께 고친다.
// ============================================================================

#include "protocol.h"   // winsock2.h를 include한다 (windows.h보다 먼저 와야 한다)
#include <ws2tcpip.h>   // inet_ntop
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <tuple>


// ---------------------------------------------------------------------------
// 공유 데이터 : 모두 g_cs 안에서만 읽고 쓴다
// ---------------------------------------------------------------------------

// 플레이어 슬롯 하나. 슬롯 번호가 곧 플레이어 id다.
struct Player {
    SOCKET      sock = INVALID_SOCKET;  // 이 플레이어와 연결된 소켓
    bool        in_use = false;         // 슬롯 사용 여부
    bool        has_moved = false;      // CS_MOVE_PLAYER를 한 번이라도 받았는지 (SC_ADD_PLAYER의 has_moved)
    PlayerState state;                  // 마지막으로 받은 위치, 시선, 든 블록
};

Player g_players[MAX_PLAYERS];

// 바뀐 블록 : 블록 월드 좌표 (x, y, z) -> 마지막으로 바뀐 BlockType. 좌표마다 마지막 변경 하나만 남긴다.
std::map<std::tuple<int32_t, int32_t, int32_t>, uint8_t> g_worldDiff;

// 월드 시드 : 서버를 시작할 때 한 번 정하고, 입장하는 클라이언트에게 SC_JOIN_OK로 보낸다.
int32_t g_seed = 0;

// 위의 공유 데이터와 모든 send()를 보호한다.
CRITICAL_SECTION g_cs;


// ---------------------------------------------------------------------------
// 함수 선언
// ---------------------------------------------------------------------------
int FindEmptySlot();                              // 메인 스레드, g_cs 안
DWORD WINAPI ClientThread(LPVOID arg);            // 클라이언트 스레드. arg = 슬롯 id
void ProcessPacket(int id, const char* packet);   // 클라이언트 스레드, g_cs 안
void SendJoinData(int id);                        // 메인 스레드, g_cs 안
void SendToAll(const void* packet, int except);   // g_cs 안. except = 뺄 id, -1이면 전원


// ---------------------------------------------------------------------------
// main : 서버 시작, 접속 받기와 입장 처리
// ---------------------------------------------------------------------------
int main()
{
    // 1) 윈속 초기화
    WSADATA wsa;
    const int startup = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (startup != 0) {
        std::cerr << "WSAStartup() 실패, 오류 코드 " << startup << '\n';
        return 1;
    }

    // 2) 리슨 소켓 : socket() -> bind() -> listen()
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "socket() 실패, 오류 코드 " << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

    sockaddr_in serveraddr = {};
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);   // 이 컴퓨터의 모든 IP로 오는 접속을 받는다
    serveraddr.sin_port = htons(SERVER_PORT);
    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&serveraddr), sizeof(serveraddr)) == SOCKET_ERROR) {
        std::cerr << "bind() 실패, 오류 코드 " << WSAGetLastError()
                  << " (포트 " << SERVER_PORT << "를 다른 프로그램이 쓰고 있는지 확인)\n";
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() 실패, 오류 코드 " << WSAGetLastError() << '\n';
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    // 3) 월드 시드 : 싱글플레이의 dis(gen)과 같은 범위(0 ~ 1000)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 1000);
    g_seed = dis(gen);

    // 4) 임계 영역
    InitializeCriticalSection(&g_cs);

    std::cout << "서버 시작 : 포트 " << SERVER_PORT << ", 월드 시드 " << g_seed
              << ", 정원 " << MAX_PLAYERS << "명\n";

    // 5) 접속 받기
    while (true) {
        sockaddr_in clientaddr = {};
        int addrlen = sizeof(clientaddr);
        SOCKET client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&clientaddr), &addrlen);
        if (client_sock == INVALID_SOCKET) {
            std::cerr << "accept() 실패, 오류 코드 " << WSAGetLastError() << '\n';
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof(ip));
        const int port = ntohs(clientaddr.sin_port);

        // 이동 패킷이 1/60초마다 작게 오간다. Nagle 알고리즘을 끄지 않으면
        // 작은 패킷을 모았다가 보내서 중계가 늦어지므로 TCP_NODELAY를 켠다.
        BOOL nodelay = TRUE;
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)) == SOCKET_ERROR) {
            std::cerr << "setsockopt(TCP_NODELAY) 실패, 오류 코드 " << WSAGetLastError() << " (계속 진행)\n";
        }

        // 6) 빈 슬롯 찾기부터 입장 알림까지 한 임계 영역 안에서 한다.
        //    그래서 두 명이 동시에 들어와도 같은 슬롯을 받지 않고,
        //    새 접속자는 입장 데이터를 다 받은 뒤에야 그 이후의 변경을 받는다.
        EnterCriticalSection(&g_cs);

        const int id = FindEmptySlot();
        if (id == -1) {
            LeaveCriticalSection(&g_cs);
            closesocket(client_sock);   // 클라이언트는 recv()가 0을 받아 접속 실패로 처리한다
            std::cout << "[접속 거절] " << ip << ':' << port << " : 정원(" << MAX_PLAYERS << "명) 초과\n";
            continue;
        }

        // 슬롯에 등록
        g_players[id].sock = client_sock;
        g_players[id].in_use = true;
        g_players[id].has_moved = false;
        g_players[id].state = PlayerState{};

        // 새 접속자에게 : SC_JOIN_OK, 바뀐 블록, 기존 플레이어
        SendJoinData(id);

        // 다른 접속자에게 : 새 플레이어. 아직 움직인 적이 없으므로 has_moved = 0
        SC_ADD_PLAYER_PACKET add;
        add.id = static_cast<uint8_t>(id);
        add.has_moved = 0;
        SendToAll(&add, id);

        LeaveCriticalSection(&g_cs);

        std::cout << "[접속] " << ip << ':' << port << " -> id " << id << '\n';

        // 7) 이 접속을 맡을 클라이언트 스레드를 만든다.
        //    슬롯 번호(id)를 넘기고, 소켓은 스레드가 g_players[id].sock에서 꺼낸다.
        HANDLE thread = CreateThread(nullptr, 0, ClientThread,
                                     reinterpret_cast<LPVOID>(static_cast<intptr_t>(id)), 0, nullptr);
        if (thread == nullptr) {
            std::cerr << "CreateThread() 실패, 오류 코드 " << GetLastError() << " : id " << id << " 연결을 닫음\n";

            // 다른 접속자에게 이미 SC_ADD_PLAYER를 보냈으므로 퇴장 처리까지 한다
            EnterCriticalSection(&g_cs);
            closesocket(g_players[id].sock);
            g_players[id].sock = INVALID_SOCKET;
            g_players[id].in_use = false;

            SC_REMOVE_PLAYER_PACKET remove;
            remove.id = static_cast<uint8_t>(id);
            SendToAll(&remove, id);
            LeaveCriticalSection(&g_cs);
            continue;
        }

        // 스레드가 끝나기를 기다리지 않으므로 핸들만 바로 닫는다 (스레드는 계속 돈다)
        CloseHandle(thread);
    }

    // 위 반복은 끝나지 않는다 (서버는 콘솔 창을 닫아 끝낸다).
    // 반복을 끝내는 조건을 넣게 되면 아래 순서로 정리한다.
    closesocket(listen_sock);
    DeleteCriticalSection(&g_cs);
    WSACleanup();
    return 0;
}


// ---------------------------------------------------------------------------
// FindEmptySlot : g_players에서 비어 있는 첫 슬롯 번호를 찾는다
// ---------------------------------------------------------------------------
// 반드시 g_cs 안에서 부른다. main()이 같은 임계 영역 안에서 바로 등록하므로
// 두 명이 동시에 접속해도 같은 슬롯을 받지 않는다.
// 찾기만 하고 g_players는 바꾸지 않는다.
// 반환 : 0 ~ MAX_PLAYERS - 1 = 빈 슬롯 번호, -1 = 빈 슬롯 없음 (정원 초과)
int FindEmptySlot()
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (!g_players[i].in_use)
            return i;
    }
    return -1;
}


// ===========================================================================
// 아래는 아직 구현하지 않은 함수다. 빌드가 되도록 빈 본문만 두었다.
// 설명은 설계서의 서버 함수 표를 따른다. 구현할 때 (void) 줄과 TODO를 지운다.
// ===========================================================================

// ---------------------------------------------------------------------------
// ClientThread : 접속마다 1개. arg = 슬롯 id (main()이 intptr_t로 바꿔 넘긴다)
// ---------------------------------------------------------------------------
// 맡은 소켓(g_players[id].sock)에서 recv() -> ExtractPacket() -> ProcessPacket()을 반복한다.
// recv()가 0 이하면 g_cs 안에서 소켓을 닫고 슬롯을 비운 뒤,
// 다른 접속자에게 SC_REMOVE_PLAYER를 보내고 끝낸다.
DWORD WINAPI ClientThread(LPVOID arg)
{
    // TODO: 구현. 구현 전까지는 접속이 끊겨도 슬롯이 비워지지 않는다.
    (void)arg;
    return 0;
}

// ---------------------------------------------------------------------------
// ProcessPacket : 받은 패킷 하나를 처리한다 (클라이언트 스레드, g_cs 안)
// ---------------------------------------------------------------------------
// CS_MOVE_PLAYER  : g_players[id]에 state 저장, has_moved = true,
//                   보낸 사람을 뺀 접속자에게 SC_MOVE_PLAYER로 중계
// CS_BLOCK_CHANGE : 좌표(청크 -19 ~ 19, 높이 0 ~ 127)와 블록 종류를 검사한 뒤
//                   g_worldDiff에 기록하고, 보낸 사람을 포함한 전원에게 SC_BLOCK_CHANGE
void ProcessPacket(int id, const char* packet)
{
    // TODO: 구현
    (void)id;
    (void)packet;
}

// ---------------------------------------------------------------------------
// SendJoinData : 새 접속자에게 입장 데이터를 보낸다 (메인 스레드, g_cs 안)
// ---------------------------------------------------------------------------
// SC_JOIN_OK(id, g_seed) 1개, g_worldDiff의 바뀐 칸마다 SC_BLOCK_CHANGE,
// 기존 플레이어마다 마지막 상태를 담은 SC_ADD_PLAYER를 보낸다.
// main()이 슬롯을 등록한 뒤에 부르므로, 기존 플레이어를 보낼 때 자기 자신(id)은 뺀다.
void SendJoinData(int id)
{
    // TODO: 구현
    (void)id;
}

// ---------------------------------------------------------------------------
// SendToAll : 사용 중인 모든 슬롯에 같은 패킷을 보낸다 (g_cs 안)
// ---------------------------------------------------------------------------
// except : 뺄 접속자 id. -1이면 전원에게 보낸다.
// 이동 중계와 입퇴장 알림은 당사자를 빼고, 블록 변경은 전원에게 보낸다.
void SendToAll(const void* packet, int except)
{
    // TODO: 구현
    (void)packet;
    (void)except;
}
