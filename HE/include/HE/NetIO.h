// DAZG-Orbit Project Source File
// Component: HE/include/HE/NetIO.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#pragma once
namespace HE {

class HEIO {
private:
    int sockfd;  // 套接字文件描述符
    struct sockaddr_in address;
    bool is_server;
    int counter = 0;

public:
    HEIO(const char* ip, int port, bool server = false);
    ~HEIO();

    void send_data(const void* data, int nbyte);
    void recv_data(void* data, int nbyte);
};

} // namespace HE
