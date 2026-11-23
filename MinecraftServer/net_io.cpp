// 소켓을 다루는 스레드 (Accept · Recv · Send)
//
// Accept : 새 접속을 받아 세션을 연다 (v.3의 main()에서 옮겨 옴)
// Recv   : 받은 패킷을 입력 큐에 넣기만 한다. 게임 상태를 만지지 않고 send()도 하지 않는다
// Send   : 송신 버퍼를 통째로 꺼내 잠금 밖에서 send()
// 한 소켓에 recv()하는 스레드와 send()하는 스레드는 각각 하나뿐이다.
// ============================================================================

#include "server.h"
#include <ws2tcpip.h>   // inet_ntop · INET_ADDRSTRLEN (AcceptThread)


// ---------------------------------------------------------------------------
// 공유 데이터 : 입력 큐 (Recv -> Logic)
// ---------------------------------------------------------------------------
std::vector<InputMsg> g_inQueue;
CRITICAL_SECTION      g_inQueueCS;


// ---------------------------------------------------------------------------
// AcceptThread : 새 접속을 받아 빈 슬롯에 등록한다 (Accept 스레드, 1개)
// ---------------------------------------------------------------------------
// arg = 리슨 소켓 (main()이 만들고, 이 스레드가 끝난 뒤 main()이 닫는다)
// 흐름 (설계안 그림 1 (가))
//   새 접속 기다리기 select() 0.2초 -> 종료 신호? -> 새 접속 있음? -> accept() · 소켓 옵션
//   -> OpenSession() : 빈 슬롯이 있으면 JOINING으로 등록하고 Recv · Send를 만든다. 없으면 RejectConnection()
// accept()에서 계속 막혀 있으면 종료 신호를 볼 수 없으므로 select()로 0.2초씩만 기다리고, 그 사이마다 종료 이벤트를 본다.
// 그래서 다른 스레드가 리슨 소켓을 닫지 않아도 이 스레드를 끝낼 수 있다.
// 다른 플레이어에게는 알리지 않는다. 입장 처리는 CS_HELLO를 받은 Logic이 한다 (HandleHello).
DWORD WINAPI AcceptThread(LPVOID arg)
{
    const SOCKET listen_sock = static_cast<SOCKET>(reinterpret_cast<UINT_PTR>(arg));

    while (true) {
        // 1) 새 접속 기다리기 (최대 0.2초)
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(listen_sock, &readset);
        timeval timeout = { 0, SELECT_TIMEOUT_MS * 1000 };

        const int ready = select(0, &readset, nullptr, nullptr, &timeout);
        const int select_error = (ready == SOCKET_ERROR) ? WSAGetLastError() : 0;   // 다른 함수를 부르기 전에 읽어 둔다

        // 2) 종료 신호? : 켜졌으면 새 접속이 와 있어도 받지 않고 끝난다
        if (WaitForSingleObject(g_hQuitEvent, 0) != WAIT_TIMEOUT)
            break;

        if (ready == SOCKET_ERROR) {
            Log("select() 실패, 오류 코드 %d", select_error);
            WaitForSingleObject(g_hQuitEvent, SELECT_TIMEOUT_MS);   // 같은 오류로 쉬지 않고 돌지 않게 잠깐 쉰다
            continue;
        }

        // 3) 새 접속 있음?
        if (ready == 0)
            continue;   // 0.2초 동안 새 접속 없음 -> 다시 기다린다

        // 4) 접속 받기 : 상대 IP 주소와 포트도 함께 받는다
        sockaddr_in clientaddr = {};
        int addrlen = sizeof(clientaddr);
        SOCKET client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&clientaddr), &addrlen);
        if (client_sock == INVALID_SOCKET) {
            // 받기 전에 상대가 먼저 끊은 경우(WSAECONNRESET) 등. 이 접속만 버리고 다음 접속을 기다린다
            Log("accept() 실패, 오류 코드 %d", WSAGetLastError());
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientaddr.sin_addr, ip, sizeof(ip));
        const int port = ntohs(clientaddr.sin_port);

        // 5) 소켓 옵션 : 실패해도 연결은 쓸 수 있으므로 출력만 하고 계속한다
        //    TCP_NODELAY : 네이글 알고리즘을 끈다. 50ms마다 나가는 작은 스냅샷이 모였다가 늦게 가지 않게 한다
        //    SO_SNDTIMEO : 5분(SEND_TIMEOUT_MS). 네트워크가 비정상적으로 끊겨 send()가 5분 넘게 끝나지 않으면 실패로 돌아오게 한다.
        //                  무응답 연결을 끊는 기준은 이것 하나다 (Send 스레드가 실패를 보고 종료를 요청)
        BOOL nodelay = TRUE;
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)) == SOCKET_ERROR) {
            Log("setsockopt(TCP_NODELAY) 실패, 오류 코드 %d (계속 진행)", WSAGetLastError());
        }

        DWORD send_timeout = SEND_TIMEOUT_MS;
        if (setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout)) == SOCKET_ERROR) {
            Log("setsockopt(SO_SNDTIMEO) 실패, 오류 코드 %d (계속 진행)", WSAGetLastError());
        }

        // 6) 세션 열기 : 빈 슬롯 있음? -> JOINING으로 등록 -> Recv · Send 생성
        //    빈 슬롯 찾기와 등록은 OpenSession()이 g_sessionCS 안에서 한 번에 한다 (두 명이 같은 슬롯을 받지 않게).
        //    IP는 세션 표에 남겨 재접속 위치를 찾는 열쇠로 쓴다
        const int id = OpenSession(client_sock, ip);
        if (id >= 0) {
            Log("[접속] %s:%d -> id %d", ip, port, id);
        }
        else if (id == -1) {
            // 빈 슬롯 없음 : 정원 초과를 알리고 닫는다 (RejectConnection은 최대 1초 걸린다)
            Log("[접속 거절] %s:%d : 정원(%d명) 초과", ip, port, MAX_PLAYERS);
            RejectConnection(client_sock, JOIN_REJECT_FULL);
        }
        // id == -2 : Recv · Send 스레드를 만들지 못함. OpenSession()이 소켓을 닫고 이유를 출력했다
    }

    return 0;
}


// ---------------------------------------------------------------------------
// RejectConnection : 세션을 만들지 못한 접속에 거절 이유를 알리고 닫는다 (Accept 스레드)
// ---------------------------------------------------------------------------
// reason : JoinRejectReason 값 (여기서는 JOIN_REJECT_FULL만. 버전 거절은 Logic이 HandleHello에서 한다)
// 세션이 되기 전의 소켓이라 Send 스레드가 없으므로 여기서만 직접 보낸다.
// v.3는 말없이 닫아서 클라이언트가 이유를 알 수 없었다.
void RejectConnection(SOCKET sock, uint8_t reason)
{
    // 1) SC_JOIN_REJECT 하나 보내고 보내기 방향만 닫는다 (FIN)
    SC_JOIN_REJECT_PACKET reject;
    reject.reason = reason;
    SendPacket(sock, &reject);   // 실패해도 닫는 것은 같다
    shutdown(sock, SD_SEND);

    // 2) 클라이언트가 닫을 때까지 받은 것을 읽어서 버린다 (최대 1초)
    //    클라이언트가 먼저 보낸 CS_HELLO를 읽지 않은 채 닫으면 FIN 대신 RST가 나가서,
    //    클라이언트가 SC_JOIN_REJECT를 읽기 전에 연결 오류만 받을 수 있기 때문이다.
    //    거절을 읽은 클라이언트가 닫으면 recv()가 0을 돌려준다.
    const ULONGLONG deadline = GetTickCount64() + 1000;
    char discard[64];
    while (GetTickCount64() < deadline) {
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(sock, &readset);
        timeval timeout = { 0, SELECT_TIMEOUT_MS * 1000 };

        const int ready = select(0, &readset, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR)
            break;
        if (ready > 0 && recv(sock, discard, sizeof(discard), 0) <= 0)
            break;   // 클라이언트가 닫았거나 끊김
    }

    // 3) 닫기
    closesocket(sock);
}


// ---------------------------------------------------------------------------
// PushInput : 입력 한 건을 입력 큐 끝에 넣는다 (Recv 스레드, g_inQueueCS 안)
// ---------------------------------------------------------------------------
// id     : 슬롯 번호 (= 플레이어 id). RecvThread가 자기 슬롯 번호를 넘긴다
// gen    : RecvThread가 시작할 때 g_sessionCS 안에서 꺼내 둔 세대 번호.
//          Logic(ProcessInput)이 지금 슬롯의 세대 번호와 비교해서, 이미 정리된 세션이 늦게 넣은 입력을 버린다
// kind   : PACKET = 받은 패킷 하나, LEAVE = 연결이 끝남 (퇴장도 패킷과 같은 길로 Logic에 전달한다)
// packet : PACKET이면 ExtractPacket()이 꺼낸 패킷 하나 (헤더부터). 헤더의 size만큼 복사한다. LEAVE면 nullptr
//
// 입력 한 건(InputMsg)은 잠금 밖에서 다 만들고, 잠금 안에서는 push_back 한 번만 한다.
// Recv 3개와 Logic이 같은 g_inQueueCS를 잡으므로 잡는 시간을 짧게 한다.
// 패킷의 종류와 크기가 맞는지는 Logic(ProcessInput)이 본다. 여기서는 복사할 길이가 말이 되는지만 본다.
// LEAVE는 버리는 경우가 없다. 버리면 Logic이 그 세션을 CLOSING으로 바꾸지 못해 슬롯이 돌아오지 않는다.
void PushInput(int id, uint32_t gen, InputKind kind, const char* packet)
{
    // 1) 잠금 밖에서 입력 한 건을 만든다
    InputMsg msg;
    msg.id = id;
    msg.gen = gen;
    msg.kind = kind;

    if (kind == InputKind::PACKET) {
        if (packet == nullptr) {
            Log("PushInput() : 패킷 없이 PACKET 입력을 넣으려 함 (id %d)", id);
            return;
        }

        // 헤더의 size만큼 복사한다. ExtractPacket()이 이미 걸렀지만,
        // 잘못된 size로 msg.packet 밖을 덮어쓰지 않게 여기서도 한 번 더 본다
        PacketHeader header;
        memcpy(&header, packet, sizeof(header));
        const int size = header.size;
        if (size < static_cast<int>(sizeof(PacketHeader)) || size > MAX_PACKET_SIZE) {
            Log("PushInput() : 크기가 잘못된 패킷을 버림 (id %d, 종류 %d, %d바이트)", id, header.type, size);
            return;
        }
        memcpy(msg.packet, packet, size);   // 나머지 바이트는 0으로 남는다
    }

    // 2) 잠금 안에서는 push_back 한 번만
    EnterCriticalSection(&g_inQueueCS);
    g_inQueue.push_back(msg);
    LeaveCriticalSection(&g_inQueueCS);
}


// ===========================================================================
// 아래는 아직 구현하지 않은 함수다. 빌드가 되도록 빈 본문만 두었다.
// 설명은 설계서의 서버 함수 표를 따른다. 구현할 때 (void) 줄과 TODO를 지운다.
// ===========================================================================

// ---------------------------------------------------------------------------
// RecvThread : 자기 클라이언트의 패킷을 받아 입력 큐에 넣는다 (Recv 스레드, 세션마다 1개)
// ---------------------------------------------------------------------------
// arg = 슬롯 id. 시작할 때 g_sessionCS 안에서 소켓과 세대 번호를 꺼낸다.
// select()를 0.2초씩 걸고, 그 사이마다 종료 이벤트와 그 세션의 closing을 확인한다.
// 무응답 시간은 따로 재지 않는다 (무응답은 SO_SNDTIMEO 5분으로 Send 쪽에서 잡는다).
// 읽을 것이 있으면 recv() -> ExtractPacket() -> PushInput(PACKET). CS_PING은 큐에 넣지 않고 버린다.
// recv() 0 이하 · 패킷 경계가 깨짐 · 종료 신호 · closing이면 반복을 끝내고
// PushInput(LEAVE)와 RequestDisconnect()를 부른 뒤 끝난다.
// 게임 상태를 만지지 않고 send()도 하지 않는다. 소켓도 닫지 않는다 (CleanupClients).
// 받기 반복은 v.3의 ClientThread()와 같은 모양이다.
// protocol.h(V2)의 ExtractPacket()은 bool이라 깨진 경우를 따로 알려 주지 않는다 (깨지면 len을 0으로 만든다).
DWORD WINAPI RecvThread(LPVOID arg)
{
    // TODO: 구현
    (void)arg;
    return 0;
}

// ---------------------------------------------------------------------------
// SendThread : 자기 세션의 송신 버퍼를 꺼내 보낸다 (Send 스레드, 세션마다 1개)
// ---------------------------------------------------------------------------
// arg = 슬롯 id. 시작할 때 g_sessionCS 안에서 소켓을 꺼낸다.
// WaitForMultipleObjects()로 종료 이벤트와 자기 세션의 송신 이벤트(hSendEvent)를 기다린다.
// 깨어나면 sendCS 안에서 송신 버퍼를 지역 버퍼와 swap하고, 잠금 밖에서 버퍼 전체를 보낸다.
// 이 클라이언트가 받지 못해 send()가 막혀도 기다리는 것은 이 스레드 하나뿐이다.
// 보내기 실패(SO_SNDTIMEO 5분 시간 초과 포함)나 closing이면 RequestDisconnect()를 부르고 끝난다.
// closing이어도 버퍼에 남은 것(버전 거절 SC_JOIN_REJECT 등)은 먼저 보내고 끝난다.
// 설계안의 SendAll()(버퍼 단위 전송)은 protocol.h(V2)에 아직 없다. 지금 있는 SendPacket()은 패킷 하나 단위다.
DWORD WINAPI SendThread(LPVOID arg)
{
    // TODO: 구현
    (void)arg;
    return 0;
}
