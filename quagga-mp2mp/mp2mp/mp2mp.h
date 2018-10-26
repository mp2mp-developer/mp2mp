#ifndef __Mp2mp_HH__
#define __Mp2mp_HH__

#include "uscb.h"
#include "dscb.h"
#include "lsp.h"

class Mp2mp {
public:
    Mp2mp();
    ~Mp2mp();

    Uscb up_uscb;
    Uscb down_uscb;
    Dscb up_dscb;
    Dscb down_dscb;

    Lsp lsp;

    std::string role;
    int port;
    unsigned long root_ip;
};

#endif
