// 세션 표 · 송신 버퍼
//
// 세션 하나 = 접속 한 건의 수명. OpenSession()이 열고 CleanupClients()가 닫는다.
// Queue…는 송신 버퍼에 넣기만 하고 실제 send()는 그 세션의 Send 스레드가 한다.
// g_sessionCS와 sendCS는 겹쳐 잡지 않는다.
// ============================================================================

#include "server.h"


// ---------------------------------------------------------------------------
// 공유 데이터 : 세션 표
// ---------------------------------------------------------------------------
Session          g_sessions[MAX_PLAYERS];
CRITICAL_SECTION g_sessionCS;


// ---------------------------------------------------------------------------
// FindEmptySlot : g_sessions에서 FREE인 첫 슬롯 번호를 찾는다
// ---------------------------------------------------------------------------
// 반드시 g_sessionCS 안에서 부른다. OpenSession()이 같은 임계 영역 안에서 바로 등록하므로
// 두 명이 동시에 접속해도 같은 슬롯을 받지 않는다.
// 정리 중(CLOSING)인 슬롯은 FREE가 아니므로 다시 주지 않는다.
// 찾기만 하고 g_sessions는 바꾸지 않는다.
// 반환 : 0 ~ MAX_PLAYERS - 1 = 빈 슬롯 번호, -1 = 빈 슬롯 없음 (정원 초과)
int FindEmptySlot()
{
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (g_sessions[i].state == SessionState::FREE)
            return i;
    }
    return -1;
}


// ---------------------------------------------------------------------------
// OpenSession : 새 접속을 빈 슬롯에 JOINING으로 등록하고 Recv · Send 스레드를 만든다 (Accept 스레드)
// ---------------------------------------------------------------------------
// sock : accept()한 소켓. 등록하면 이 세션의 것이 되고, 닫는 곳은 CleanupClients()다
// ip   : accept()가 알려 준 상대 IPv4 주소 문자열. 세션 표에 남겨 재접속 위치를 찾는 열쇠로 쓴다
// 아직 다른 플레이어에게 알리지 않는다. 알리는 것은 CS_HELLO를 받은 Logic이 한다 (HandleHello).
// 반환 : 0 ~ MAX_PLAYERS - 1 = 등록한 슬롯 번호
//        -1 = 빈 슬롯 없음. sock은 그대로 (부른 쪽이 RejectConnection)
//        -2 = 스레드를 만들지 못함. sock은 여기서 닫았고 슬롯은 FREE로 되돌림
int OpenSession(SOCKET sock, const char* ip)
{
    // 1) 빈 슬롯을 찾아 JOINING으로 등록. 찾기와 등록을 한 임계 영역 안에서 한다
    EnterCriticalSection(&g_sessionCS);
    const int id = FindEmptySlot();
    if (id == -1) {
        LeaveCriticalSection(&g_sessionCS);
        return -1;
    }
    Session& s = g_sessions[id];
    s.state = SessionState::JOINING;
    s.sock = sock;
    strncpy_s(s.ip, sizeof(s.ip), ip, _TRUNCATE);   // 넘치면 자른다 (IPv4 문자열은 최대 15자라 잘리지 않는다)
    ++s.gen;
    s.closing = false;
    LeaveCriticalSection(&g_sessionCS);

    // 2) 송신 버퍼 비우기 : sendCS는 g_sessionCS를 놓은 뒤에 잡는다
    //    JOINING이고 스레드도 아직 없으므로 이 사이에 이 버퍼에 넣는 스레드는 없다
    EnterCriticalSection(&s.sendCS);
    s.sendBuf.clear();
    LeaveCriticalSection(&s.sendCS);

    // 3) Recv · Send 스레드 만들기
    //    멈춘 채(CREATE_SUSPENDED) 만들어 핸들을 세션 표에 적은 뒤에 출발시킨다.
    //    바로 출발시키면 접속이 곧바로 끊겼을 때, 핸들을 적기 전에 Logic이 이 세션을 회수하려 할 수 있다.
    LPVOID arg = reinterpret_cast<LPVOID>(static_cast<intptr_t>(id));
    HANDLE hRecv = CreateThread(nullptr, 0, RecvThread, arg, CREATE_SUSPENDED, nullptr);
    HANDLE hSend = (hRecv != nullptr) ? CreateThread(nullptr, 0, SendThread, arg, CREATE_SUSPENDED, nullptr) : nullptr;

    if (hRecv == nullptr || hSend == nullptr) {
        Log("CreateThread() 실패, 오류 코드 %lu : 슬롯 %d 연결을 닫음", GetLastError(), id);

        // 만든 스레드는 closing을 켠 뒤 출발시켜 스스로 끝나게 하고 기다린다
        if (hRecv != nullptr) {
            RequestDisconnect(id);
            ResumeThread(hRecv);
            WaitForSingleObject(hRecv, INFINITE);
            CloseHandle(hRecv);
        }
        closesocket(sock);

        // 슬롯을 FREE로 되돌린다. 세대 번호를 한 번 더 올려서,
        // 끝난 Recv가 넣었을 수 있는 퇴장 입력은 Logic이 세대가 달라 버리게 한다
        EnterCriticalSection(&g_sessionCS);
        s.state = SessionState::FREE;
        s.sock = INVALID_SOCKET;
        ++s.gen;
        LeaveCriticalSection(&g_sessionCS);
        return -2;
    }

    EnterCriticalSection(&g_sessionCS);
    s.hRecvThread = hRecv;
    s.hSendThread = hSend;
    LeaveCriticalSection(&g_sessionCS);

    ResumeThread(hRecv);
    ResumeThread(hSend);
    return id;
}


// ---------------------------------------------------------------------------
// QueuePacket : 패킷 하나를 그 세션의 송신 버퍼 끝에 붙이고 Send 스레드를 깨운다 (Logic 스레드, sendCS 안)
// ---------------------------------------------------------------------------
// packet : 패킷 구조체의 주소 (&p). 맨 앞 헤더의 size만큼 붙인다.
// send()는 하지 않는다. Logic 한 스레드가 처리한 순서대로 붙이므로 모든 클라이언트가 같은 순서로 받는다.
// 끊는 중(closing)인 세션에는 더 붙이지 않는다.
// 송신 대기가 1MB(MAX_SEND_BUFFER)를 넘으면 받지 못하는 클라이언트로 보고 RequestDisconnect()를 부른다.
void QueuePacket(int id, const void* packet)
{
    PacketHeader header;
    memcpy(&header, packet, sizeof(header));
    const int size = header.size;

    // 헤더가 잘못된 패킷을 붙이면 받는 쪽의 경계가 깨지므로 붙이지 않는다
    if (size < static_cast<int>(sizeof(PacketHeader)) || size > MAX_PACKET_SIZE) {
        Log("QueuePacket() : 크기가 잘못된 패킷을 버림 (종류 %d, %d바이트)", header.type, size);
        return;
    }

    Session& s = g_sessions[id];
    if (s.closing.load())
        return;

    // 1) 잠금 안에서는 이어 붙이기만
    const char* bytes = static_cast<const char*>(packet);
    EnterCriticalSection(&s.sendCS);
    s.sendBuf.insert(s.sendBuf.end(), bytes, bytes + size);
    const bool overflow = s.sendBuf.size() > MAX_SEND_BUFFER;
    LeaveCriticalSection(&s.sendCS);

    // 2) 잠금 밖에서 Send 깨우기
    if (overflow) {
        Log("[끊기] id %d : 송신 대기가 %zu바이트를 넘음", id, MAX_SEND_BUFFER);
        RequestDisconnect(id);   // closing을 켜고 Send를 깨운다
        return;
    }
    SetEvent(s.hSendEvent);
}


// ---------------------------------------------------------------------------
// RequestDisconnect : 그 세션을 끝내라고 요청만 한다 (Recv · Send · Logic · Accept 스레드)
// ---------------------------------------------------------------------------
// closing을 켜고 Send 스레드를 깨운다. Send는 바로, Recv는 0.2초 안에 closing을 보고 끝난다.
// 다른 스레드가 쓰는 소켓을 닫지 않기 위해 여기서는 소켓을 닫지 않는다. 실제 정리는 CleanupClients().
void RequestDisconnect(int id)
{
    g_sessions[id].closing = true;
    SetEvent(g_sessions[id].hSendEvent);
}


// ===========================================================================
// 아래는 아직 구현하지 않은 함수다. 빌드가 되도록 빈 본문만 두었다.
// 설명은 설계서의 서버 함수 표를 따른다. 구현할 때 (void) 줄과 TODO를 지운다.
// ===========================================================================

// ---------------------------------------------------------------------------
// QueueToAll : 게임 중인 모든 세션에 같은 패킷을 붙인다 (Logic 스레드)
// ---------------------------------------------------------------------------
// except : 뺄 접속자 id. -1이면 전원에게 보낸다.
// 입퇴장 알림은 당사자를 빼고, 블록 변경은 전원에게 보낸다.
// g_sessionCS 안에서 PLAYING인 슬롯 번호만 복사하고, 잠금을 놓은 뒤 각각 QueuePacket()을 부른다 (잠금을 겹치지 않게).
void QueueToAll(const void* packet, int except)
{
    // TODO: 구현
    (void)packet;
    (void)except;
}

// ---------------------------------------------------------------------------
// CleanupClients : 끝난 세션을 회수한다 (Logic 스레드, 매 틱)
// ---------------------------------------------------------------------------
// CLOSING인 세션마다 WaitForMultipleObjects(2, {Recv, Send}, TRUE, 0)으로
// 두 스레드가 모두 끝났는지를 기다리지 않고 확인한다.
// 끝났으면 소켓과 스레드 핸들을 닫고(nullptr로) 송신 버퍼를 비운 뒤 슬롯을 FREE로 되돌린다.
// 송신 버퍼는 sendCS로 지키므로 g_sessionCS와 겹치지 않게 따로 잡는다.
// 소켓을 닫는 곳은 여기 한 곳뿐이라, 새 접속이 이전 스레드나 소켓과 섞이지 않는다.
void CleanupClients()
{
    // TODO: 구현
}
