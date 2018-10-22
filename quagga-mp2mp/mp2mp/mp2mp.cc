#include <iostream>
#include <gflags/gflags.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mp2mp.h"

DEFINE_string(root_ip, "192.168.56.102", "root ip");
DEFINE_string(role, "transit", "role of this mp2mp node");
DEFINE_int32(port, 15000, "port");

Mp2mp::Mp2mp() {
    std::cout << "mp2mp()" << std::endl;
    root_ip = inet_addr(FLAGS_root_ip.c_str());
    role = FLAGS_role;
    port = FLAGS_port;
}

Mp2mp::~Mp2mp() {
    std::cout << "~mp2mp()" << std::endl;
}

int main(int argc, char *argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    Mp2mp mp2mp;
    std::cout << mp2mp.root_ip << std::endl;
    std::cout << mp2mp.role << std::endl;
    std::cout << mp2mp.port << std::endl;
    std::cout << "end" << std::endl;
}
