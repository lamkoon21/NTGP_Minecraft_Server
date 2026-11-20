// 서버 켜기 · 끄기 (main 스레드)
//
// main()은 초기화 후 AcceptThread와 LogicThread를 만들고 콘솔 명령(quit · status)만 기다린다.
// quit이면 g_hQuitEvent를 켠다. 모든 스레드는 자기 흐름의 "종료 신호?"에서 스스로 끝나고,
// main()은 그것을 기다린 뒤 소켓 · 핸들 · 임계 영역을 정리한다.
// v.3에서 main()이 하던 접속 받기는 AcceptThread(net_io.cpp)로 옮겼다. 스레드와 임계 영역 전체 그림은 server.h 맨 위
// ============================================================================

#include "server.h"
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>


// ---------------------------------------------------------------------------
// 공유 데이터
// ---------------------------------------------------------------------------
HANDLE           g_hQuitEvent = nullptr;
CRITICAL_SECTION g_logCS;


// ---------------------------------------------------------------------------
// main : 서버 켜기 -> 콘솔 명령 -> 끄기
// ---------------------------------------------------------------------------
int main()
{
    // 1) 윈속 초기화
    //    스레드를 만들기 전까지는 출력하는 스레드가 하나뿐이라 Log() 대신 std::cerr를 쓴다
    WSADATA wsa;
    const int startup = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (startup != 0) {
        std::cerr << "WSAStartup() 실패, 오류 코드 " << startup << '\n';
        return 1;
    }

    // 2) 리슨 소켓 : socket() -> SO_REUSEADDR -> bind() -> listen()
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "socket() 실패, 오류 코드 " << WSAGetLastError() << '\n';
        WSACleanup();
        return 1;
    }

    // 서버를 껐다가 바로 다시 켜도 bind()가 실패하지 않게 한다.
    // Windows에서는 이 옵션 때문에 서버를 두 개 켜도 둘 다 bind()에 성공하므로, 서버는 하나만 켠다.
    BOOL reuse = TRUE;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == SOCKET_ERROR) {
        std::cerr << "setsockopt(SO_REUSEADDR) 실패, 오류 코드 " << WSAGetLastError() << " (계속 진행)\n";
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
    //    켤 때마다 새로 만들고 파일로 남기지 않는다 (월드 변경 · 저장 위치도 서버를 끄면 사라진다).
    //    LogicThread에 넘기면 이후로는 Logic만 쓴다 (입장 때 SC_JOIN_OK로 보냄)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 1000);
    const int32_t seed = dis(gen);

    // 4) 잠금 · 이벤트
    //    종료 이벤트는 수동 리셋 : 한 번 켜면 기다리는 모든 스레드가 계속 켜진 것으로 본다
    //    송신 이벤트는 자동 리셋 : 깨어난 Send 스레드가 한 번 보면 다시 꺼진다
    //    슬롯마다의 sendCS와 송신 이벤트는 서버가 끝날 때까지 그 슬롯에 들어오는 세션들이 돌려 쓴다
    InitializeCriticalSection(&g_logCS);
    InitializeCriticalSection(&g_sessionCS);
    InitializeCriticalSection(&g_inQueueCS);

    // CreateEvent (보안, 수동 ResetEvent 호출 여부, 초기상태, 이벤트 객체 이름 설정)
    g_hQuitEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    bool ok = (g_hQuitEvent != nullptr);
    for (Session& s : g_sessions) {
        InitializeCriticalSection(&s.sendCS);
        s.hSendEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (s.hSendEvent == nullptr)
            ok = false;
    }
    if (!ok)
        Log("CreateEvent() 실패");

    // 이 아래로는 실패해도 바로 return하지 않고 7)의 종료 · 정리를 거친다 (ok = false)

    // 5) Accept · Logic 스레드 만들기
    //    리슨 소켓은 Accept에게, 시드는 Logic에게 인자로 넘긴다
    HANDLE hAccept = nullptr;
    HANDLE hLogic = nullptr;
    if (ok) {
        hAccept = CreateThread(nullptr, 0, AcceptThread, reinterpret_cast<LPVOID>(listen_sock), 0, nullptr);
        if (hAccept != nullptr)
            hLogic = CreateThread(nullptr, 0, LogicThread,
                                  reinterpret_cast<LPVOID>(static_cast<intptr_t>(seed)), 0, nullptr);

        if (hAccept == nullptr || hLogic == nullptr) {
            Log("CreateThread() 실패, 오류 코드 %lu", GetLastError());
            ok = false;
        }
    }

    // 6) 콘솔 명령 기다리기 : quit = 종료, status = 상태 보기
    //    입력이 끝나도(Ctrl+Z 후 Enter) quit으로 본다
    if (ok) {
        Log("서버 시작 : 포트 %d, 월드 시드 %d, 정원 %d명 (명령 : quit, status)", SERVER_PORT, seed, MAX_PLAYERS);

        std::string command;
        while (std::getline(std::cin, command) && command != "quit") {
            if (command == "status")
                PrintStatus();
            else if (!command.empty())
                Log("알 수 없는 명령 : %s (quit, status)", command.c_str());
        }
    }

    // 7) 종료 : 종료 이벤트를 켜고 모든 스레드가 스스로 끝나길 기다린다
    //    Accept · Logic을 먼저 기다린다. 둘이 끝나면 세션 표를 바꾸는 스레드가 없으므로
    //    그다음에 남은 Recv · Send 핸들을 모아서 기다린다.
    //    (Send가 받지 못하는 클라이언트에게 send() 중이면 SO_SNDTIMEO(5분)까지 늦어질 수 있다)
    Log("서버 종료 중...");
    if (g_hQuitEvent != nullptr)
        SetEvent(g_hQuitEvent);

    HANDLE threads[MAX_PLAYERS * 2];
    DWORD count = 0;
    if (hAccept != nullptr) threads[count++] = hAccept;
    if (hLogic != nullptr)  threads[count++] = hLogic;
    if (count > 0)
        WaitForMultipleObjects(count, threads, TRUE, INFINITE);

    count = 0;
    EnterCriticalSection(&g_sessionCS);
    for (const Session& s : g_sessions) {
        if (s.hRecvThread != nullptr) threads[count++] = s.hRecvThread;
        if (s.hSendThread != nullptr) threads[count++] = s.hSendThread;
    }
    LeaveCriticalSection(&g_sessionCS);
    if (count > 0)
        WaitForMultipleObjects(count, threads, TRUE, INFINITE);

    // 8) 정리 : 남은 스레드가 없으므로 잠금 없이 닫는다
    //    CleanupClients()가 아직 회수하지 못한 세션도 여기서 닫는다
    for (Session& s : g_sessions) {
        if (s.sock != INVALID_SOCKET)  closesocket(s.sock);
        if (s.hRecvThread != nullptr)  CloseHandle(s.hRecvThread);
        if (s.hSendThread != nullptr)  CloseHandle(s.hSendThread);
        if (s.hSendEvent != nullptr)   CloseHandle(s.hSendEvent);
        DeleteCriticalSection(&s.sendCS);
    }
    closesocket(listen_sock);
    if (hAccept != nullptr)      CloseHandle(hAccept);
    if (hLogic != nullptr)       CloseHandle(hLogic);
    if (g_hQuitEvent != nullptr) CloseHandle(g_hQuitEvent);

    Log("서버 종료");
    DeleteCriticalSection(&g_inQueueCS);
    DeleteCriticalSection(&g_sessionCS);
    DeleteCriticalSection(&g_logCS);    // Log()가 쓰므로 맨 마지막
    WSACleanup();
    return ok ? 0 : 1;
}


// ---------------------------------------------------------------------------
// Log : printf 형식으로 한 줄 출력한다 (모든 스레드)
// ---------------------------------------------------------------------------
// 줄은 잠금 밖에서 만들고 출력만 g_logCS 안에서 해서, 여러 스레드의 로그가 한 줄에 섞이지 않게 한다.
// 앞에 시각(시:분:초.밀리초)을 붙이고 끝에 줄바꿈을 붙인다. fmt에 '\n'을 넣지 않는다.
void Log(const char* fmt, ...)
{
    SYSTEMTIME now;
    GetLocalTime(&now);

    char line[512];
    const int prefix = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] ",
                                     now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line + prefix, sizeof(line) - prefix, fmt, args);
    va_end(args);

    EnterCriticalSection(&g_logCS);
    std::puts(line);
    std::fflush(stdout);
    LeaveCriticalSection(&g_logCS);
}


// ---------------------------------------------------------------------------
// PrintStatus : status 명령. 서버 상태를 출력한다 (main 스레드)
// ---------------------------------------------------------------------------
// 틱 번호, 틱 처리 평균 · 최대 시간, 입력 큐 길이, 세션별 상태와 송신 대기 바이트
// 잠금을 겹쳐 잡지 않도록 자료마다 따로 잠가 값만 복사하고, 출력은 잠금 밖에서 한다.
void PrintStatus()
{
    static const char* const STATE_NAMES[] = { "FREE", "JOINING", "PLAYING", "CLOSING" };

    // 1) 세션 표
    SessionState states[MAX_PLAYERS];
    uint32_t gens[MAX_PLAYERS];
    EnterCriticalSection(&g_sessionCS);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        states[i] = g_sessions[i].state;
        gens[i] = g_sessions[i].gen;
    }
    LeaveCriticalSection(&g_sessionCS);

    // 2) 세션마다 송신 대기 바이트
    size_t pending[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        EnterCriticalSection(&g_sessions[i].sendCS);
        pending[i] = g_sessions[i].sendBuf.size();
        LeaveCriticalSection(&g_sessions[i].sendCS);
    }

    // 3) 입력 큐 길이
    EnterCriticalSection(&g_inQueueCS);
    const size_t queued = g_inQueue.size();
    LeaveCriticalSection(&g_inQueueCS);

    // 4) 출력
    Log("틱 %u, 틱 처리 평균 %.2fms / 최대 %.2fms, 입력 큐 %zu건",
        g_tickStats.tick.load(), g_tickStats.avgUs.load() / 1000.0, g_tickStats.maxUs.load() / 1000.0, queued);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const bool closing = (states[i] != SessionState::FREE) && g_sessions[i].closing.load();
        Log("  슬롯 %d : %-7s 세대 %u, 송신 대기 %zu바이트%s",
            i, STATE_NAMES[static_cast<int>(states[i])], gens[i], pending[i], closing ? " (끊는 중)" : "");
    }
}
