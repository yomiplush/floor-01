#include "lanremote.h"
#include <iostream>

int main() {
    const char* accepted[]={"127.0.0.1","10.0.0.2","172.16.0.2","172.31.255.254",
        "192.168.1.2","169.254.1.2","192.0.0.0","192.0.0.1","192.0.0.2","192.0.0.7"};
    const char* rejected[]={"0.0.0.0","8.8.8.8","172.15.255.255","172.32.0.1",
        "192.0.0.8","192.0.0.9","192.0.0.10","192.0.0.255","192.0.1.2",
        "255.255.255.255","::1","::ffff:192.0.0.2"};
    for(auto ip:accepted)if(!LanRemote::localAddress(QHostAddress(ip))){std::cerr<<"Rejected "<<ip<<'\n';return 1;}
    for(auto ip:rejected)if(LanRemote::localAddress(QHostAddress(ip))){std::cerr<<"Accepted "<<ip<<'\n';return 1;}
    const char* global6[]={"2001:db8::1","2001:db8:abcd:ef01:0:1:2:3","2606:4700::1111"};
    const char* local6[]={"::1","fe80::1","fc00::1","fd12:3456:789a::1","ff02::1","::ffff:192.0.0.2"};
    for(auto ip:global6)if(!LanRemote::ipv6Global(QHostAddress(ip))){std::cerr<<"V6 rejected "<<ip<<'\n';return 1;}
    for(auto ip:local6)if(LanRemote::ipv6Global(QHostAddress(ip))){std::cerr<<"V6 accepted "<<ip<<'\n';return 1;}
    for(auto ip:global6)if(!LanRemote::remoteCandidate(QHostAddress(ip))){std::cerr<<"Candidate rejected "<<ip<<'\n';return 1;}
    for(auto ip:local6)if(LanRemote::remoteCandidate(QHostAddress(ip))){std::cerr<<"Candidate accepted "<<ip<<'\n';return 1;}
    if(LanRemote::formatHost(QHostAddress("2001:db8:abcd:1::1"))!="[2001:db8:abcd:1::1]"){std::cerr<<"formatHost v6\n";return 1;}
    if(LanRemote::formatHost(QHostAddress("192.168.1.2"))!="192.168.1.2"){std::cerr<<"formatHost v4\n";return 1;}
    std::cout<<"PASS: tethering range boundaries, IPv6 candidate selection, host formatting\n";
}
